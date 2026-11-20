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

#ifndef OTNS_SERVER_VIZ_PUBLISHER_HPP_
#define OTNS_SERVER_VIZ_PUBLISHER_HPP_

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "core/simulation.hpp"
#include "nlohmann/json.hpp"
#include "server/replay.hpp"
#include "server/ws_server.hpp"

namespace otns {
namespace Server {

struct VizSubscription
{
    uint32_t             mMaxHz   = 30;
    bool                 mPackets = true;
    std::set<Rf::NodeId> mWatchNodes;
};

/**
 * Coalesced visualization stream publisher adhering to viz_rpc_v1.schema.json.
 *
 * Emits event.snapshot on connect/reset, minimal rate-capped event.batch_diff
 * updates (30-60 Hz), speed-gated event.packet animations, event.stats, and
 * event.heartbeat to connected WebSocket clients and optional JsonlRecorder.
 */
class VizPublisher
{
public:
    static constexpr double kDefaultPacketAnimMaxSpeed = 5.0;

    VizPublisher(Core::Simulation &aSim, WsServer &aServer, JsonlRecorder *aRecorder = nullptr);

    void OnClientConnect(ClientId aClientId, bool aPaused);
    void OnClientDisconnect(ClientId aClientId);
    void UpdateSubscription(ClientId aClientId, const VizSubscription &aSub);

    nlohmann::json BuildSnapshot(bool aPaused) const;
    nlohmann::json BuildStatsEvent(void) const;
    nlohmann::json BuildHeartbeatEvent(bool aPaused) const;

    void PublishReset(bool aPaused);
    void PublishDiffIfDue(bool aPaused, bool aForce = false);
    void OnPacket(const Core::PacketObservation &aPacket);
    void PublishLog(Rf::NodeId aNodeId, const std::string &aLevel, const std::string &aMessage);

    void SetPacketAnimMaxSpeed(double aMaxSpeed) { mPacketAnimMaxSpeed = aMaxSpeed; }

private:
    struct CachedNode
    {
        bool operator==(const CachedNode &aOther) const;

        Rf::NodeId       mId          = Rf::kInvalidNodeId;
        double           mX           = 0.0;
        double           mY           = 0.0;
        double           mZ           = 0.0;
        Core::DeviceRole mRole        = Core::DeviceRole::kDisabled;
        uint16_t         mRloc16      = 0xFFFE;
        Rf::NodeId       mParentId    = 0;
        uint32_t         mPartitionId = 0;
        double           mRadioRange  = 0.0;
        bool             mFailed      = false;
        Core::NodeKind   mKind        = Core::NodeKind::kInProc;
        std::string      mExtAddr;
    };

    struct LinkKey
    {
        bool operator<(const LinkKey &aOther) const;

        Rf::NodeId  mSrc = 0;
        Rf::NodeId  mDst = 0;
        std::string mKind;
    };

    struct CachedLink
    {
        bool operator==(const CachedLink &aOther) const { return mRssiDbm == aOther.mRssiDbm && mLqi == aOther.mLqi; }

        int8_t  mRssiDbm = 0;
        uint8_t mLqi     = 0;
    };

    void ComputeCurrentState(std::map<Rf::NodeId, CachedNode> &aNodes, std::map<LinkKey, CachedLink> &aLinks) const;
    static nlohmann::json NodeToJson(const CachedNode &aNode);
    static nlohmann::json LinkToJson(const LinkKey &aKey, const CachedLink &aLink);
    void                  EmitToAll(const nlohmann::json &aEvent);

    Core::Simulation                     &mSim;
    WsServer                             &mServer;
    JsonlRecorder                        *mRecorder           = nullptr;
    double                                mPacketAnimMaxSpeed = kDefaultPacketAnimMaxSpeed;
    std::map<ClientId, VizSubscription>   mSubscribers;
    std::map<Rf::NodeId, CachedNode>      mLastNodes;
    std::map<LinkKey, CachedLink>         mLastLinks;
    std::chrono::steady_clock::time_point mLastPublishWall;
    bool                                  mHasPublishedOnce = false;
};

} // namespace Server
} // namespace otns

#endif // OTNS_SERVER_VIZ_PUBLISHER_HPP_
