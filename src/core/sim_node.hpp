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

#ifndef OTNS_CORE_SIM_NODE_HPP_
#define OTNS_CORE_SIM_NODE_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include "core/net_fabric.hpp"
#include "rf/types.hpp"

namespace otns {
namespace Core {

enum class NodeKind : uint8_t
{
    kInProc   = 0,
    kExternal = 1,
};

enum class NodeType : uint8_t
{
    kRouter = 0, // FTD Router-eligible
    kFed    = 1, // FTD non-router-eligible
    kMed    = 2, // Minimal End Device (rx-on-when-idle)
    kSed    = 3, // Sleepy End Device
};

enum class DeviceRole : uint8_t
{
    kDisabled = 0,
    kDetached = 1,
    kChild    = 2,
    kRouter   = 3,
    kLeader   = 4,
};

struct NodeConfig
{
    NodeType       mType       = NodeType::kRouter;
    Rf::Position3D mPos        = {};
    double         mRadioRange = Rf::kDefaultRadioRange;
    bool           mAutoStart  = true;
};

struct NodeSnapshot
{
    Rf::NodeId     mId          = Rf::kInvalidNodeId;
    NodeKind       mKind        = NodeKind::kInProc;
    Rf::Position3D mPos         = {};
    double         mRadioRange  = Rf::kDefaultRadioRange;
    DeviceRole     mRole        = DeviceRole::kDisabled;
    uint16_t       mRloc16      = 0xFFFE;
    uint32_t       mPartitionId = 0;
    std::string    mExtAddr;
    bool           mEnabled = true;
};

struct RawFrame
{
    Rf::ChannelId        mChannel     = Rf::kMinChannel;
    int8_t               mRssiDbm     = Rf::kRssiInvalidInt8;
    uint8_t              mLqi         = 255;
    uint64_t             mTimestampUs = 0;
    std::vector<uint8_t> mPsdu;
};

/**
 * Abstract node interface shared by in-process multi-instance OT nodes
 * and external socket-bridged ot-rfsim v3 nodes.
 */
class SimNode
{
public:
    virtual ~SimNode(void) = default;

    virtual Rf::NodeId     GetId(void) const                = 0;
    virtual NodeKind       GetKind(void) const              = 0;
    virtual Rf::Position3D GetPosition(void) const          = 0;
    virtual void           SetPosition(Rf::Position3D aPos) = 0;

    virtual bool IsEnabled(void) const     = 0;
    virtual void SetEnabled(bool aEnabled) = 0;

    virtual bool HasPendingTasklets(void) const = 0;
    virtual void ProcessTasklets(void)          = 0;

    virtual std::string ExecuteCli(const std::string &aCommand) = 0;
    virtual std::string TakeCliOutput(void)                     = 0;

    virtual NodeSnapshot GetSnapshot(void) const = 0;

    virtual bool OnRadioRxDone(const RawFrame &aRxFrame, RawFrame &aAckOut) = 0;

    // Optional virtual backbone delivery hooks (TREL / Infra-If / Host UDP).
    virtual void OnTrelPacket(const NetPacket &aPacket) { (void)aPacket; }
    virtual void OnInfraIfPacket(const NetPacket &aPacket) { (void)aPacket; }
    virtual void OnHostUdpPacket(const NetPacket &aPacket) { (void)aPacket; }
};

} // namespace Core
} // namespace otns

#endif // OTNS_CORE_SIM_NODE_HPP_
