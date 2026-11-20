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

#include "core/net_fabric.hpp"

#include <algorithm>

namespace otns {
namespace Core {

Ip6Addr NetFabric::MakeNodeBackboneIp6(Rf::NodeId aNodeId)
{
    // fd00:0:0:cafe::/<nodeId>
    Ip6Addr addr = {};

    addr[0]  = 0xFD;
    addr[1]  = 0x00;
    addr[6]  = 0xCA;
    addr[7]  = 0xFE;
    addr[12] = static_cast<uint8_t>((aNodeId >> 24u) & 0xFFu);
    addr[13] = static_cast<uint8_t>((aNodeId >> 16u) & 0xFFu);
    addr[14] = static_cast<uint8_t>((aNodeId >> 8u) & 0xFFu);
    addr[15] = static_cast<uint8_t>(aNodeId & 0xFFu);

    return addr;
}

Ip6Addr NetFabric::MakeNodeLinkLocalIp6(Rf::NodeId aNodeId)
{
    // fe80::/<nodeId>
    Ip6Addr addr = {};

    addr[0]  = 0xFE;
    addr[1]  = 0x80;
    addr[12] = static_cast<uint8_t>((aNodeId >> 24u) & 0xFFu);
    addr[13] = static_cast<uint8_t>((aNodeId >> 16u) & 0xFFu);
    addr[14] = static_cast<uint8_t>((aNodeId >> 8u) & 0xFFu);
    addr[15] = static_cast<uint8_t>(aNodeId & 0xFFu);

    return addr;
}

bool NetFabric::IsMulticastIp6(const Ip6Addr &aAddr) { return aAddr[0] == 0xFF; }

void NetFabric::Clear(void)
{
    mTrelPeers.clear();
    mInfraIfMembers.clear();
    mHostUdpBindings.clear();
}

void NetFabric::RemoveNode(Rf::NodeId aNodeId, const TrelPeerCallback &aOnPeerChange)
{
    UnregisterTrelPeer(aNodeId, aOnPeerChange);
    LeaveInfraIf(aNodeId);

    for (auto it = mHostUdpBindings.begin(); it != mHostUdpBindings.end();)
    {
        auto &vec = it->second;

        vec.erase(std::remove(vec.begin(), vec.end(), aNodeId), vec.end());
        if (vec.empty())
        {
            it = mHostUdpBindings.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void NetFabric::RegisterTrelPeer(Rf::NodeId                  aNodeId,
                                 uint16_t                    aUdpPort,
                                 const std::vector<uint8_t> &aTxtData,
                                 const TrelPeerCallback     &aOnPeerChange)
{
    TrelPeerRecord rec;

    rec.mNodeId  = aNodeId;
    rec.mIp6Addr = MakeNodeBackboneIp6(aNodeId);
    rec.mUdpPort = aUdpPort;
    rec.mTxtData = aTxtData;

    bool isNew = (mTrelPeers.find(aNodeId) == mTrelPeers.end());

    mTrelPeers[aNodeId] = rec;

    if (aOnPeerChange)
    {
        for (const auto &pair : mTrelPeers)
        {
            if (pair.first == aNodeId)
            {
                continue;
            }
            // Inform existing peers about this node.
            aOnPeerChange(pair.first, rec, false);
            // If newly registered, inform this node about all existing peers.
            if (isNew)
            {
                aOnPeerChange(aNodeId, pair.second, false);
            }
        }
    }
}

void NetFabric::UnregisterTrelPeer(Rf::NodeId aNodeId, const TrelPeerCallback &aOnPeerChange)
{
    auto it = mTrelPeers.find(aNodeId);

    if (it == mTrelPeers.end())
    {
        return;
    }

    TrelPeerRecord removed = it->second;

    mTrelPeers.erase(it);

    if (aOnPeerChange)
    {
        for (const auto &pair : mTrelPeers)
        {
            aOnPeerChange(pair.first, removed, true);
        }
    }
}

std::vector<TrelPeerRecord> NetFabric::GetTrelPeers(void) const
{
    std::vector<TrelPeerRecord> peers;

    peers.reserve(mTrelPeers.size());
    for (const auto &pair : mTrelPeers)
    {
        peers.push_back(pair.second);
    }

    return peers;
}

std::optional<Rf::NodeId> NetFabric::ResolveTrelTarget(const Ip6Addr &aDstIp6, uint16_t aDstPort) const
{
    for (const auto &pair : mTrelPeers)
    {
        if (pair.second.mIp6Addr == aDstIp6 && (aDstPort == 0 || pair.second.mUdpPort == aDstPort))
        {
            return pair.first;
        }
    }

    return std::nullopt;
}

void NetFabric::JoinInfraIf(Rf::NodeId aNodeId) { mInfraIfMembers[aNodeId] = MakeNodeLinkLocalIp6(aNodeId); }

void NetFabric::LeaveInfraIf(Rf::NodeId aNodeId) { mInfraIfMembers.erase(aNodeId); }

std::vector<Rf::NodeId> NetFabric::ResolveInfraIfTargets(Rf::NodeId aSrcId, const Ip6Addr &aDstIp6) const
{
    std::vector<Rf::NodeId> targets;
    bool                    mcast = IsMulticastIp6(aDstIp6);

    for (const auto &pair : mInfraIfMembers)
    {
        if (pair.first == aSrcId)
        {
            continue;
        }
        if (mcast || pair.second == aDstIp6 || MakeNodeBackboneIp6(pair.first) == aDstIp6)
        {
            targets.push_back(pair.first);
        }
    }

    return targets;
}

void NetFabric::BindHostUdp(Rf::NodeId aNodeId, uint16_t aPort)
{
    auto &vec = mHostUdpBindings[aPort];

    if (std::find(vec.begin(), vec.end(), aNodeId) == vec.end())
    {
        vec.push_back(aNodeId);
        std::sort(vec.begin(), vec.end());
    }
}

void NetFabric::UnbindHostUdp(Rf::NodeId aNodeId, uint16_t aPort)
{
    auto it = mHostUdpBindings.find(aPort);

    if (it == mHostUdpBindings.end())
    {
        return;
    }

    auto &vec = it->second;

    vec.erase(std::remove(vec.begin(), vec.end(), aNodeId), vec.end());
    if (vec.empty())
    {
        mHostUdpBindings.erase(it);
    }
}

std::vector<Rf::NodeId> NetFabric::ResolveHostUdpTargets(Rf::NodeId     aSrcId,
                                                         const Ip6Addr &aDstIp6,
                                                         uint16_t       aDstPort) const
{
    std::vector<Rf::NodeId> targets;
    auto                    it = mHostUdpBindings.find(aDstPort);

    if (it == mHostUdpBindings.end())
    {
        return targets;
    }

    bool mcast = IsMulticastIp6(aDstIp6);

    for (Rf::NodeId nodeId : it->second)
    {
        if (nodeId == aSrcId)
        {
            continue;
        }
        if (mcast || MakeNodeBackboneIp6(nodeId) == aDstIp6 || MakeNodeLinkLocalIp6(nodeId) == aDstIp6)
        {
            targets.push_back(nodeId);
        }
    }

    return targets;
}

} // namespace Core
} // namespace otns
