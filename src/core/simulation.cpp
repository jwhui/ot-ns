/*
 *  Copyright (c) 2026, The OTNS Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#include "core/simulation.hpp"

#include <openthread/error.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <thread>

#include "core/wpan_dissector.hpp"
#include "nodes/inproc_node.hpp"

namespace otns {
namespace Core {

namespace {

static constexpr uint64_t kUsPerByte250Kbps = 32;    // 250 kbps => 4 us/bit => 32 us/byte
static constexpr uint64_t kCcaDurationUs    = 128;   // 8 symbols * 16 us
static constexpr uint64_t kAckTurnaroundUs  = 192;   // aTurnaroundTime = 12 symbols * 16 us
static constexpr uint64_t kAckWaitTimeoutUs = 1000;  // macAckWaitDuration
static constexpr uint64_t kPacedSliceUs     = 10000; // 10 ms virtual slice for real-time pacing

} // namespace

thread_local Simulation *Simulation::sCurrentSim = nullptr;

Simulation::Simulation(const SimulationConfig &aConfig)
    : mConfig(aConfig)
    , mRadioModel(aConfig.mRfPreset, aConfig.mSeed)
    , mPlatformPrng(aConfig.mSeed ^ 0xA5A55A5AF0F00F0FULL)
{
}

Simulation::~Simulation(void)
{
    ScopedCurrentSim guard(this);

    mPcapWriter.Close();
    mNodes.clear();
    mNodeConfigs.clear();
    mScheduler.Clear();
    mNetFabric.Clear();
}

void Simulation::Reset(std::optional<uint64_t> aSeed)
{
    ScopedCurrentSim guard(this);

    // Destroy nodes BEFORE clearing the scheduler so node destructors canceling
    // old alarms do not pollute the reset scheduler's canceled-ID set.
    mNodes.clear();
    mNodeConfigs.clear();
    mScheduler.Clear();
    mNetFabric.Clear();
    mKpiTracker.Reset();
    mNextNodeId         = 1;
    mHasPendingTasklets = false;
    mStatTxFrames       = 0;
    mStatRxOk           = 0;
    mStatRxFcsErr       = 0;

    if (aSeed.has_value())
    {
        mConfig.mSeed = *aSeed;
    }

    mRadioModel.Clear(mConfig.mSeed);
    mPlatformPrng.Seed(mConfig.mSeed ^ 0xA5A55A5AF0F00F0FULL);
}

void Simulation::SetRfPreset(Rf::RadioModelPreset aPreset)
{
    mConfig.mRfPreset = aPreset;
    mRadioModel.SetPreset(aPreset);
}

Rf::NodeId Simulation::AllocateNodeId(Rf::NodeId aPreferredId)
{
    if (aPreferredId != Rf::kInvalidNodeId && mNodes.find(aPreferredId) == mNodes.end())
    {
        if (aPreferredId >= mNextNodeId)
        {
            mNextNodeId = aPreferredId + 1;
        }
        return aPreferredId;
    }

    while (mNodes.find(mNextNodeId) != mNodes.end())
    {
        mNextNodeId += 1;
    }

    return mNextNodeId++;
}

Rf::NodeId Simulation::AddNode(const NodeConfig &aConfig)
{
    ScopedCurrentSim   guard(this);
    Rf::NodeId         id = AllocateNodeId();
    Rf::RadioNodeState radioNode;

    radioNode.mId               = id;
    radioNode.mPos              = aConfig.mPos;
    radioNode.mRadioRange       = aConfig.mRadioRange;
    radioNode.mChannel          = mConfig.mChannel;
    radioNode.mTxPowerDbm       = Rf::kDefaultTxPowerDbm;
    radioNode.mRxSensitivityDbm = Rf::kDefaultRxSensitivity;
    radioNode.mState            = Rf::RadioState::kDisabled;

    mRadioModel.AddNode(radioNode);
    mNodeConfigs[id] = aConfig;

    auto node = std::make_unique<Nodes::InProcNode>(*this, id, aConfig);

    mNodes.emplace(id, std::move(node));
    DrainTasklets();

    return id;
}

Rf::NodeId Simulation::RegisterExternalNode(std::unique_ptr<SimNode> aNode, const NodeConfig &aConfig)
{
    ScopedCurrentSim guard(this);

    if (!aNode)
    {
        return Rf::kInvalidNodeId;
    }

    Rf::NodeId         id = aNode->GetId();
    Rf::RadioNodeState radioNode;

    if (id == Rf::kInvalidNodeId || mNodes.find(id) != mNodes.end())
    {
        return Rf::kInvalidNodeId;
    }

    if (id >= mNextNodeId)
    {
        mNextNodeId = id + 1;
    }

    radioNode.mId               = id;
    radioNode.mPos              = aConfig.mPos;
    radioNode.mRadioRange       = aConfig.mRadioRange;
    radioNode.mChannel          = mConfig.mChannel;
    radioNode.mTxPowerDbm       = Rf::kDefaultTxPowerDbm;
    radioNode.mRxSensitivityDbm = Rf::kDefaultRxSensitivity;
    radioNode.mState            = Rf::RadioState::kDisabled;

    mRadioModel.AddNode(radioNode);
    mNodeConfigs[id] = aConfig;
    mNodes.emplace(id, std::move(aNode));
    DrainTasklets();

    return id;
}

bool Simulation::RemoveNode(Rf::NodeId aId)
{
    ScopedCurrentSim guard(this);
    auto             it = mNodes.find(aId);

    if (it == mNodes.end())
    {
        return false;
    }

    mNetFabric.RemoveNode(aId, MakeTrelPeerCallback());
    mRadioModel.RemoveNode(aId);
    mNodeConfigs.erase(aId);
    mNodes.erase(it);

    return true;
}

bool Simulation::MoveNode(Rf::NodeId aId, Rf::Position3D aPos)
{
    auto it = mNodes.find(aId);

    if (it == mNodes.end())
    {
        return false;
    }

    it->second->SetPosition(aPos);
    mRadioModel.SetNodePosition(aId, aPos);

    auto cit = mNodeConfigs.find(aId);

    if (cit != mNodeConfigs.end())
    {
        cit->second.mPos = aPos;
    }

    return true;
}

bool Simulation::SetNodeEnabled(Rf::NodeId aId, bool aEnabled)
{
    ScopedCurrentSim guard(this);
    auto             it = mNodes.find(aId);

    if (it == mNodes.end())
    {
        return false;
    }

    it->second->SetEnabled(aEnabled);
    if (!aEnabled)
    {
        if (const auto *rn = mRadioModel.GetNode(aId))
        {
            mRadioModel.SetNodeRadioState(aId, Rf::RadioState::kDisabled, rn->mChannel);
        }
    }

    return true;
}

std::string Simulation::ExecuteCli(Rf::NodeId aId, const std::string &aCommand)
{
    ScopedCurrentSim guard(this);
    auto             it = mNodes.find(aId);

    if (it == mNodes.end())
    {
        return "Error: node not found\n";
    }

    std::string out = it->second->ExecuteCli(aCommand);

    DrainTasklets();

    return out;
}

std::string Simulation::TakeCliOutput(Rf::NodeId aId)
{
    auto it = mNodes.find(aId);

    if (it == mNodes.end())
    {
        return "";
    }

    return it->second->TakeCliOutput();
}

std::optional<NodeSnapshot> Simulation::GetNodeSnapshot(Rf::NodeId aId) const
{
    auto it = mNodes.find(aId);

    if (it == mNodes.end())
    {
        return std::nullopt;
    }

    return it->second->GetSnapshot();
}

std::vector<NodeSnapshot> Simulation::GetAllNodeSnapshots(void) const
{
    std::vector<NodeSnapshot> snaps;

    snaps.reserve(mNodes.size());
    for (const auto &pair : mNodes)
    {
        snaps.push_back(pair.second->GetSnapshot());
    }

    return snaps;
}

SimNode *Simulation::GetNode(Rf::NodeId aId)
{
    auto it = mNodes.find(aId);

    return (it != mNodes.end()) ? it->second.get() : nullptr;
}

void Simulation::DrainTasklets(void)
{
    while (mHasPendingTasklets)
    {
        mHasPendingTasklets = false;
        for (auto &pair : mNodes)
        {
            if (pair.second->IsEnabled() && pair.second->HasPendingTasklets())
            {
                pair.second->ProcessTasklets();
            }
        }
    }
}

void Simulation::PollExternalNodes(void)
{
    ScopedCurrentSim guard(this);

    for (auto &pair : mNodes)
    {
        if (pair.second->IsEnabled())
        {
            pair.second->PollExternal();
        }
    }
    DrainTasklets();
}

void Simulation::AdvanceUs(uint64_t aDurationUs)
{
    ScopedCurrentSim guard(this);
    uint64_t         targetUs = mScheduler.GetNowUs() + aDurationUs;

    PollExternalNodes();
    DrainTasklets();

    while (true)
    {
        auto nextTs = mScheduler.GetNextEventTimeUs();

        if (!nextTs.has_value() || *nextTs > targetUs)
        {
            break;
        }

        mRadioModel.AdvanceTime(*nextTs);
        mScheduler.DispatchNextIfBeforeOrAt(targetUs);
        DrainTasklets();
    }

    mScheduler.SetNowUs(targetUs);
    mRadioModel.AdvanceTime(targetUs);
    DrainTasklets();

    for (const auto &pair : mNodes)
    {
        mKpiTracker.RecordRoleObservation(pair.first, pair.second->GetSnapshot().mRole);
    }
}

void Simulation::AdvanceSec(double aSeconds)
{
    if (aSeconds > 0.0)
    {
        uint64_t durUs = static_cast<uint64_t>(std::llround(aSeconds * 1e6));

        if (mConfig.mSpeed > 0.0)
        {
            AdvancePacedUs(durUs, mConfig.mSpeed);
        }
        else
        {
            AdvanceUs(durUs);
        }
    }
}

void Simulation::AdvancePacedUs(uint64_t aDurationUs, double aSpeed)
{
    if (aSpeed <= 0.0 || aDurationUs == 0)
    {
        AdvanceUs(aDurationUs);
        return;
    }

    auto     wallStart = std::chrono::steady_clock::now();
    uint64_t elapsed   = 0;

    while (elapsed < aDurationUs)
    {
        uint64_t stepUs = std::min(kPacedSliceUs, aDurationUs - elapsed);

        AdvanceUs(stepUs);
        elapsed += stepUs;

        double targetWallUs = static_cast<double>(elapsed) / aSpeed;
        auto   targetPoint  = wallStart + std::chrono::microseconds(static_cast<int64_t>(std::llround(targetWallUs)));
        auto   now          = std::chrono::steady_clock::now();

        if (targetPoint > now)
        {
            std::this_thread::sleep_until(targetPoint);
        }
    }
}

bool Simulation::StartPcapContained(const std::filesystem::path &aBaseDir,
                                    const std::string           &aRequestedPath,
                                    PcapFormat                   aFormat)
{
    return mPcapWriter.OpenContained(aBaseDir, aRequestedPath, aFormat);
}

void Simulation::StopPcap(void) { mPcapWriter.Close(); }

TopologySpec Simulation::ExportTopology(void) const
{
    TopologySpec spec;

    spec.mVersion     = 1;
    spec.mSeed        = mConfig.mSeed;
    spec.mRfPreset    = mConfig.mRfPreset;
    spec.mPanId       = mConfig.mPanId;
    spec.mChannel     = mConfig.mChannel;
    spec.mNetworkKey  = mConfig.mNetworkKey;
    spec.mRadioParams = mRadioModel.GetParams();

    for (const auto &pair : mNodes)
    {
        TopologyNodeSpec nodeSpec;
        NodeSnapshot     snap = pair.second->GetSnapshot();
        auto             cit  = mNodeConfigs.find(pair.first);

        nodeSpec.mId         = snap.mId;
        nodeSpec.mKind       = snap.mKind;
        nodeSpec.mPos        = snap.mPos;
        nodeSpec.mRadioRange = snap.mRadioRange;
        if (cit != mNodeConfigs.end())
        {
            nodeSpec.mType      = cit->second.mType;
            nodeSpec.mAutoStart = cit->second.mAutoStart;
        }
        spec.mNodes.push_back(std::move(nodeSpec));
    }

    return spec;
}

bool Simulation::LoadTopology(const TopologySpec &aSpec)
{
    ScopedCurrentSim guard(this);

    Reset(aSpec.mSeed);

    mConfig.mSeed       = aSpec.mSeed;
    mConfig.mRfPreset   = aSpec.mRfPreset;
    mConfig.mPanId      = aSpec.mPanId;
    mConfig.mChannel    = aSpec.mChannel;
    mConfig.mNetworkKey = aSpec.mNetworkKey;

    mRadioModel.SetPreset(aSpec.mRfPreset);
    if (aSpec.mRadioParams.has_value())
    {
        mRadioModel.SetParams(*aSpec.mRadioParams);
    }
    mPlatformPrng.Seed(aSpec.mSeed ^ 0xA5A55A5AF0F00F0FULL);

    for (const auto &ns : aSpec.mNodes)
    {
        if (ns.mKind != NodeKind::kInProc)
        {
            continue;
        }

        NodeConfig cfg;

        cfg.mType       = ns.mType;
        cfg.mPos        = ns.mPos;
        cfg.mRadioRange = ns.mRadioRange;
        cfg.mAutoStart  = ns.mAutoStart;

        Rf::NodeId         id = AllocateNodeId(ns.mId);
        Rf::RadioNodeState radioNode;

        radioNode.mId               = id;
        radioNode.mPos              = cfg.mPos;
        radioNode.mRadioRange       = cfg.mRadioRange;
        radioNode.mChannel          = mConfig.mChannel;
        radioNode.mTxPowerDbm       = Rf::kDefaultTxPowerDbm;
        radioNode.mRxSensitivityDbm = Rf::kDefaultRxSensitivity;
        radioNode.mState            = Rf::RadioState::kDisabled;

        mRadioModel.AddNode(radioNode);
        mNodeConfigs[id] = cfg;

        auto node = std::make_unique<Nodes::InProcNode>(*this, id, cfg);

        mNodes.emplace(id, std::move(node));
        DrainTasklets();

        for (const auto &cmd : ns.mInitCommands)
        {
            (void)ExecuteCli(id, cmd);
        }
    }

    return true;
}

NetFabric::TrelPeerCallback Simulation::MakeTrelPeerCallback(void)
{
    return [this](Rf::NodeId aTargetId, const TrelPeerRecord &aPeer, bool aRemoved) {
        mScheduler.ScheduleAfter(0, [this, aTargetId, aPeer, aRemoved](void) {
            auto it = mNodes.find(aTargetId);

            if (it != mNodes.end() && it->second->IsEnabled())
            {
                it->second->OnTrelPeerChange(aPeer, aRemoved);
            }
        });
    };
}

void Simulation::RegisterTrelPeer(Rf::NodeId aNodeId, uint16_t aUdpPort, const std::vector<uint8_t> &aTxtData)
{
    mNetFabric.RegisterTrelPeer(aNodeId, aUdpPort, aTxtData, MakeTrelPeerCallback());
}

void Simulation::UnregisterTrelPeer(Rf::NodeId aNodeId)
{
    mNetFabric.UnregisterTrelPeer(aNodeId, MakeTrelPeerCallback());
}

void Simulation::JoinInfraIf(Rf::NodeId aNodeId) { mNetFabric.JoinInfraIf(aNodeId); }

void Simulation::LeaveInfraIf(Rf::NodeId aNodeId) { mNetFabric.LeaveInfraIf(aNodeId); }

void Simulation::SendTrelPacket(const NetPacket &aPacket)
{
    auto target = mNetFabric.ResolveTrelTarget(aPacket.mDstIp6, aPacket.mDstPort);

    if (!target.has_value())
    {
        return;
    }

    Rf::NodeId dstId = *target;
    NetPacket  pkt   = aPacket;

    if (pkt.mSrcIp6 == Ip6Addr{})
    {
        pkt.mSrcIp6 = NetFabric::MakeNodeBackboneIp6(pkt.mSrcNodeId);
    }

    mScheduler.ScheduleAfter(mNetFabric.GetBackboneDelayUs(), [this, dstId, pkt = std::move(pkt)](void) {
        auto it = mNodes.find(dstId);

        if (it != mNodes.end() && it->second->IsEnabled())
        {
            it->second->OnTrelPacket(pkt);
        }
    });
}

void Simulation::SendInfraIfPacket(const NetPacket &aPacket)
{
    std::vector<Rf::NodeId> targets = mNetFabric.ResolveInfraIfTargets(aPacket.mSrcNodeId, aPacket.mDstIp6);
    NetPacket               basePkt = aPacket;

    if (basePkt.mSrcIp6 == Ip6Addr{})
    {
        basePkt.mSrcIp6 = NetFabric::MakeNodeLinkLocalIp6(basePkt.mSrcNodeId);
    }

    for (Rf::NodeId dstId : targets)
    {
        mScheduler.ScheduleAfter(mNetFabric.GetBackboneDelayUs(), [this, dstId, pkt = basePkt](void) {
            auto it = mNodes.find(dstId);

            if (it != mNodes.end() && it->second->IsEnabled())
            {
                it->second->OnInfraIfPacket(pkt);
            }
        });
    }
}

void Simulation::SendHostUdpPacket(const NetPacket &aPacket)
{
    std::vector<Rf::NodeId> targets =
        mNetFabric.ResolveHostUdpTargets(aPacket.mSrcNodeId, aPacket.mDstIp6, aPacket.mDstPort);
    NetPacket basePkt = aPacket;

    if (basePkt.mSrcIp6 == Ip6Addr{})
    {
        basePkt.mSrcIp6 = NetFabric::MakeNodeBackboneIp6(basePkt.mSrcNodeId);
    }

    for (Rf::NodeId dstId : targets)
    {
        mScheduler.ScheduleAfter(mNetFabric.GetBackboneDelayUs(), [this, dstId, pkt = basePkt](void) {
            auto it = mNodes.find(dstId);

            if (it != mNodes.end() && it->second->IsEnabled())
            {
                it->second->OnHostUdpPacket(pkt);
            }
        });
    }
}

SimulationStats Simulation::GetStats(void) const
{
    SimulationStats    stats;
    std::set<uint32_t> partitions;

    stats.mTxFrames = mStatTxFrames;
    stats.mRxOk     = mStatRxOk;
    stats.mRxFcsErr = mStatRxFcsErr;

    for (const auto &pair : mNodes)
    {
        NodeSnapshot snap = pair.second->GetSnapshot();

        stats.mTotal += 1;
        switch (snap.mRole)
        {
        case DeviceRole::kDisabled:
            stats.mDisabled += 1;
            break;
        case DeviceRole::kDetached:
            stats.mDetached += 1;
            break;
        case DeviceRole::kChild:
            stats.mChildren += 1;
            partitions.insert(snap.mPartitionId);
            break;
        case DeviceRole::kRouter:
            stats.mRouters += 1;
            partitions.insert(snap.mPartitionId);
            break;
        case DeviceRole::kLeader:
            stats.mLeaders += 1;
            partitions.insert(snap.mPartitionId);
            break;
        }
    }

    stats.mPartitions = static_cast<uint32_t>(partitions.size());

    return stats;
}

void Simulation::StartRadioTx(Rf::NodeId      aSrcId,
                              const RawFrame &aTxFrame,
                              bool            aAckRequested,
                              bool            aCsmaCaEnabled,
                              int8_t          aCcaThresholdDbm,
                              Rf::DbValue     aTxPowerDbm,
                              TxDoneCallback  aOnTxDone)
{
    uint64_t ccaDelay = aCsmaCaEnabled ? kCcaDurationUs : 0;

    mScheduler.ScheduleAfter(ccaDelay, [this, aSrcId, aTxFrame, aAckRequested, aCsmaCaEnabled, aCcaThresholdDbm,
                                        aTxPowerDbm, cb = std::move(aOnTxDone)](void) mutable {
        auto sit = mNodes.find(aSrcId);

        if (sit == mNodes.end() || !sit->second->IsEnabled())
        {
            if (cb)
            {
                cb(std::nullopt, static_cast<uint8_t>(OT_ERROR_ABORT));
            }
            return;
        }

        if (aCsmaCaEnabled)
        {
            Rf::DbValue chRssi = mRadioModel.SampleChannelRssi(aSrcId, aTxFrame.mChannel);

            if (chRssi >= static_cast<Rf::DbValue>(aCcaThresholdDbm))
            {
                mRadioModel.SetNodeRadioState(aSrcId, Rf::RadioState::kRx, aTxFrame.mChannel);
                mKpiTracker.RecordTxOutcome(aSrcId, aAckRequested, false, true);
                if (cb)
                {
                    cb(std::nullopt, static_cast<uint8_t>(OT_ERROR_CHANNEL_ACCESS_FAILURE));
                }
                return;
            }
        }

        uint64_t txDurUs  = (Rf::kPhyHeaderLenBytes + aTxFrame.mPsdu.size()) * kUsPerByte250Kbps;
        auto     startRes = mRadioModel.BeginTx(aSrcId, aTxFrame.mChannel, aTxPowerDbm, aTxFrame.mPsdu.size(), txDurUs);

        if (startRes.mAborted)
        {
            mRadioModel.SetNodeRadioState(aSrcId, Rf::RadioState::kRx, aTxFrame.mChannel);
            if (cb)
            {
                cb(std::nullopt, static_cast<uint8_t>(OT_ERROR_ABORT));
            }
            return;
        }

        mStatTxFrames += 1;

        RawFrame frameWithTs = aTxFrame;

        frameWithTs.mTimestampUs = mScheduler.GetNowUs();
        frameWithTs.mRssiDbm     = Rf::ClipRssiToInt8(aTxPowerDbm);

        MacFrameInfo macInfo =
            DissectWpanFrame(frameWithTs.mPsdu.data(), frameWithTs.mPsdu.size(), frameWithTs.mChannel);

        mKpiTracker.RecordTxStart(aSrcId, frameWithTs.mPsdu.size(), txDurUs, macInfo);
        if (mPcapWriter.IsOpen())
        {
            (void)mPcapWriter.AppendFrame(frameWithTs);
        }

        if (mPacketObserver)
        {
            PacketObservation obs;

            obs.mTimestampUs = frameWithTs.mTimestampUs;
            obs.mSrcId       = aSrcId;
            obs.mDstId       = Rf::kInvalidNodeId;
            obs.mChannel     = frameWithTs.mChannel;
            obs.mDurationUs  = txDurUs;
            obs.mRssiDbm     = frameWithTs.mRssiDbm;
            obs.mFrameLen    = static_cast<uint16_t>(frameWithTs.mPsdu.size());
            obs.mProtocol    = (macInfo.mFrameType == MacFrameType::kAck) ? "802.15.4-ACK" : "802.15.4";
            obs.mSummary     = macInfo.ToSummaryString();

            if (macInfo.mValid && !macInfo.IsBroadcast() && macInfo.mDstAddrMode == MacAddrMode::kShort)
            {
                for (const auto &pair : mNodes)
                {
                    if (pair.first != aSrcId && pair.second->GetSnapshot().mRloc16 == macInfo.mDstAddrShort)
                    {
                        obs.mDstId = pair.first;
                        break;
                    }
                }
            }

            mPacketObserver(obs);
        }

        mScheduler.ScheduleAfter(txDurUs, [this, aSrcId, frameWithTs = std::move(frameWithTs), aAckRequested,
                                           cb = std::move(cb)](void) mutable {
            CompleteRadioTx(aSrcId, frameWithTs, aAckRequested, std::move(cb));
        });
    });
}

void Simulation::CompleteRadioTx(Rf::NodeId      aSrcId,
                                 const RawFrame &aTxFrame,
                                 bool            aAckRequested,
                                 TxDoneCallback  aOnTxDone)
{
    auto       endRes      = mRadioModel.EndTx(aSrcId, aTxFrame.mChannel, aTxFrame.mPsdu.size());
    auto       sit         = mNodes.find(aSrcId);
    Rf::NodeId ackSenderId = Rf::kInvalidNodeId;
    RawFrame   ackFrame;

    if (sit != mNodes.end() && sit->second->IsEnabled())
    {
        mRadioModel.SetNodeRadioState(aSrcId, Rf::RadioState::kRx, aTxFrame.mChannel);
    }

    for (const auto &delivery : endRes.mDeliveries)
    {
        if (delivery.mStatus == Rf::RxResultStatus::kFcsError)
        {
            mStatRxFcsErr += 1;
            mKpiTracker.RecordRxOutcome(delivery.mNodeId, aTxFrame.mPsdu.size(), false);
            continue;
        }

        if (delivery.mStatus != Rf::RxResultStatus::kOk)
        {
            continue;
        }

        auto rit = mNodes.find(delivery.mNodeId);

        if (rit == mNodes.end() || !rit->second->IsEnabled())
        {
            continue;
        }

        mStatRxOk += 1;
        mKpiTracker.RecordRxOutcome(delivery.mNodeId, aTxFrame.mPsdu.size(), true);

        RawFrame rxCopy = aTxFrame;
        RawFrame candidateAck;

        rxCopy.mRssiDbm     = delivery.mRssiDbm;
        rxCopy.mLqi         = 255;
        rxCopy.mTimestampUs = aTxFrame.mTimestampUs;

        if (rit->second->OnRadioRxDone(rxCopy, candidateAck))
        {
            if (aAckRequested && ackSenderId == Rf::kInvalidNodeId && !candidateAck.mPsdu.empty())
            {
                ackSenderId = delivery.mNodeId;
                ackFrame    = std::move(candidateAck);
            }
        }
    }

    if (!aAckRequested)
    {
        mKpiTracker.RecordTxOutcome(aSrcId, false, false, false);
        if (aOnTxDone)
        {
            aOnTxDone(std::nullopt, static_cast<uint8_t>(OT_ERROR_NONE));
        }
        return;
    }

    if (ackSenderId == Rf::kInvalidNodeId)
    {
        mScheduler.ScheduleAfter(kAckWaitTimeoutUs, [this, aSrcId, cb = std::move(aOnTxDone)](void) {
            mKpiTracker.RecordTxOutcome(aSrcId, true, false, false);
            if (cb)
            {
                cb(std::nullopt, static_cast<uint8_t>(OT_ERROR_NO_ACK));
            }
        });
        return;
    }

    Rf::ChannelId channel = aTxFrame.mChannel;

    mScheduler.ScheduleAfter(kAckTurnaroundUs, [this, aSrcId, ackSenderId, channel, ackFrame = std::move(ackFrame),
                                                cb = std::move(aOnTxDone)](void) mutable {
        auto ait = mNodes.find(ackSenderId);

        if (ait == mNodes.end() || !ait->second->IsEnabled())
        {
            mKpiTracker.RecordTxOutcome(aSrcId, true, false, false);
            if (cb)
            {
                cb(std::nullopt, static_cast<uint8_t>(OT_ERROR_NO_ACK));
            }
            return;
        }

        const auto *arn      = mRadioModel.GetNode(ackSenderId);
        Rf::DbValue ackPower = (arn != nullptr) ? arn->mTxPowerDbm : Rf::kDefaultTxPowerDbm;
        uint64_t    ackDurUs = (Rf::kPhyHeaderLenBytes + ackFrame.mPsdu.size()) * kUsPerByte250Kbps;
        auto        ackStart = mRadioModel.BeginTx(ackSenderId, channel, ackPower, ackFrame.mPsdu.size(), ackDurUs);

        if (ackStart.mAborted)
        {
            mKpiTracker.RecordTxOutcome(aSrcId, true, false, false);
            if (cb)
            {
                cb(std::nullopt, static_cast<uint8_t>(OT_ERROR_NO_ACK));
            }
            return;
        }

        ackFrame.mChannel     = channel;
        ackFrame.mTimestampUs = mScheduler.GetNowUs();
        ackFrame.mRssiDbm     = Rf::ClipRssiToInt8(ackPower);

        MacFrameInfo ackMac = DissectWpanFrame(ackFrame.mPsdu.data(), ackFrame.mPsdu.size(), channel);

        mKpiTracker.RecordTxStart(ackSenderId, ackFrame.mPsdu.size(), ackDurUs, ackMac);
        if (mPcapWriter.IsOpen())
        {
            (void)mPcapWriter.AppendFrame(ackFrame);
        }

        mScheduler.ScheduleAfter(ackDurUs, [this, aSrcId, ackSenderId, channel, ackFrame = std::move(ackFrame),
                                            cb = std::move(cb)](void) mutable {
            auto                    ackEnd = mRadioModel.EndTx(ackSenderId, channel, ackFrame.mPsdu.size());
            auto                    ait2   = mNodes.find(ackSenderId);
            std::optional<RawFrame> receivedAck;

            if (ait2 != mNodes.end() && ait2->second->IsEnabled())
            {
                mRadioModel.SetNodeRadioState(ackSenderId, Rf::RadioState::kRx, channel);
            }

            for (const auto &d : ackEnd.mDeliveries)
            {
                if (d.mNodeId == aSrcId && d.mStatus == Rf::RxResultStatus::kOk)
                {
                    ackFrame.mRssiDbm = d.mRssiDbm;
                    ackFrame.mLqi     = 255;
                    receivedAck       = ackFrame;
                    break;
                }
            }

            mKpiTracker.RecordTxOutcome(aSrcId, true, receivedAck.has_value(), false);
            if (cb)
            {
                uint8_t err = receivedAck.has_value() ? static_cast<uint8_t>(OT_ERROR_NONE)
                                                      : static_cast<uint8_t>(OT_ERROR_NO_ACK);
                cb(receivedAck, err);
            }
        });
    });
}

void Simulation::StartEnergyScan(Rf::NodeId             aNodeId,
                                 Rf::ChannelId          aChannel,
                                 uint64_t               aDurationUs,
                                 EnergyScanDoneCallback aOnScanDone)
{
    if (!mRadioModel.BeginEnergyScan(aNodeId, aChannel))
    {
        mScheduler.ScheduleAfter(1, [cb = std::move(aOnScanDone)](void) {
            if (cb)
            {
                cb(Rf::kRssiInvalidInt8);
            }
        });
        return;
    }

    mScheduler.ScheduleAfter(aDurationUs, [this, aNodeId, aChannel, cb = std::move(aOnScanDone)](void) {
        int8_t rssi = mRadioModel.EndEnergyScan(aNodeId, aChannel);

        if (cb)
        {
            cb(rssi);
        }
    });
}

} // namespace Core
} // namespace otns
