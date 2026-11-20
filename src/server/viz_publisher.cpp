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

#include "server/viz_publisher.hpp"

#include <algorithm>
#include <cmath>

namespace otns {
namespace Server {

namespace {

const char *RoleToString(Core::DeviceRole aRole)
{
    switch (aRole)
    {
    case Core::DeviceRole::kDisabled:
        return "disabled";
    case Core::DeviceRole::kDetached:
        return "detached";
    case Core::DeviceRole::kChild:
        return "child";
    case Core::DeviceRole::kRouter:
        return "router";
    case Core::DeviceRole::kLeader:
        return "leader";
    }
    return "disabled";
}

const char *PresetToName(Rf::RadioModelPreset aPreset)
{
    switch (aPreset)
    {
    case Rf::RadioModelPreset::kIdeal:
        return "Ideal";
    case Rf::RadioModelPreset::kIdealRssi:
        return "IdealRssi";
    case Rf::RadioModelPreset::kMutualInterference:
        return "MutualInterference";
    case Rf::RadioModelPreset::kMidisc:
        return "Midisc";
    case Rf::RadioModelPreset::kOutdoor:
        return "Outdoor";
    }
    return "MutualInterference";
}

nlohmann::json StatsToJson(const Core::SimulationStats &aStats)
{
    nlohmann::json s = nlohmann::json::object();

    s["total"]      = aStats.mTotal;
    s["leaders"]    = aStats.mLeaders;
    s["routers"]    = aStats.mRouters;
    s["children"]   = aStats.mChildren;
    s["detached"]   = aStats.mDetached;
    s["disabled"]   = aStats.mDisabled;
    s["partitions"] = aStats.mPartitions;

    return s;
}

} // namespace

bool VizPublisher::CachedNode::operator==(const CachedNode &aOther) const
{
    return mId == aOther.mId && std::abs(mX - aOther.mX) < 1e-6 && std::abs(mY - aOther.mY) < 1e-6 &&
           std::abs(mZ - aOther.mZ) < 1e-6 && mRole == aOther.mRole && mRloc16 == aOther.mRloc16 &&
           mParentId == aOther.mParentId && mPartitionId == aOther.mPartitionId &&
           std::abs(mRadioRange - aOther.mRadioRange) < 1e-6 && mFailed == aOther.mFailed && mKind == aOther.mKind &&
           mExtAddr == aOther.mExtAddr;
}

bool VizPublisher::LinkKey::operator<(const LinkKey &aOther) const
{
    if (mSrc != aOther.mSrc)
    {
        return mSrc < aOther.mSrc;
    }
    if (mDst != aOther.mDst)
    {
        return mDst < aOther.mDst;
    }
    return mKind < aOther.mKind;
}

VizPublisher::VizPublisher(Core::Simulation &aSim, WsServer &aServer, JsonlRecorder *aRecorder)
    : mSim(aSim)
    , mServer(aServer)
    , mRecorder(aRecorder)
    , mLastPublishWall(std::chrono::steady_clock::now())
{
}

void VizPublisher::ComputeCurrentState(std::map<Rf::NodeId, CachedNode> &aNodes,
                                       std::map<LinkKey, CachedLink>    &aLinks) const
{
    std::vector<Core::NodeSnapshot> snaps = mSim.GetAllNodeSnapshots();
    std::map<uint16_t, Rf::NodeId>  rlocToId;

    aNodes.clear();
    aLinks.clear();

    for (const auto &snap : snaps)
    {
        if (snap.mRloc16 != 0xFFFE && snap.mRole != Core::DeviceRole::kDisabled &&
            snap.mRole != Core::DeviceRole::kDetached)
        {
            rlocToId[snap.mRloc16] = snap.mId;
        }
    }

    for (const auto &snap : snaps)
    {
        CachedNode cn;

        cn.mId          = snap.mId;
        cn.mX           = snap.mPos.mX;
        cn.mY           = snap.mPos.mY;
        cn.mZ           = snap.mPos.mZ;
        cn.mRole        = snap.mRole;
        cn.mRloc16      = snap.mRloc16;
        cn.mPartitionId = snap.mPartitionId;
        cn.mRadioRange  = snap.mRadioRange;
        cn.mFailed      = !snap.mEnabled;
        cn.mKind        = snap.mKind;
        cn.mExtAddr     = snap.mExtAddr;

        if (snap.mParentRloc16 != 0xFFFE)
        {
            auto pit = rlocToId.find(snap.mParentRloc16);

            if (pit != rlocToId.end() && pit->second != snap.mId)
            {
                cn.mParentId                              = pit->second;
                aLinks[{snap.mId, pit->second, "parent"}] = {Rf::kRssiInvalidInt8, 3};
            }
        }

        for (const auto &nb : snap.mNeighbors)
        {
            auto nit = rlocToId.find(nb.mRloc16);

            if (nit == rlocToId.end() || nit->second == snap.mId)
            {
                continue;
            }

            if (nb.mIsChild)
            {
                aLinks[{snap.mId, nit->second, "child"}] = {nb.mRssiDbm, nb.mLqi};
            }
            else if (snap.mRole == Core::DeviceRole::kRouter || snap.mRole == Core::DeviceRole::kLeader)
            {
                // Canonical undirected router-router edge (min -> max) to avoid duplicates.
                Rf::NodeId u = std::min(snap.mId, nit->second);
                Rf::NodeId v = std::max(snap.mId, nit->second);

                aLinks[{u, v, "router"}] = {nb.mRssiDbm, nb.mLqi};
            }
        }

        aNodes[snap.mId] = std::move(cn);
    }
}

nlohmann::json VizPublisher::NodeToJson(const CachedNode &aNode)
{
    nlohmann::json obj = nlohmann::json::object();

    obj["id"]           = aNode.mId;
    obj["x"]            = aNode.mX;
    obj["y"]            = aNode.mY;
    obj["z"]            = aNode.mZ;
    obj["role"]         = RoleToString(aNode.mRole);
    obj["rloc16"]       = aNode.mRloc16;
    obj["ext_addr"]     = aNode.mExtAddr;
    obj["partition_id"] = aNode.mPartitionId;
    obj["parent_id"]    = aNode.mParentId;
    obj["radio_range"]  = aNode.mRadioRange;
    obj["failed"]       = aNode.mFailed;
    obj["kind"]         = (aNode.mKind == Core::NodeKind::kExternal) ? "external" : "inproc";

    return obj;
}

nlohmann::json VizPublisher::LinkToJson(const LinkKey &aKey, const CachedLink &aLink)
{
    nlohmann::json obj = nlohmann::json::object();

    obj["src"]  = aKey.mSrc;
    obj["dst"]  = aKey.mDst;
    obj["kind"] = aKey.mKind;
    if (aLink.mRssiDbm != Rf::kRssiInvalidInt8)
    {
        obj["rssi_dbm"] = aLink.mRssiDbm;
    }
    obj["lqi"] = std::min<uint8_t>(aLink.mLqi, 3u);

    return obj;
}

nlohmann::json VizPublisher::BuildSnapshot(bool aPaused) const
{
    std::map<Rf::NodeId, CachedNode> nodes;
    std::map<LinkKey, CachedLink>    links;
    nlohmann::json                   root     = nlohmann::json::object();
    nlohmann::json                   nodesArr = nlohmann::json::array();
    nlohmann::json                   linksArr = nlohmann::json::array();

    ComputeCurrentState(nodes, links);

    for (const auto &pair : nodes)
    {
        nodesArr.push_back(NodeToJson(pair.second));
    }
    for (const auto &pair : links)
    {
        linksArr.push_back(LinkToJson(pair.first, pair.second));
    }

    root["v"]        = 1;
    root["type"]     = "event.snapshot";
    root["ts_us"]    = mSim.GetNowUs();
    root["engine"]   = "otns-v3";
    root["speed"]    = mSim.GetSpeed();
    root["paused"]   = aPaused;
    root["rf_model"] = PresetToName(mSim.GetConfig().mRfPreset);
    root["nodes"]    = std::move(nodesArr);
    root["links"]    = std::move(linksArr);
    root["stats"]    = StatsToJson(mSim.GetStats());

    return root;
}

nlohmann::json VizPublisher::BuildStatsEvent(void) const
{
    nlohmann::json root = nlohmann::json::object();

    root["v"]     = 1;
    root["type"]  = "event.stats";
    root["ts_us"] = mSim.GetNowUs();
    root["stats"] = StatsToJson(mSim.GetStats());

    return root;
}

nlohmann::json VizPublisher::BuildHeartbeatEvent(bool aPaused) const
{
    nlohmann::json root = nlohmann::json::object();

    root["v"]      = 1;
    root["type"]   = "event.heartbeat";
    root["ts_us"]  = mSim.GetNowUs();
    root["speed"]  = mSim.GetSpeed();
    root["paused"] = aPaused;

    return root;
}

void VizPublisher::EmitToAll(const nlohmann::json &aEvent)
{
    std::string line = aEvent.dump();

    if (mRecorder != nullptr && mRecorder->IsOpen())
    {
        (void)mRecorder->WriteEvent(line);
    }

    for (const auto &pair : mSubscribers)
    {
        (void)mServer.SendText(pair.first, line);
    }
}

void VizPublisher::OnClientConnect(ClientId aClientId, bool aPaused)
{
    mSubscribers[aClientId] = VizSubscription{};
    (void)mServer.SendText(aClientId, BuildSnapshot(aPaused).dump());
}

void VizPublisher::OnClientDisconnect(ClientId aClientId) { mSubscribers.erase(aClientId); }

void VizPublisher::UpdateSubscription(ClientId aClientId, const VizSubscription &aSub)
{
    mSubscribers[aClientId] = aSub;
}

void VizPublisher::PublishReset(bool aPaused)
{
    ComputeCurrentState(mLastNodes, mLastLinks);
    mHasPublishedOnce = true;
    mLastPublishWall  = std::chrono::steady_clock::now();
    EmitToAll(BuildSnapshot(aPaused));
}

void VizPublisher::PublishDiffIfDue(bool aPaused, bool aForce)
{
    if (mSubscribers.empty() && (mRecorder == nullptr || !mRecorder->IsOpen()))
    {
        return;
    }

    uint32_t maxHz = 30;

    for (const auto &pair : mSubscribers)
    {
        if (pair.second.mMaxHz > maxHz)
        {
            maxHz = std::min<uint32_t>(pair.second.mMaxHz, 60u);
        }
    }

    auto   now       = std::chrono::steady_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(now - mLastPublishWall).count();
    double minGapMs  = 1000.0 / static_cast<double>(std::max<uint32_t>(maxHz, 1u));

    if (!aForce && mHasPublishedOnce && elapsedMs < minGapMs)
    {
        return;
    }

    std::map<Rf::NodeId, CachedNode> curNodes;
    std::map<LinkKey, CachedLink>    curLinks;

    ComputeCurrentState(curNodes, curLinks);

    nlohmann::json upsertNodes = nlohmann::json::array();
    nlohmann::json removeNodes = nlohmann::json::array();
    nlohmann::json upsertLinks = nlohmann::json::array();
    nlohmann::json removeLinks = nlohmann::json::array();

    for (const auto &pair : curNodes)
    {
        auto prev = mLastNodes.find(pair.first);

        if (prev == mLastNodes.end() || !(prev->second == pair.second))
        {
            upsertNodes.push_back(NodeToJson(pair.second));
        }
    }
    for (const auto &pair : mLastNodes)
    {
        if (curNodes.find(pair.first) == curNodes.end())
        {
            removeNodes.push_back(pair.first);
        }
    }

    for (const auto &pair : curLinks)
    {
        auto prev = mLastLinks.find(pair.first);

        if (prev == mLastLinks.end() || !(prev->second == pair.second))
        {
            upsertLinks.push_back(LinkToJson(pair.first, pair.second));
        }
    }
    for (const auto &pair : mLastLinks)
    {
        if (curLinks.find(pair.first) == curLinks.end())
        {
            nlohmann::json rem = nlohmann::json::object();

            rem["src"]  = pair.first.mSrc;
            rem["dst"]  = pair.first.mDst;
            rem["kind"] = pair.first.mKind;
            removeLinks.push_back(std::move(rem));
        }
    }

    mLastNodes        = std::move(curNodes);
    mLastLinks        = std::move(curLinks);
    mLastPublishWall  = now;
    mHasPublishedOnce = true;

    if (!upsertNodes.empty() || !removeNodes.empty() || !upsertLinks.empty() || !removeLinks.empty())
    {
        nlohmann::json diff = nlohmann::json::object();

        diff["v"]     = 1;
        diff["type"]  = "event.batch_diff";
        diff["ts_us"] = mSim.GetNowUs();
        if (!upsertNodes.empty())
        {
            diff["upsert_nodes"] = std::move(upsertNodes);
        }
        if (!removeNodes.empty())
        {
            diff["remove_nodes"] = std::move(removeNodes);
        }
        if (!upsertLinks.empty())
        {
            diff["upsert_links"] = std::move(upsertLinks);
        }
        if (!removeLinks.empty())
        {
            diff["remove_links"] = std::move(removeLinks);
        }

        EmitToAll(diff);
        EmitToAll(BuildStatsEvent());
    }

    EmitToAll(BuildHeartbeatEvent(aPaused));
}

void VizPublisher::OnPacket(const Core::PacketObservation &aPacket)
{
    bool recorderOpen = (mRecorder != nullptr && mRecorder->IsOpen());

    if (mSubscribers.empty() && !recorderOpen)
    {
        return;
    }

    double speed        = mSim.GetSpeed();
    bool   highSpeed    = (speed == 0.0 || speed > mPacketAnimMaxSpeed);
    bool   anyRecipient = recorderOpen;

    for (const auto &pair : mSubscribers)
    {
        const auto &sub = pair.second;

        if (!sub.mPackets)
        {
            continue;
        }
        bool watched = (sub.mWatchNodes.count(aPacket.mSrcId) > 0) || (sub.mWatchNodes.count(aPacket.mDstId) > 0);
        if (!highSpeed || watched)
        {
            anyRecipient = true;
            break;
        }
    }

    if (!anyRecipient)
    {
        return;
    }

    nlohmann::json evt = nlohmann::json::object();

    evt["v"]           = 1;
    evt["type"]        = "event.packet";
    evt["ts_us"]       = aPacket.mTimestampUs;
    evt["src"]         = aPacket.mSrcId;
    evt["dst"]         = aPacket.mDstId;
    evt["channel"]     = aPacket.mChannel;
    evt["duration_us"] = aPacket.mDurationUs;
    evt["rssi_dbm"]    = aPacket.mRssiDbm;
    evt["frame_len"]   = aPacket.mFrameLen;
    evt["protocol"]    = aPacket.mProtocol;
    evt["summary"]     = aPacket.mSummary;

    std::string line = evt.dump();

    if (recorderOpen)
    {
        (void)mRecorder->WriteEvent(line);
    }

    for (const auto &pair : mSubscribers)
    {
        const auto &sub = pair.second;

        if (!sub.mPackets)
        {
            continue;
        }
        bool watched = (sub.mWatchNodes.count(aPacket.mSrcId) > 0) || (sub.mWatchNodes.count(aPacket.mDstId) > 0);
        if (!highSpeed || watched)
        {
            (void)mServer.SendText(pair.first, line);
        }
    }
}

void VizPublisher::PublishLog(Rf::NodeId aNodeId, const std::string &aLevel, const std::string &aMessage)
{
    nlohmann::json evt = nlohmann::json::object();

    evt["v"]       = 1;
    evt["type"]    = "event.log";
    evt["ts_us"]   = mSim.GetNowUs();
    evt["node_id"] = aNodeId;
    evt["level"]   = aLevel;
    evt["message"] = aMessage;

    std::string line = evt.dump();

    for (const auto &pair : mSubscribers)
    {
        if (pair.second.mWatchNodes.empty() || pair.second.mWatchNodes.count(aNodeId) > 0)
        {
            (void)mServer.SendText(pair.first, line);
        }
    }
}

} // namespace Server
} // namespace otns
