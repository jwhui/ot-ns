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

#include <cmath>
#include <set>

#include "nodes/inproc_node.hpp"

namespace otns {
namespace Core {

namespace {

static constexpr uint64_t kUsPerByte250Kbps = 32;   // 250 kbps => 4 us/bit => 32 us/byte
static constexpr uint64_t kCcaDurationUs    = 128;  // 8 symbols * 16 us
static constexpr uint64_t kAckTurnaroundUs  = 192;  // aTurnaroundTime = 12 symbols * 16 us
static constexpr uint64_t kAckWaitTimeoutUs = 1000; // macAckWaitDuration

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

    mScheduler.Clear();
    mNodes.clear();
}

Rf::NodeId Simulation::AddNode(const NodeConfig &aConfig)
{
    ScopedCurrentSim   guard(this);
    Rf::NodeId         id = mNextNodeId++;
    Rf::RadioNodeState radioNode;

    radioNode.mId               = id;
    radioNode.mPos              = aConfig.mPos;
    radioNode.mRadioRange       = aConfig.mRadioRange;
    radioNode.mChannel          = mConfig.mChannel;
    radioNode.mTxPowerDbm       = Rf::kDefaultTxPowerDbm;
    radioNode.mRxSensitivityDbm = Rf::kDefaultRxSensitivity;
    radioNode.mState            = Rf::RadioState::kDisabled;

    mRadioModel.AddNode(radioNode);

    auto node = std::make_unique<Nodes::InProcNode>(*this, id, aConfig);

    mNodes.emplace(id, std::move(node));
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

    mRadioModel.RemoveNode(aId);
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

void Simulation::AdvanceUs(uint64_t aDurationUs)
{
    ScopedCurrentSim guard(this);
    uint64_t         targetUs = mScheduler.GetNowUs() + aDurationUs;

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
}

void Simulation::AdvanceSec(double aSeconds)
{
    if (aSeconds > 0.0)
    {
        AdvanceUs(static_cast<uint64_t>(std::llround(aSeconds * 1e6)));
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
        if (aOnTxDone)
        {
            aOnTxDone(std::nullopt, static_cast<uint8_t>(OT_ERROR_NONE));
        }
        return;
    }

    if (ackSenderId == Rf::kInvalidNodeId)
    {
        mScheduler.ScheduleAfter(kAckWaitTimeoutUs, [cb = std::move(aOnTxDone)](void) {
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
            if (cb)
            {
                cb(std::nullopt, static_cast<uint8_t>(OT_ERROR_NO_ACK));
            }
            return;
        }

        ackFrame.mChannel     = channel;
        ackFrame.mTimestampUs = mScheduler.GetNowUs();

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
