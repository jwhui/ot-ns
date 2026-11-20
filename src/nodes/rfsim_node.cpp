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

#include "nodes/rfsim_node.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "core/simulation.hpp"

namespace otns {
namespace Nodes {

namespace {

static constexpr size_t kMaxStoredLogs = 256;

Rf::RadioState ByteToRfState(uint8_t aRaw)
{
    switch (aRaw)
    {
    case 1:
        return Rf::RadioState::kSleep;
    case 2:
        return Rf::RadioState::kRx;
    case 3:
        return Rf::RadioState::kTx;
    default:
        return Rf::RadioState::kDisabled;
    }
}

Core::DeviceRole ByteToDeviceRole(uint8_t aRaw)
{
    switch (aRaw)
    {
    case 1:
        return Core::DeviceRole::kDetached;
    case 2:
        return Core::DeviceRole::kChild;
    case 3:
        return Core::DeviceRole::kRouter;
    case 4:
        return Core::DeviceRole::kLeader;
    default:
        return Core::DeviceRole::kDisabled;
    }
}

bool SendAll(int aFd, const uint8_t *aData, size_t aLength)
{
    size_t sent = 0;

    while (sent < aLength)
    {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t n = ::send(aFd, aData + sent, aLength - sent, flags);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (n == 0)
        {
            return false;
        }
        sent += static_cast<size_t>(n);
    }

    return true;
}

} // namespace

RfSimNode::RfSimNode(Core::Simulation       &aSim,
                     Rf::NodeId              aId,
                     const Core::NodeConfig &aConfig,
                     int                     aSocketFd,
                     pid_t                   aChildPid)
    : mSim(aSim)
    , mId(aId)
    , mPos(aConfig.mPos)
    , mRadioRange(aConfig.mRadioRange)
    , mSockFd(aSocketFd)
    , mChildPid(aChildPid)
    , mAliveFlag(std::make_shared<bool>(true))
    , mChannel(aSim.GetConfig().mChannel)
{
#ifdef SO_NOSIGPIPE
    if (mSockFd >= 0)
    {
        int one = 1;

        (void)::setsockopt(mSockFd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
}

RfSimNode::~RfSimNode(void)
{
    *mAliveFlag = false;
    CancelAlarm();
    CloseConnection();
    ReapChildProcess();
}

void RfSimNode::CloseConnection(void)
{
    if (mSockFd >= 0)
    {
        if (!mDisconnected)
        {
            Protocol::Frame bye;

            bye.mType        = Protocol::MsgType::kShutdown;
            bye.mTimestampUs = mSim.GetNowUs();
            (void)SendFrame(bye);
        }
        ::close(mSockFd);
        mSockFd = -1;
    }
    mDisconnected = true;
}

void RfSimNode::ReapChildProcess(void)
{
    if (mChildPid <= 0)
    {
        return;
    }

    int status = 0;

    // Wait briefly for graceful exit after kShutdown; otherwise SIGTERM then SIGKILL.
    for (int i = 0; i < 40; ++i)
    {
        pid_t r = ::waitpid(mChildPid, &status, WNOHANG);

        if (r == mChildPid || r < 0)
        {
            mChildPid = -1;
            return;
        }
        ::usleep(10000); // 10 ms
    }

    (void)::kill(mChildPid, SIGTERM);
    for (int i = 0; i < 50; ++i)
    {
        pid_t r = ::waitpid(mChildPid, &status, WNOHANG);

        if (r == mChildPid || r < 0)
        {
            mChildPid = -1;
            return;
        }
        ::usleep(10000);
    }

    (void)::kill(mChildPid, SIGKILL);
    (void)::waitpid(mChildPid, &status, 0);
    mChildPid = -1;
}

bool RfSimNode::CompleteInitialSync(void) { return PumpUntilStepDone(); }

void RfSimNode::SetEnabled(bool aEnabled)
{
    mEnabled = aEnabled;
    if (!mEnabled)
    {
        CancelAlarm();
    }
}

bool RfSimNode::SendFrame(const Protocol::Frame &aFrame)
{
    if (mSockFd < 0 || mDisconnected)
    {
        return false;
    }

    std::vector<uint8_t> wire;

    if (!Protocol::EncodeFrame(aFrame, wire))
    {
        return false;
    }

    if (!SendAll(mSockFd, wire.data(), wire.size()))
    {
        mDisconnected = true;
        return false;
    }

    return true;
}

void RfSimNode::CancelAlarm(void)
{
    if (mAlarmEventId != Core::kInvalidEventId)
    {
        mSim.GetScheduler().Cancel(mAlarmEventId);
        mAlarmEventId = Core::kInvalidEventId;
    }
}

void RfSimNode::ScheduleAlarm(uint64_t aTargetUs)
{
    CancelAlarm();
    if (!mEnabled || mDisconnected)
    {
        return;
    }

    auto alive = mAliveFlag;

    mAlarmEventId = mSim.GetScheduler().ScheduleAt(aTargetUs, [this, alive](void) {
        if (!*alive || !mEnabled || mDisconnected)
        {
            return;
        }
        mAlarmEventId = Core::kInvalidEventId;

        Protocol::Frame fire;

        fire.mType        = Protocol::MsgType::kAlarmFired;
        fire.mTimestampUs = mSim.GetNowUs();
        if (SendFrame(fire))
        {
            (void)PumpUntilStepDone();
        }
    });
}

bool RfSimNode::PumpUntilStepDone(Core::RawFrame *aAckOut)
{
    if (mSockFd < 0 || mDisconnected)
    {
        return false;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kStepTimeoutMs);

    while (!mDisconnected)
    {
        // First drain any complete frames already buffered.
        while (!mRxBuffer.empty())
        {
            Protocol::DecodeResult res = Protocol::DecodeFrame(mRxBuffer.data(), mRxBuffer.size());

            if (res.mStatus == Protocol::DecodeStatus::kNeedMoreData)
            {
                break;
            }
            if (res.mStatus != Protocol::DecodeStatus::kOk)
            {
                // Protocol violation or oversized frame -> drop peer immediately.
                mDisconnected = true;
                return false;
            }

            mRxBuffer.erase(mRxBuffer.begin(), mRxBuffer.begin() + static_cast<std::ptrdiff_t>(res.mBytesConsumed));

            bool sawStepDone = false;

            if (!HandleIncomingFrame(res.mFrame, sawStepDone, aAckOut))
            {
                mDisconnected = true;
                return false;
            }
            if (sawStepDone)
            {
                return true;
            }
        }

        auto now = std::chrono::steady_clock::now();

        if (now >= deadline)
        {
            mDisconnected = true;
            return false;
        }

        int remMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        struct pollfd pfd;

        pfd.fd      = mSockFd;
        pfd.events  = POLLIN;
        pfd.revents = 0;

        int pr = ::poll(&pfd, 1, (remMs > 0) ? remMs : 1);

        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            mDisconnected = true;
            return false;
        }
        if (pr == 0)
        {
            continue;
        }

        uint8_t tmp[4096];
        ssize_t n = ::recv(mSockFd, tmp, sizeof(tmp), 0);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            mDisconnected = true;
            return false;
        }
        if (n == 0)
        {
            mDisconnected = true;
            return false;
        }

        if (mRxBuffer.size() + static_cast<size_t>(n) > kMaxRxBufferBytes)
        {
            mDisconnected = true;
            return false;
        }

        mRxBuffer.insert(mRxBuffer.end(), tmp, tmp + n);
    }

    return false;
}

bool RfSimNode::HandleIncomingFrame(const Protocol::Frame &aFrame, bool &aSawStepDone, Core::RawFrame *aAckOut)
{
    aSawStepDone = false;

    switch (aFrame.mType)
    {
    case Protocol::MsgType::kStepDone:
        if (aAckOut != nullptr)
        {
            if (const Protocol::Tlv *ackTlv = aFrame.FindTlv(Protocol::TlvType::kAckPsdu))
            {
                aAckOut->mChannel = mChannel;
                aAckOut->mPsdu    = ackTlv->mValue;
            }
        }
        aSawStepDone = true;
        return true;

    case Protocol::MsgType::kShutdown:
        mDisconnected = true;
        return false;

    case Protocol::MsgType::kAlarmSet:
        if ((aFrame.mFlags & Protocol::kFlagCancel) != 0)
        {
            CancelAlarm();
        }
        else
        {
            uint64_t targetUs = ((aFrame.mFlags & Protocol::kFlagRelativeDelay) != 0)
                                    ? (mSim.GetNowUs() + aFrame.mTimestampUs)
                                    : aFrame.mTimestampUs;

            ScheduleAlarm(targetUs);
        }
        return true;

    case Protocol::MsgType::kRadioState:
    {
        auto rs = aFrame.GetRadioState();
        auto ch = aFrame.GetU8(Protocol::TlvType::kChannel);
        auto pw = aFrame.GetI8(Protocol::TlvType::kPowerDbm);
        auto sn = aFrame.GetI8(Protocol::TlvType::kSensitivityDbm);

        if (ch.has_value())
        {
            mChannel = *ch;
        }
        if (pw.has_value())
        {
            mSim.GetRadioModel().SetNodeTxPower(mId, static_cast<Rf::DbValue>(*pw));
        }
        if (sn.has_value())
        {
            mSim.GetRadioModel().SetNodeRxSensitivity(mId, static_cast<Rf::DbValue>(*sn));
        }
        if (rs.has_value())
        {
            mSim.GetRadioModel().SetNodeRadioState(mId, ByteToRfState(rs->mState), mChannel);
        }
        return true;
    }

    case Protocol::MsgType::kTxStart:
    {
        const Protocol::Tlv *psduTlv = aFrame.FindTlv(Protocol::TlvType::kPsdu);
        auto                 ch      = aFrame.GetU8(Protocol::TlvType::kChannel);
        auto                 txPow   = aFrame.GetI8(Protocol::TlvType::kPowerDbm);
        auto                 ccaThr  = aFrame.GetI8(Protocol::TlvType::kCcaThresholdDbm);
        auto                 msgId   = aFrame.GetU64(Protocol::TlvType::kMsgId);

        if (psduTlv == nullptr || psduTlv->mValue.empty() || psduTlv->mValue.size() > 127)
        {
            return false;
        }

        Core::RawFrame raw;

        raw.mChannel     = ch.value_or(mChannel);
        raw.mTimestampUs = mSim.GetNowUs();
        raw.mPsdu        = psduTlv->mValue;
        mChannel         = raw.mChannel;

        bool        ackReq  = (aFrame.mFlags & Protocol::kFlagAckRequested) != 0;
        bool        csmaCa  = (aFrame.mFlags & Protocol::kFlagCsmaCa) != 0;
        int8_t      ccaDbm  = ccaThr.value_or(-75);
        Rf::DbValue powDbm  = static_cast<Rf::DbValue>(txPow.value_or(0));
        uint64_t    txMsgId = msgId.value_or(0);
        auto        alive   = mAliveFlag;

        mSim.StartRadioTx(mId, raw, ackReq, csmaCa, ccaDbm, powDbm,
                          [this, alive, txMsgId](const std::optional<Core::RawFrame> &aAck, uint8_t aErr) {
                              if (!*alive || !mEnabled || mDisconnected)
                              {
                                  return;
                              }

                              Protocol::Frame done;

                              done.mType        = Protocol::MsgType::kTxDone;
                              done.mTimestampUs = mSim.GetNowUs();
                              done.AddU64(Protocol::TlvType::kMsgId, txMsgId);
                              done.AddU8(Protocol::TlvType::kErrorCode, aErr);
                              if (aAck.has_value() && !aAck->mPsdu.empty())
                              {
                                  done.AddBytes(Protocol::TlvType::kAckPsdu, aAck->mPsdu.data(), aAck->mPsdu.size());
                                  done.AddI8(Protocol::TlvType::kPowerDbm, aAck->mRssiDbm);
                                  done.AddU8(Protocol::TlvType::kLqi, aAck->mLqi);
                              }

                              if (SendFrame(done))
                              {
                                  (void)PumpUntilStepDone();
                              }
                          });
        return true;
    }

    case Protocol::MsgType::kEnergyScan:
    {
        auto ch    = aFrame.GetU8(Protocol::TlvType::kChannel);
        auto durUs = aFrame.GetU64(Protocol::TlvType::kDurationUs);
        auto alive = mAliveFlag;

        mSim.StartEnergyScan(mId, ch.value_or(mChannel), durUs.value_or(1000), [this, alive](int8_t aPeakRssi) {
            if (!*alive || !mEnabled || mDisconnected)
            {
                return;
            }

            Protocol::Frame rsp;

            rsp.mType        = Protocol::MsgType::kEnergyScan;
            rsp.mFlags       = Protocol::kFlagResponse;
            rsp.mTimestampUs = mSim.GetNowUs();
            rsp.AddI8(Protocol::TlvType::kPowerDbm, aPeakRssi);
            if (SendFrame(rsp))
            {
                (void)PumpUntilStepDone();
            }
        });
        return true;
    }

    case Protocol::MsgType::kCliData:
    {
        auto text = aFrame.GetText(Protocol::TlvType::kText);

        if (text.has_value())
        {
            mCliOutput.append(*text);
        }
        return true;
    }

    case Protocol::MsgType::kLogRecord:
    {
        auto text = aFrame.GetText(Protocol::TlvType::kText);

        if (text.has_value())
        {
            if (mLogs.size() >= kMaxStoredLogs)
            {
                mLogs.erase(mLogs.begin());
            }
            mLogs.push_back(*text);
        }
        return true;
    }

    case Protocol::MsgType::kStatusPush:
    {
        auto info = aFrame.GetStatusInfo();

        if (info.has_value())
        {
            char hex[17];

            mRole        = ByteToDeviceRole(info->mRole);
            mRloc16      = info->mRloc16;
            mPartitionId = info->mPartitionId;
            std::snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x%02x%02x", info->mExtAddr[0], info->mExtAddr[1],
                          info->mExtAddr[2], info->mExtAddr[3], info->mExtAddr[4], info->mExtAddr[5], info->mExtAddr[6],
                          info->mExtAddr[7]);
            mExtAddr = hex;
        }
        return true;
    }

    case Protocol::MsgType::kTrelPacket:
    case Protocol::MsgType::kInfraIfPacket:
    case Protocol::MsgType::kHostSocket:
    {
        auto                 addr    = aFrame.GetNetAddr();
        const Protocol::Tlv *payload = aFrame.FindTlv(Protocol::TlvType::kPayload);

        if (addr.has_value() && payload != nullptr)
        {
            Core::NetPacket pkt;

            pkt.mSrcNodeId = mId;
            pkt.mSrcIp6    = addr->mSrcIp6;
            pkt.mDstIp6    = addr->mDstIp6;
            pkt.mSrcPort   = addr->mSrcPort;
            pkt.mDstPort   = addr->mDstPort;
            pkt.mPayload   = payload->mValue;

            if (aFrame.mType == Protocol::MsgType::kTrelPacket)
            {
                mSim.SendTrelPacket(pkt);
            }
            else if (aFrame.mType == Protocol::MsgType::kInfraIfPacket)
            {
                mSim.SendInfraIfPacket(pkt);
            }
            else
            {
                mSim.SendHostUdpPacket(pkt);
            }
        }
        return true;
    }

    default:
        // Ignore unknown / forward-compatible frame types cleanly.
        return true;
    }
}

std::string RfSimNode::ExecuteCli(const std::string &aCommand)
{
    mCliOutput.clear();
    if (!mEnabled || mDisconnected)
    {
        return "Error: external node disconnected\n";
    }

    Protocol::Frame req;

    req.mType        = Protocol::MsgType::kCliData;
    req.mTimestampUs = mSim.GetNowUs();
    req.AddText(Protocol::TlvType::kText, aCommand);

    if (!SendFrame(req) || !PumpUntilStepDone())
    {
        return "Error: external node CLI failed\n";
    }

    return TakeCliOutput();
}

std::string RfSimNode::TakeCliOutput(void)
{
    std::string out;

    out.swap(mCliOutput);

    return out;
}

Core::NodeSnapshot RfSimNode::GetSnapshot(void) const
{
    Core::NodeSnapshot snap;

    snap.mId          = mId;
    snap.mKind        = Core::NodeKind::kExternal;
    snap.mPos         = mPos;
    snap.mRadioRange  = mRadioRange;
    snap.mRole        = mRole;
    snap.mRloc16      = mRloc16;
    snap.mPartitionId = mPartitionId;
    snap.mExtAddr     = mExtAddr;
    snap.mEnabled     = mEnabled && !mDisconnected;

    return snap;
}

bool RfSimNode::OnRadioRxDone(const Core::RawFrame &aRxFrame, Core::RawFrame &aAckOut)
{
    if (!mEnabled || mDisconnected || aRxFrame.mPsdu.empty())
    {
        return false;
    }

    Protocol::Frame rx;

    rx.mType        = Protocol::MsgType::kRxDone;
    rx.mTimestampUs = mSim.GetNowUs();
    rx.AddU8(Protocol::TlvType::kChannel, aRxFrame.mChannel);
    rx.AddI8(Protocol::TlvType::kPowerDbm, aRxFrame.mRssiDbm);
    rx.AddU8(Protocol::TlvType::kLqi, aRxFrame.mLqi);
    rx.AddBytes(Protocol::TlvType::kPsdu, aRxFrame.mPsdu.data(), aRxFrame.mPsdu.size());

    aAckOut.mPsdu.clear();
    if (!SendFrame(rx) || !PumpUntilStepDone(&aAckOut))
    {
        return false;
    }

    return !aAckOut.mPsdu.empty();
}

void RfSimNode::OnTrelPacket(const Core::NetPacket &aPacket)
{
    if (!mEnabled || mDisconnected)
    {
        return;
    }

    Protocol::Frame        msg;
    Protocol::NetAddrValue addr;

    addr.mSrcPort = aPacket.mSrcPort;
    addr.mDstPort = aPacket.mDstPort;
    addr.mSrcIp6  = aPacket.mSrcIp6;
    addr.mDstIp6  = aPacket.mDstIp6;

    msg.mType        = Protocol::MsgType::kTrelPacket;
    msg.mTimestampUs = mSim.GetNowUs();
    msg.AddNetAddr(addr);
    msg.AddBytes(Protocol::TlvType::kPayload, aPacket.mPayload.data(), aPacket.mPayload.size());
    if (SendFrame(msg))
    {
        (void)PumpUntilStepDone();
    }
}

void RfSimNode::OnInfraIfPacket(const Core::NetPacket &aPacket)
{
    if (!mEnabled || mDisconnected)
    {
        return;
    }

    Protocol::Frame        msg;
    Protocol::NetAddrValue addr;

    addr.mSrcPort = aPacket.mSrcPort;
    addr.mDstPort = aPacket.mDstPort;
    addr.mSrcIp6  = aPacket.mSrcIp6;
    addr.mDstIp6  = aPacket.mDstIp6;

    msg.mType        = Protocol::MsgType::kInfraIfPacket;
    msg.mTimestampUs = mSim.GetNowUs();
    msg.AddNetAddr(addr);
    msg.AddBytes(Protocol::TlvType::kPayload, aPacket.mPayload.data(), aPacket.mPayload.size());
    if (SendFrame(msg))
    {
        (void)PumpUntilStepDone();
    }
}

void RfSimNode::OnHostUdpPacket(const Core::NetPacket &aPacket)
{
    if (!mEnabled || mDisconnected)
    {
        return;
    }

    Protocol::Frame        msg;
    Protocol::NetAddrValue addr;

    addr.mSrcPort = aPacket.mSrcPort;
    addr.mDstPort = aPacket.mDstPort;
    addr.mSrcIp6  = aPacket.mSrcIp6;
    addr.mDstIp6  = aPacket.mDstIp6;

    msg.mType        = Protocol::MsgType::kHostSocket;
    msg.mTimestampUs = mSim.GetNowUs();
    msg.AddNetAddr(addr);
    msg.AddBytes(Protocol::TlvType::kPayload, aPacket.mPayload.data(), aPacket.mPayload.size());
    if (SendFrame(msg))
    {
        (void)PumpUntilStepDone();
    }
}

} // namespace Nodes
} // namespace otns
