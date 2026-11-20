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

#ifndef OTNS_CORE_NET_FABRIC_HPP_
#define OTNS_CORE_NET_FABRIC_HPP_

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "rf/types.hpp"

namespace otns {
namespace Core {

using Ip6Addr = std::array<uint8_t, 16>;

struct TrelPeerRecord
{
    Rf::NodeId           mNodeId  = Rf::kInvalidNodeId;
    Ip6Addr              mIp6Addr = {};
    uint16_t             mUdpPort = 0;
    std::vector<uint8_t> mTxtData;
};

struct NetPacket
{
    Rf::NodeId           mSrcNodeId = Rf::kInvalidNodeId;
    Ip6Addr              mSrcIp6    = {};
    Ip6Addr              mDstIp6    = {};
    uint16_t             mSrcPort   = 0;
    uint16_t             mDstPort   = 0;
    std::vector<uint8_t> mPayload;
};

/**
 * Deterministic virtual IPv6/Ethernet backbone connecting nodes for TREL,
 * Border Router Adjacent Infrastructure Link (Infra-If), and simulated Host UDP/IPv6.
 */
class NetFabric
{
public:
    static constexpr uint64_t kDefaultBackboneDelayUs = 200; // 200 us LAN hop

    using TrelPeerCallback = std::function<void(Rf::NodeId aTargetId, const TrelPeerRecord &aPeer, bool aRemoved)>;

    static Ip6Addr MakeNodeBackboneIp6(Rf::NodeId aNodeId);
    static Ip6Addr MakeNodeLinkLocalIp6(Rf::NodeId aNodeId);
    static bool    IsMulticastIp6(const Ip6Addr &aAddr);

    void Clear(void);
    void RemoveNode(Rf::NodeId aNodeId, const TrelPeerCallback &aOnPeerChange = nullptr);

    // TREL registry & routing
    void                        RegisterTrelPeer(Rf::NodeId                  aNodeId,
                                                 uint16_t                    aUdpPort,
                                                 const std::vector<uint8_t> &aTxtData,
                                                 const TrelPeerCallback     &aOnPeerChange = nullptr);
    void                        UnregisterTrelPeer(Rf::NodeId aNodeId, const TrelPeerCallback &aOnPeerChange = nullptr);
    std::vector<TrelPeerRecord> GetTrelPeers(void) const;
    std::optional<Rf::NodeId>   ResolveTrelTarget(const Ip6Addr &aDstIp6, uint16_t aDstPort) const;

    // Infra-If (Border Router AIL) membership & routing
    void                    JoinInfraIf(Rf::NodeId aNodeId);
    void                    LeaveInfraIf(Rf::NodeId aNodeId);
    std::vector<Rf::NodeId> ResolveInfraIfTargets(Rf::NodeId aSrcId, const Ip6Addr &aDstIp6) const;

    // Host UDP/IPv6 binding & routing
    void                    BindHostUdp(Rf::NodeId aNodeId, uint16_t aPort);
    void                    UnbindHostUdp(Rf::NodeId aNodeId, uint16_t aPort);
    std::vector<Rf::NodeId> ResolveHostUdpTargets(Rf::NodeId aSrcId, const Ip6Addr &aDstIp6, uint16_t aDstPort) const;

    uint64_t GetBackboneDelayUs(void) const { return mBackboneDelayUs; }
    void     SetBackboneDelayUs(uint64_t aDelayUs) { mBackboneDelayUs = aDelayUs; }

private:
    uint64_t                                    mBackboneDelayUs = kDefaultBackboneDelayUs;
    std::map<Rf::NodeId, TrelPeerRecord>        mTrelPeers;
    std::map<Rf::NodeId, Ip6Addr>               mInfraIfMembers;
    std::map<uint16_t, std::vector<Rf::NodeId>> mHostUdpBindings;
};

} // namespace Core
} // namespace otns

#endif // OTNS_CORE_NET_FABRIC_HPP_
