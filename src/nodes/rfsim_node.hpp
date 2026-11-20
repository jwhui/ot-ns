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

#ifndef OTNS_NODES_RFSIM_NODE_HPP_
#define OTNS_NODES_RFSIM_NODE_HPP_

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/scheduler.hpp"
#include "core/sim_node.hpp"
#include "protocol/rfsim_v3_codec.hpp"

namespace otns {
namespace Core {
class Simulation;
} // namespace Core

namespace Nodes {

/**
 * External OS-process node bridged over a Unix or TCP socket using the
 * versioned binary/TLV ot-rfsim v3 protocol in lock-step virtual time.
 */
class RfSimNode final : public Core::SimNode
{
public:
    static constexpr size_t kMaxRxBufferBytes = 256u * 1024u;
    static constexpr int    kStepTimeoutMs    = 5000;

    RfSimNode(Core::Simulation       &aSim,
              Rf::NodeId              aId,
              const Core::NodeConfig &aConfig,
              int                     aSocketFd,
              pid_t                   aChildPid = -1);
    ~RfSimNode(void) override;

    RfSimNode(const RfSimNode &)            = delete;
    RfSimNode &operator=(const RfSimNode &) = delete;

    // Completes initial synchronization (drains startup frames until first StepDone).
    bool CompleteInitialSync(void);

    bool IsConnected(void) const { return mSockFd >= 0 && !mDisconnected; }
    void CloseConnection(void);

    // SimNode overrides
    Rf::NodeId     GetId(void) const override { return mId; }
    Core::NodeKind GetKind(void) const override { return Core::NodeKind::kExternal; }
    Rf::Position3D GetPosition(void) const override { return mPos; }
    void           SetPosition(Rf::Position3D aPos) override { mPos = aPos; }

    bool IsEnabled(void) const override { return mEnabled; }
    void SetEnabled(bool aEnabled) override;

    bool HasPendingTasklets(void) const override { return false; }
    void ProcessTasklets(void) override {}

    std::string        ExecuteCli(const std::string &aCommand) override;
    std::string        TakeCliOutput(void) override;
    Core::NodeSnapshot GetSnapshot(void) const override;

    bool OnRadioRxDone(const Core::RawFrame &aRxFrame, Core::RawFrame &aAckOut) override;
    void OnTrelPacket(const Core::NetPacket &aPacket) override;
    void OnInfraIfPacket(const Core::NetPacket &aPacket) override;
    void OnHostUdpPacket(const Core::NetPacket &aPacket) override;

    const std::vector<std::string> &GetLogs(void) const { return mLogs; }

private:
    bool SendFrame(const Protocol::Frame &aFrame);
    bool PumpUntilStepDone(Core::RawFrame *aAckOut = nullptr);
    bool HandleIncomingFrame(const Protocol::Frame &aFrame, bool &aSawStepDone, Core::RawFrame *aAckOut);
    void ScheduleAlarm(uint64_t aTargetUs);
    void CancelAlarm(void);
    void ReapChildProcess(void);

    Core::Simulation     &mSim;
    Rf::NodeId            mId           = Rf::kInvalidNodeId;
    Rf::Position3D        mPos          = {};
    double                mRadioRange   = Rf::kDefaultRadioRange;
    bool                  mEnabled      = true;
    int                   mSockFd       = -1;
    pid_t                 mChildPid     = -1;
    bool                  mDisconnected = false;
    std::shared_ptr<bool> mAliveFlag;

    Core::EventId        mAlarmEventId = Core::kInvalidEventId;
    std::vector<uint8_t> mRxBuffer;
    std::string          mCliOutput;

    // Cached snapshot fields pushed via kStatusPush
    Core::DeviceRole mRole        = Core::DeviceRole::kDisabled;
    uint16_t         mRloc16      = 0xFFFE;
    uint32_t         mPartitionId = 0;
    std::string      mExtAddr;
    Rf::ChannelId    mChannel = Rf::kMinChannel;

    std::vector<std::string> mLogs;
};

} // namespace Nodes
} // namespace otns

#endif // OTNS_NODES_RFSIM_NODE_HPP_
