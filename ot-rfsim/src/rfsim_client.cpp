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

#include "rfsim_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <openthread-core-config.h>
#include <openthread-select.h>
#include <openthread-system.h>
#include <openthread/config.h>
#include <openthread/link.h>
#include <openthread/tasklet.h>
#include <openthread/thread.h>
#include <openthread/platform/alarm-micro.h>
#include <openthread/platform/alarm-milli.h>
#include <openthread/platform/entropy.h>
#include <openthread/platform/memory.h>
#include <openthread/platform/misc.h>
#include <openthread/platform/otns.h>
#include <openthread/platform/settings.h>
#include <openthread/platform/time.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace otns {
namespace RfSim {

namespace {

static constexpr size_t kMaxRxBufferBytes = 256u * 1024u;

uint64_t SplitMix64(uint64_t &aState)
{
    uint64_t z = (aState += 0x9e3779b97f4a7c15ULL);

    z = (z ^ (z >> 30u)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27u)) * 0x94d049bb133111ebULL;

    return z ^ (z >> 31u);
}

uint16_t UpdateCrc16Ccitt(uint16_t aCrc, uint8_t aByte)
{
    uint16_t q = (aCrc ^ aByte) & 0x0Fu;

    aCrc = static_cast<uint16_t>((aCrc >> 4u) ^ (q * 0x1081u));
    q    = (aCrc ^ (aByte >> 4u)) & 0x0Fu;
    aCrc = static_cast<uint16_t>((aCrc >> 4u) ^ (q * 0x1081u));

    return aCrc;
}

void WriteFcs16(otRadioFrame &aFrame)
{
    uint16_t fcs = 0;

    if (aFrame.mLength < 2)
    {
        return;
    }

    for (uint16_t i = 0; i < aFrame.mLength - 2u; ++i)
    {
        fcs = UpdateCrc16Ccitt(fcs, aFrame.mPsdu[i]);
    }

    aFrame.mPsdu[aFrame.mLength - 2u] = static_cast<uint8_t>(fcs & 0xFFu);
    aFrame.mPsdu[aFrame.mLength - 1u] = static_cast<uint8_t>((fcs >> 8u) & 0xFFu);
}

uint8_t ToWireRadioState(otRadioState aState)
{
    switch (aState)
    {
    case OT_RADIO_STATE_SLEEP:
        return 1;
    case OT_RADIO_STATE_RECEIVE:
        return 2;
    case OT_RADIO_STATE_TRANSMIT:
        return 3;
    default:
        return 0;
    }
}

uint8_t ToWireRole(otDeviceRole aRole)
{
    switch (aRole)
    {
    case OT_DEVICE_ROLE_DETACHED:
        return 1;
    case OT_DEVICE_ROLE_CHILD:
        return 2;
    case OT_DEVICE_ROLE_ROUTER:
        return 3;
    case OT_DEVICE_ROLE_LEADER:
        return 4;
    default:
        return 0;
    }
}

bool SendAllFd(int aFd, const uint8_t *aData, size_t aLength)
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

int ConnectEndpoint(const std::string &aEndpoint)
{
    if (aEndpoint.rfind("tcp://", 0) == 0)
    {
        std::string hostPort = aEndpoint.substr(6);
        size_t      colon    = hostPort.rfind(':');

        if (colon == std::string::npos)
        {
            return -1;
        }

        std::string host = hostPort.substr(0, colon);
        long        port = std::strtol(hostPort.substr(colon + 1).c_str(), nullptr, 10);

        if (port <= 0 || port > 65535)
        {
            return -1;
        }

        struct sockaddr_in addr;

        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(port));
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        {
            return -1;
        }

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0)
        {
            return -1;
        }

        int one = 1;

        (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

        if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
        {
            ::close(fd);
            return -1;
        }

        return fd;
    }

    std::string unixPath = aEndpoint;

    if (unixPath.rfind("unix://", 0) == 0)
    {
        unixPath = unixPath.substr(7);
    }

    struct sockaddr_un addr;

    if (unixPath.empty() || unixPath.size() >= sizeof(addr.sun_path))
    {
        return -1;
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
    {
        return -1;
    }

#ifdef SO_NOSIGPIPE
    int one = 1;

    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, unixPath.c_str(), unixPath.size() + 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        return -1;
    }

    return fd;
}

} // namespace

Client &Client::Get(void)
{
    static Client sInstance;

    return sInstance;
}

Client::Client(void)
{
    std::memset(&mRadioCtx, 0, sizeof(mRadioCtx));
    mRadioCtx.mShortAddress          = OT_RADIO_INVALID_SHORT_ADDR;
    mRadioCtx.mAlternateShortAddress = OT_RADIO_INVALID_SHORT_ADDR;

    std::memset(&mTxFrame, 0, sizeof(mTxFrame));
    mTxFrame.mPsdu = mTxPsdu.data();
}

void Client::Configure(const std::string &aEndpoint, uint32_t aNodeId, uint64_t aSeed)
{
    mEndpoint  = aEndpoint;
    mNodeId    = aNodeId;
    mPrngState = (aSeed != 0) ? aSeed : 1;
}

bool Client::Connect(void)
{
    Disconnect();
    mDisconnected    = false;
    mHasPushedStatus = false;
    mLastSentAlarmUs.reset();
    mAlarmMilliUs.reset();
    mAlarmMicroUs.reset();
    mRxBuffer.clear();

    mSockFd = ConnectEndpoint(mEndpoint);
    if (mSockFd < 0)
    {
        mDisconnected = true;
        return false;
    }

    Protocol::Frame hello;

    hello.mType        = Protocol::MsgType::kHello;
    hello.mTimestampUs = 0;
    hello.AddBytes(Protocol::TlvType::kMagic, Protocol::kProtocolMagic.data(), Protocol::kProtocolMagic.size());
    hello.AddVersion(Protocol::kProtocolMajor, Protocol::kProtocolMinor);
    hello.AddU32(Protocol::TlvType::kNodeId, mNodeId);
    hello.AddText(Protocol::TlvType::kText, "ot-rfsim-v3");

    if (!SendFrame(hello))
    {
        Disconnect();
        return false;
    }

    // Wait for HelloAck.
    while (!mDisconnected)
    {
        Protocol::DecodeResult res = Protocol::DecodeFrame(mRxBuffer.data(), mRxBuffer.size());

        if (res.mStatus == Protocol::DecodeStatus::kOk)
        {
            mRxBuffer.erase(mRxBuffer.begin(), mRxBuffer.begin() + static_cast<std::ptrdiff_t>(res.mBytesConsumed));
            if (res.mFrame.mType != Protocol::MsgType::kHelloAck)
            {
                Disconnect();
                return false;
            }
            mNowUs = res.mFrame.mTimestampUs;
            if (auto id = res.mFrame.GetU32(Protocol::TlvType::kNodeId))
            {
                mNodeId = *id;
            }
            if (auto seed = res.mFrame.GetU64(Protocol::TlvType::kSeed))
            {
                mPrngState = (*seed != 0) ? *seed : 1;
            }
            return true;
        }

        if (res.mStatus != Protocol::DecodeStatus::kNeedMoreData)
        {
            Disconnect();
            return false;
        }

        if (!RecvMoreBytes(5000))
        {
            Disconnect();
            return false;
        }
    }

    return false;
}

void Client::Disconnect(void)
{
    if (mSockFd >= 0)
    {
        if (!mDisconnected)
        {
            Protocol::Frame bye;

            bye.mType        = Protocol::MsgType::kShutdown;
            bye.mTimestampUs = mNowUs;
            (void)SendFrame(bye);
        }
        ::close(mSockFd);
        mSockFd = -1;
    }
    mDisconnected = true;
}

uint8_t Client::NextRandomByte(void) { return static_cast<uint8_t>(SplitMix64(mPrngState) & 0xFFu); }

void Client::SetCliHandler(otRfSimCliInputCallback aCallback, void *aContext)
{
    mCliCallback = aCallback;
    mCliContext  = aContext;
}

void Client::SendCliOutput(const char *aText, size_t aLength)
{
    if (aText == nullptr || aLength == 0 || mDisconnected)
    {
        return;
    }

    Protocol::Frame msg;

    msg.mType        = Protocol::MsgType::kCliData;
    msg.mTimestampUs = mNowUs;
    msg.AddBytes(Protocol::TlvType::kText, reinterpret_cast<const uint8_t *>(aText), aLength);
    (void)SendFrame(msg);
}

bool Client::SendFrame(const Protocol::Frame &aFrame)
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

    if (!SendAllFd(mSockFd, wire.data(), wire.size()))
    {
        mDisconnected = true;
        return false;
    }

    return true;
}

bool Client::RecvMoreBytes(int aTimeoutMs)
{
    if (mSockFd < 0 || mDisconnected)
    {
        return false;
    }

    struct pollfd pfd;

    pfd.fd      = mSockFd;
    pfd.events  = POLLIN;
    pfd.revents = 0;

    int pr = ::poll(&pfd, 1, aTimeoutMs);

    if (pr <= 0)
    {
        if (pr < 0 && errno == EINTR)
        {
            return true;
        }
        if (pr < 0)
        {
            mDisconnected = true;
        }
        return false;
    }

    uint8_t tmp[4096];
    ssize_t n = ::recv(mSockFd, tmp, sizeof(tmp), 0);

    if (n <= 0)
    {
        if (n < 0 && errno == EINTR)
        {
            return true;
        }
        mDisconnected = true;
        return false;
    }

    if (mRxBuffer.size() + static_cast<size_t>(n) > kMaxRxBufferBytes)
    {
        mDisconnected = true;
        return false;
    }

    mRxBuffer.insert(mRxBuffer.end(), tmp, tmp + n);

    return true;
}

bool Client::CompleteStartup(otInstance *aInstance)
{
    mInstance = aInstance;
    DrainTasklets(aInstance);
    SyncAlarmToSim();
    SyncStatusToSim(aInstance, true);
    SendStepDone();

    return !mDisconnected;
}

void Client::ProcessDrivers(otInstance *aInstance)
{
    mInstance = aInstance;

    if (mDisconnected)
    {
        return;
    }

    DrainFrames(aInstance);
    if (!mDisconnected && mRxBuffer.empty())
    {
        if (RecvMoreBytes(250))
        {
            DrainFrames(aInstance);
        }
    }
}

void Client::ProcessReadySocket(otInstance *aInstance)
{
    mInstance = aInstance;

    if (mDisconnected)
    {
        return;
    }

    (void)RecvMoreBytes(0);
    DrainFrames(aInstance);
}

void Client::DrainFrames(otInstance *aInstance)
{
    while (!mDisconnected && !mRxBuffer.empty())
    {
        Protocol::DecodeResult res = Protocol::DecodeFrame(mRxBuffer.data(), mRxBuffer.size());

        if (res.mStatus == Protocol::DecodeStatus::kNeedMoreData)
        {
            break;
        }
        if (res.mStatus != Protocol::DecodeStatus::kOk)
        {
            mDisconnected = true;
            break;
        }

        mRxBuffer.erase(mRxBuffer.begin(), mRxBuffer.begin() + static_cast<std::ptrdiff_t>(res.mBytesConsumed));
        HandleFrame(aInstance, res.mFrame);
    }
}

void Client::DrainTasklets(otInstance *aInstance)
{
    if (aInstance == nullptr)
    {
        return;
    }

    while (mHasPendingTasklets || otTaskletsArePending(aInstance))
    {
        mHasPendingTasklets = false;
        otTaskletsProcess(aInstance);
    }
}

void Client::SyncAlarmToSim(void)
{
    std::optional<uint64_t> earliest;

    if (mAlarmMilliUs.has_value())
    {
        earliest = mAlarmMilliUs;
    }
    if (mAlarmMicroUs.has_value() && (!earliest.has_value() || *mAlarmMicroUs < *earliest))
    {
        earliest = mAlarmMicroUs;
    }

    if (earliest == mLastSentAlarmUs)
    {
        return;
    }

    Protocol::Frame msg;

    msg.mType = Protocol::MsgType::kAlarmSet;
    if (earliest.has_value())
    {
        msg.mTimestampUs = *earliest;
    }
    else
    {
        msg.mFlags       = Protocol::kFlagCancel;
        msg.mTimestampUs = mNowUs;
    }

    mLastSentAlarmUs = earliest;
    (void)SendFrame(msg);
}

void Client::SyncStatusToSim(otInstance *aInstance, bool aForce)
{
    if (aInstance == nullptr)
    {
        return;
    }

    Protocol::StatusInfoValue info;

    info.mRole        = ToWireRole(otThreadGetDeviceRole(aInstance));
    info.mRloc16      = otThreadGetRloc16(aInstance);
    info.mPartitionId = otThreadGetPartitionId(aInstance);

    if (const otExtAddress *ext = otLinkGetExtendedAddress(aInstance))
    {
        std::memcpy(info.mExtAddr.data(), ext->m8, 8);
    }

    bool changed = !mHasPushedStatus || (info.mRole != mLastStatus.mRole) || (info.mRloc16 != mLastStatus.mRloc16) ||
                   (info.mPartitionId != mLastStatus.mPartitionId) || (info.mExtAddr != mLastStatus.mExtAddr);

    if (!aForce && !changed)
    {
        return;
    }

    mHasPushedStatus = true;
    mLastStatus      = info;

    Protocol::Frame msg;

    msg.mType        = Protocol::MsgType::kStatusPush;
    msg.mTimestampUs = mNowUs;
    msg.AddStatusInfo(info);
    (void)SendFrame(msg);
}

void Client::SendRadioState(void)
{
    Protocol::Frame msg;

    msg.mType        = Protocol::MsgType::kRadioState;
    msg.mTimestampUs = mNowUs;
    msg.AddRadioState(ToWireRadioState(mRadioState), 0);
    msg.AddU8(Protocol::TlvType::kChannel, mChannel);
    msg.AddI8(Protocol::TlvType::kPowerDbm, mTxPowerDbm);
    msg.AddI8(Protocol::TlvType::kSensitivityDbm, mRxSensitivityDbm);
    (void)SendFrame(msg);
}

void Client::SendStepDone(const std::vector<uint8_t> *aAckPsdu)
{
    Protocol::Frame done;

    done.mType        = Protocol::MsgType::kStepDone;
    done.mTimestampUs = mNowUs;
    if (aAckPsdu != nullptr && !aAckPsdu->empty())
    {
        done.AddBytes(Protocol::TlvType::kAckPsdu, aAckPsdu->data(), aAckPsdu->size());
    }
    (void)SendFrame(done);
}

void Client::HandleFrame(otInstance *aInstance, const Protocol::Frame &aFrame)
{
    if (aFrame.mTimestampUs > mNowUs)
    {
        mNowUs = aFrame.mTimestampUs;
    }

    switch (aFrame.mType)
    {
    case Protocol::MsgType::kShutdown:
        mDisconnected = true;
        return;

    case Protocol::MsgType::kAlarmFired:
    {
        mLastSentAlarmUs.reset();
        if (mAlarmMicroUs.has_value() && *mAlarmMicroUs <= mNowUs)
        {
            mAlarmMicroUs.reset();
            if (aInstance != nullptr)
            {
                otPlatAlarmMicroFired(aInstance);
            }
        }
        if (mAlarmMilliUs.has_value() && *mAlarmMilliUs <= mNowUs)
        {
            mAlarmMilliUs.reset();
            if (aInstance != nullptr)
            {
                otPlatAlarmMilliFired(aInstance);
            }
        }
        DrainTasklets(aInstance);
        SyncAlarmToSim();
        SyncStatusToSim(aInstance);
        SendStepDone();
        return;
    }

    case Protocol::MsgType::kTxDone:
    {
        uint8_t              err    = aFrame.GetU8(Protocol::TlvType::kErrorCode).value_or(OT_ERROR_ABORT);
        const Protocol::Tlv *ackTlv = aFrame.FindTlv(Protocol::TlvType::kAckPsdu);

        mRadioState = OT_RADIO_STATE_RECEIVE;
        SendRadioState();

        if (aInstance != nullptr)
        {
            if (ackTlv != nullptr && !ackTlv->mValue.empty() && err == OT_ERROR_NONE)
            {
                std::array<uint8_t, OT_RADIO_FRAME_MAX_SIZE> ackBuf{};
                size_t                                       len = std::min(ackTlv->mValue.size(), ackBuf.size());
                otRadioFrame                                 otAck;

                std::memcpy(ackBuf.data(), ackTlv->mValue.data(), len);
                std::memset(&otAck, 0, sizeof(otAck));
                otAck.mPsdu                    = ackBuf.data();
                otAck.mLength                  = static_cast<uint16_t>(len);
                otAck.mChannel                 = mChannel;
                otAck.mInfo.mRxInfo.mTimestamp = mNowUs;
                otAck.mInfo.mRxInfo.mRssi      = aFrame.GetI8(Protocol::TlvType::kPowerDbm).value_or(-60);
                otAck.mInfo.mRxInfo.mLqi       = aFrame.GetU8(Protocol::TlvType::kLqi).value_or(255);
                otPlatRadioTxDone(aInstance, &mTxFrame, &otAck, OT_ERROR_NONE);
            }
            else
            {
                otPlatRadioTxDone(aInstance, &mTxFrame, nullptr, static_cast<otError>(err));
            }
        }

        DrainTasklets(aInstance);
        SyncAlarmToSim();
        SyncStatusToSim(aInstance);
        SendStepDone();
        return;
    }

    case Protocol::MsgType::kRxDone:
    {
        const Protocol::Tlv *psduTlv = aFrame.FindTlv(Protocol::TlvType::kPsdu);
        uint8_t              ch      = aFrame.GetU8(Protocol::TlvType::kChannel).value_or(mChannel);
        int8_t               rssi    = aFrame.GetI8(Protocol::TlvType::kPowerDbm).value_or(-60);
        uint8_t              lqi     = aFrame.GetU8(Protocol::TlvType::kLqi).value_or(255);
        std::vector<uint8_t> ackPsdu;

        mLastRxRssiDbm = rssi;

        if (aInstance != nullptr && mRadioState == OT_RADIO_STATE_RECEIVE && ch == mChannel && psduTlv != nullptr &&
            psduTlv->mValue.size() >= 2 && psduTlv->mValue.size() <= OT_RADIO_FRAME_MAX_SIZE)
        {
            std::array<uint8_t, OT_RADIO_FRAME_MAX_SIZE> psduCopy{};
            otRadioFrame                                 otRx;

            std::memcpy(psduCopy.data(), psduTlv->mValue.data(), psduTlv->mValue.size());
            std::memset(&otRx, 0, sizeof(otRx));
            otRx.mPsdu                    = psduCopy.data();
            otRx.mLength                  = static_cast<uint16_t>(psduTlv->mValue.size());
            otRx.mChannel                 = ch;
            otRx.mInfo.mRxInfo.mTimestamp = mNowUs;
            otRx.mInfo.mRxInfo.mRssi      = rssi;
            otRx.mInfo.mRxInfo.mLqi       = lqi;

            bool addrMatch = otMacFrameDoesAddrMatchAny(&otRx, mPanId, mRadioCtx.mShortAddress,
                                                        mRadioCtx.mAlternateShortAddress, &mRadioCtx.mExtAddress);

            if (addrMatch || mPromiscuous)
            {
                if (addrMatch && otMacFrameIsAckRequested(&otRx))
                {
                    otMacAddress dstAddr;
                    bool         hasDst = (otMacFrameGetDstAddr(&otRx, &dstAddr) == OT_ERROR_NONE);
                    bool         isUnicast =
                        hasDst && dstAddr.mType != OT_MAC_ADDRESS_TYPE_NONE &&
                        !(dstAddr.mType == OT_MAC_ADDRESS_TYPE_SHORT && dstAddr.mAddress.mShortAddress == 0xFFFF);

                    if (isUnicast)
                    {
                        bool framePending = false;

                        if (otMacFrameIsDataRequest(&otRx))
                        {
                            otMacAddress srcAddr;

                            if (otMacFrameGetSrcAddr(&otRx, &srcAddr) == OT_ERROR_NONE)
                            {
                                framePending = HasFramePendingFor(srcAddr);
                            }
                        }
                        otRx.mInfo.mRxInfo.mAckedWithFramePending = framePending;

                        std::array<uint8_t, OT_RADIO_FRAME_MAX_SIZE> ackBuf{};
                        otRadioFrame                                 otAck;
                        bool                                         ackOk = false;

                        std::memset(&otAck, 0, sizeof(otAck));
                        otAck.mPsdu    = ackBuf.data();
                        otAck.mChannel = mChannel;

                        if (otMacFrameIsVersion2015(&otRx))
                        {
                            uint8_t ieData[OT_ACK_IE_MAX_SIZE];
                            uint8_t ieLen = 0;

#if OPENTHREAD_CONFIG_MAC_CSL_RECEIVER_ENABLE
                            if (mRadioCtx.mCslPeriod > 0 && otMacFrameSrcAddrMatchCslReceiverPeer(&otRx, &mRadioCtx))
                            {
                                ieLen = otMacFrameGenerateCslIeTemplate(ieData);
                            }
#endif
                            if (otMacFrameGenerateEnhAck(&otRx, framePending, ieData, ieLen, &otAck) == OT_ERROR_NONE &&
                                otMacFrameProcessTxSfd(&otAck, mNowUs, &mRadioCtx) == OT_ERROR_NONE)
                            {
                                ackOk = true;
                            }
                        }
                        else
                        {
                            otMacFrameGenerateImmAck(&otRx, framePending, &otAck);
                            ackOk = true;
                        }

                        if (ackOk)
                        {
                            WriteFcs16(otAck);
                            ackPsdu.assign(otAck.mPsdu, otAck.mPsdu + otAck.mLength);
                        }
                    }
                }

                otPlatRadioReceiveDone(aInstance, &otRx, OT_ERROR_NONE);
            }
        }

        DrainTasklets(aInstance);
        SyncAlarmToSim();
        SyncStatusToSim(aInstance);
        SendStepDone(&ackPsdu);
        return;
    }

    case Protocol::MsgType::kEnergyScan:
    {
        int8_t peakRssi = aFrame.GetI8(Protocol::TlvType::kPowerDbm).value_or(OT_RADIO_RSSI_INVALID);

        if (aInstance != nullptr)
        {
            otPlatRadioEnergyScanDone(aInstance, peakRssi);
        }
        DrainTasklets(aInstance);
        SyncAlarmToSim();
        SyncStatusToSim(aInstance);
        SendStepDone();
        return;
    }

    case Protocol::MsgType::kCliData:
    {
        auto line = aFrame.GetText(Protocol::TlvType::kText);

        if (line.has_value() && mCliCallback != nullptr)
        {
            mCliCallback(line->c_str(), mCliContext);
        }
        DrainTasklets(aInstance);
        SyncAlarmToSim();
        SyncStatusToSim(aInstance);
        SendStepDone();
        return;
    }

    default:
        DrainTasklets(aInstance);
        SyncAlarmToSim();
        SyncStatusToSim(aInstance);
        SendStepDone();
        return;
    }
}

void Client::StartAlarmMilliAt(uint32_t aT0Ms, uint32_t aDtMs)
{
    uint64_t nowMs   = mNowUs / 1000ULL;
    uint32_t nowMs32 = static_cast<uint32_t>(nowMs);
    int32_t  deltaMs = static_cast<int32_t>((aT0Ms + aDtMs) - nowMs32);

    mAlarmMilliUs = (deltaMs <= 0) ? mNowUs : ((nowMs + static_cast<uint64_t>(deltaMs)) * 1000ULL);
}

void Client::StopAlarmMilli(void) { mAlarmMilliUs.reset(); }

void Client::StartAlarmMicroAt(uint32_t aT0Us, uint32_t aDtUs)
{
    uint32_t nowUs32 = static_cast<uint32_t>(mNowUs);
    int32_t  deltaUs = static_cast<int32_t>((aT0Us + aDtUs) - nowUs32);

    mAlarmMicroUs = (deltaUs <= 0) ? mNowUs : (mNowUs + static_cast<uint64_t>(deltaUs));
}

void Client::StopAlarmMicro(void) { mAlarmMicroUs.reset(); }

otError Client::RadioEnable(void)
{
    if (mRadioState != OT_RADIO_STATE_DISABLED)
    {
        return OT_ERROR_FAILED;
    }

    mRadioState = OT_RADIO_STATE_SLEEP;
    SendRadioState();

    return OT_ERROR_NONE;
}

otError Client::RadioDisable(void)
{
    mRadioState = OT_RADIO_STATE_DISABLED;
    SendRadioState();

    return OT_ERROR_NONE;
}

otError Client::RadioSleep(void)
{
    if (mRadioState == OT_RADIO_STATE_DISABLED)
    {
        return OT_ERROR_INVALID_STATE;
    }
    if (mRadioState == OT_RADIO_STATE_TRANSMIT)
    {
        return OT_ERROR_BUSY;
    }

    mRadioState = OT_RADIO_STATE_SLEEP;
    SendRadioState();

    return OT_ERROR_NONE;
}

otError Client::RadioReceive(uint8_t aChannel)
{
    if (mRadioState == OT_RADIO_STATE_DISABLED)
    {
        return OT_ERROR_INVALID_STATE;
    }

    mChannel    = aChannel;
    mRadioState = OT_RADIO_STATE_RECEIVE;
    SendRadioState();

    return OT_ERROR_NONE;
}

void Client::SetExtendedAddress(const otExtAddress *aExtAddr)
{
    if (aExtAddr == nullptr)
    {
        return;
    }

    for (size_t i = 0; i < 8; ++i)
    {
        mRadioCtx.mExtAddress.m8[i] = aExtAddr->m8[7 - i];
    }
}

void Client::SetTxPower(int8_t aPowerDbm)
{
    mTxPowerDbm = aPowerDbm;
    SendRadioState();
}

otError Client::AddSrcMatchShort(otShortAddress aAddr)
{
    if (std::find(mSrcMatchShort.begin(), mSrcMatchShort.end(), aAddr) == mSrcMatchShort.end())
    {
        mSrcMatchShort.push_back(aAddr);
    }

    return OT_ERROR_NONE;
}

otError Client::AddSrcMatchExt(const otExtAddress *aAddr)
{
    std::array<uint8_t, 8> rev{};

    if (aAddr == nullptr)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    for (size_t i = 0; i < 8; ++i)
    {
        rev[i] = aAddr->m8[7 - i];
    }

    if (std::find(mSrcMatchExt.begin(), mSrcMatchExt.end(), rev) == mSrcMatchExt.end())
    {
        mSrcMatchExt.push_back(rev);
    }

    return OT_ERROR_NONE;
}

otError Client::ClearSrcMatchShort(otShortAddress aAddr)
{
    auto it = std::find(mSrcMatchShort.begin(), mSrcMatchShort.end(), aAddr);

    if (it == mSrcMatchShort.end())
    {
        return OT_ERROR_NO_ADDRESS;
    }

    mSrcMatchShort.erase(it);

    return OT_ERROR_NONE;
}

otError Client::ClearSrcMatchExt(const otExtAddress *aAddr)
{
    std::array<uint8_t, 8> rev{};

    if (aAddr == nullptr)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    for (size_t i = 0; i < 8; ++i)
    {
        rev[i] = aAddr->m8[7 - i];
    }

    auto it = std::find(mSrcMatchExt.begin(), mSrcMatchExt.end(), rev);

    if (it == mSrcMatchExt.end())
    {
        return OT_ERROR_NO_ADDRESS;
    }

    mSrcMatchExt.erase(it);

    return OT_ERROR_NONE;
}

bool Client::HasFramePendingFor(const otMacAddress &aSrcAddr) const
{
    if (!mSrcMatchEnabled)
    {
        return true;
    }

    if (aSrcAddr.mType == OT_MAC_ADDRESS_TYPE_SHORT)
    {
        return std::find(mSrcMatchShort.begin(), mSrcMatchShort.end(), aSrcAddr.mAddress.mShortAddress) !=
               mSrcMatchShort.end();
    }

    if (aSrcAddr.mType == OT_MAC_ADDRESS_TYPE_EXTENDED)
    {
        std::array<uint8_t, 8> key{};

        std::memcpy(key.data(), aSrcAddr.mAddress.mExtAddress.m8, 8);

        return std::find(mSrcMatchExt.begin(), mSrcMatchExt.end(), key) != mSrcMatchExt.end();
    }

    return false;
}

void Client::SetMacKey(uint8_t                 aKeyIdMode,
                       uint8_t                 aKeyIndex,
                       const otMacKeyMaterial *aPrevKey,
                       const otMacKeyMaterial *aCurrKey,
                       const otMacKeyMaterial *aNextKey,
                       otRadioKeyType          aKeyType)
{
    (void)aKeyIdMode;

    mRadioCtx.mKeyId   = aKeyIndex;
    mRadioCtx.mKeyType = aKeyType;

    if (!mMacFcReset)
    {
        mRadioCtx.mPrevMacFrameCounter = mRadioCtx.mMacFrameCounter;
        mRadioCtx.mMacFrameCounter     = 0;
    }
    mMacFcReset = false;

    if (aPrevKey != nullptr)
    {
        std::memcpy(&mRadioCtx.mPrevKey, aPrevKey, sizeof(otMacKeyMaterial));
    }
    if (aCurrKey != nullptr)
    {
        std::memcpy(&mRadioCtx.mCurrKey, aCurrKey, sizeof(otMacKeyMaterial));
    }
    if (aNextKey != nullptr)
    {
        std::memcpy(&mRadioCtx.mNextKey, aNextKey, sizeof(otMacKeyMaterial));
    }
}

void Client::SetMacFrameCounter(uint32_t aCounter)
{
    if (aCounter == 0)
    {
        mRadioCtx.mPrevMacFrameCounter = mRadioCtx.mMacFrameCounter;
        mMacFcReset                    = true;
    }
    mRadioCtx.mMacFrameCounter = aCounter;
}

void Client::SetMacFrameCounterIfLarger(uint32_t aCounter)
{
    if (aCounter > mRadioCtx.mMacFrameCounter)
    {
        mRadioCtx.mMacFrameCounter = aCounter;
    }
}

void Client::SetCslParams(uint32_t aPeriod, otShortAddress aShortAddr, const otExtAddress *aExtAddr)
{
    mRadioCtx.mCslPeriod       = static_cast<uint16_t>(aPeriod);
    mRadioCtx.mCslShortAddress = aShortAddr;
    if (aExtAddr != nullptr)
    {
        for (size_t i = 0; i < 8; ++i)
        {
            mRadioCtx.mCslExtAddress.m8[i] = aExtAddr->m8[7 - i];
        }
    }
}

otError Client::RadioTransmit(otRadioFrame *aFrame)
{
    if (mRadioState != OT_RADIO_STATE_RECEIVE || aFrame != &mTxFrame)
    {
        return OT_ERROR_INVALID_STATE;
    }

    mChannel    = aFrame->mChannel;
    mRadioState = OT_RADIO_STATE_TRANSMIT;
    SendRadioState();

    if (otMacFrameProcessTxSfd(aFrame, mNowUs, &mRadioCtx) != OT_ERROR_NONE)
    {
        mRadioState = OT_RADIO_STATE_RECEIVE;
        SendRadioState();
        return OT_ERROR_ABORT;
    }

    WriteFcs16(*aFrame);
    if (mInstance != nullptr)
    {
        otPlatRadioTxStarted(mInstance, aFrame);
    }

    otMacAddress dstAddr;
    bool         hasDst  = (otMacFrameGetDstAddr(aFrame, &dstAddr) == OT_ERROR_NONE);
    bool         isBcast = !hasDst || (dstAddr.mType == OT_MAC_ADDRESS_TYPE_NONE) ||
                   (dstAddr.mType == OT_MAC_ADDRESS_TYPE_SHORT && dstAddr.mAddress.mShortAddress == 0xFFFF);
    bool ackReq = otMacFrameIsAckRequested(aFrame) && !isBcast;

    Protocol::Frame tx;

    tx.mType        = Protocol::MsgType::kTxStart;
    tx.mTimestampUs = mNowUs;
    tx.mFlags       = static_cast<uint16_t>((ackReq ? Protocol::kFlagAckRequested : 0u) |
                                            (aFrame->mInfo.mTxInfo.mCsmaCaEnabled ? Protocol::kFlagCsmaCa : 0u));
    tx.AddU8(Protocol::TlvType::kChannel, aFrame->mChannel);
    tx.AddI8(Protocol::TlvType::kPowerDbm, mTxPowerDbm);
    tx.AddI8(Protocol::TlvType::kCcaThresholdDbm, mCcaThresholdDbm);
    tx.AddU64(Protocol::TlvType::kMsgId, mNextTxMsgId++);
    tx.AddBytes(Protocol::TlvType::kPsdu, aFrame->mPsdu, aFrame->mLength);

    if (!SendFrame(tx))
    {
        return OT_ERROR_ABORT;
    }

    return OT_ERROR_NONE;
}

otError Client::RadioEnergyScan(uint8_t aChannel, uint16_t aDurationMs)
{
    if (mRadioState == OT_RADIO_STATE_DISABLED)
    {
        return OT_ERROR_INVALID_STATE;
    }

    Protocol::Frame req;

    req.mType        = Protocol::MsgType::kEnergyScan;
    req.mTimestampUs = mNowUs;
    req.AddU8(Protocol::TlvType::kChannel, aChannel);
    req.AddU64(Protocol::TlvType::kDurationUs, static_cast<uint64_t>(aDurationMs) * 1000ULL);

    return SendFrame(req) ? OT_ERROR_NONE : OT_ERROR_FAILED;
}

otError Client::SettingsGet(uint16_t aKey, int aIndex, uint8_t *aValue, uint16_t *aValueLen) const
{
    auto it = mSettings.find(aKey);

    if (it == mSettings.end() || aIndex < 0 || static_cast<size_t>(aIndex) >= it->second.size())
    {
        return OT_ERROR_NOT_FOUND;
    }

    const auto &entry = it->second[static_cast<size_t>(aIndex)];

    if (aValueLen != nullptr)
    {
        uint16_t maxLen = *aValueLen;
        uint16_t len    = static_cast<uint16_t>(entry.size());

        *aValueLen = len;
        if (aValue != nullptr && len > 0)
        {
            std::memcpy(aValue, entry.data(), std::min(maxLen, len));
        }
    }

    return OT_ERROR_NONE;
}

otError Client::SettingsSet(uint16_t aKey, const uint8_t *aValue, uint16_t aValueLen)
{
    auto                &vec = mSettings[aKey];
    std::vector<uint8_t> bytes;

    vec.clear();
    if (aValue != nullptr && aValueLen > 0)
    {
        bytes.assign(aValue, aValue + aValueLen);
    }
    vec.push_back(std::move(bytes));

    return OT_ERROR_NONE;
}

otError Client::SettingsAdd(uint16_t aKey, const uint8_t *aValue, uint16_t aValueLen)
{
    auto                &vec = mSettings[aKey];
    std::vector<uint8_t> bytes;

    if (aValue != nullptr && aValueLen > 0)
    {
        bytes.assign(aValue, aValue + aValueLen);
    }
    vec.insert(vec.begin(), std::move(bytes));

    return OT_ERROR_NONE;
}

otError Client::SettingsDelete(uint16_t aKey, int aIndex)
{
    auto it = mSettings.find(aKey);

    if (it == mSettings.end())
    {
        return OT_ERROR_NOT_FOUND;
    }

    if (aIndex < 0)
    {
        mSettings.erase(it);
        return OT_ERROR_NONE;
    }

    if (static_cast<size_t>(aIndex) >= it->second.size())
    {
        return OT_ERROR_NOT_FOUND;
    }

    it->second.erase(it->second.begin() + aIndex);
    if (it->second.empty())
    {
        mSettings.erase(it);
    }

    return OT_ERROR_NONE;
}

void Client::SendLog(otLogLevel aLevel, const char *aLine)
{
    if (aLine == nullptr || mDisconnected)
    {
        return;
    }

    Protocol::Frame msg;

    msg.mType        = Protocol::MsgType::kLogRecord;
    msg.mTimestampUs = mNowUs;
    msg.AddU8(Protocol::TlvType::kErrorCode, static_cast<uint8_t>(aLevel));
    msg.AddText(Protocol::TlvType::kText, aLine);
    (void)SendFrame(msg);
}

} // namespace RfSim
} // namespace otns

using otns::RfSim::Client;

extern "C" {

//---------------------------------------------------------------------------------------------------------------------
// Public ot-rfsim v3 Library API

void otRfSimConfigure(const char *aEndpoint, uint32_t aNodeId, uint64_t aSeed)
{
    Client::Get().Configure((aEndpoint != nullptr) ? aEndpoint : "", aNodeId, aSeed);
}

bool otRfSimConnect(void) { return Client::Get().Connect(); }

void otRfSimDisconnect(void) { Client::Get().Disconnect(); }

bool otRfSimIsConnected(void) { return Client::Get().IsConnected(); }

int otRfSimGetSocketFd(void) { return Client::Get().GetSocketFd(); }

uint32_t otRfSimGetNodeId(void) { return Client::Get().GetNodeId(); }

void otRfSimAttachInstance(otInstance *aInstance) { Client::Get().AttachInstance(aInstance); }

void otRfSimSetCliHandler(otRfSimCliInputCallback aCallback, void *aContext)
{
    Client::Get().SetCliHandler(aCallback, aContext);
}

void otRfSimSendCliOutput(const char *aText, size_t aLength) { Client::Get().SendCliOutput(aText, aLength); }

bool otRfSimCompleteStartup(otInstance *aInstance) { return Client::Get().CompleteStartup(aInstance); }

//---------------------------------------------------------------------------------------------------------------------
// OpenThread System & Select API (for Standalone CLI, OTBR, and Matter Library Mode)

void otSysInit(int aArgCount, char *aArgVector[])
{
    if (Client::Get().PseudoResetRequested())
    {
        Client::Get().ClearPseudoReset();
        return;
    }

    std::string endpoint;
    uint32_t    nodeId = 0;
    uint64_t    seed   = 1;

    for (int i = 1; i < aArgCount; ++i)
    {
        if (std::strcmp(aArgVector[i], "--endpoint") == 0 && i + 1 < aArgCount)
        {
            endpoint = aArgVector[++i];
        }
        else if (std::strcmp(aArgVector[i], "--socket") == 0 && i + 1 < aArgCount)
        {
            endpoint = aArgVector[++i];
        }
        else if (std::strcmp(aArgVector[i], "--node-id") == 0 && i + 1 < aArgCount)
        {
            nodeId = static_cast<uint32_t>(std::strtoul(aArgVector[++i], nullptr, 0));
        }
        else if (std::strcmp(aArgVector[i], "--seed") == 0 && i + 1 < aArgCount)
        {
            seed = static_cast<uint64_t>(std::strtoull(aArgVector[++i], nullptr, 0));
        }
    }

    if (!endpoint.empty())
    {
        Client::Get().Configure(endpoint, nodeId, seed);
    }

    (void)Client::Get().Connect();
}

void otSysDeinit(void) { Client::Get().Disconnect(); }

bool otSysPseudoResetWasRequested(void) { return Client::Get().PseudoResetRequested(); }

void otSysProcessDrivers(otInstance *aInstance) { Client::Get().ProcessDrivers(aInstance); }

void otSysUpdateEvents(otInstance     *aInstance,
                       int            *aMaxFd,
                       fd_set         *aReadFdSet,
                       fd_set         *aWriteFdSet,
                       fd_set         *aErrorFdSet,
                       struct timeval *aTimeout)
{
    int fd = Client::Get().GetSocketFd();

    (void)aInstance;
    (void)aWriteFdSet;

    if (fd >= 0 && aReadFdSet != nullptr)
    {
        FD_SET(fd, aReadFdSet);
        if (aErrorFdSet != nullptr)
        {
            FD_SET(fd, aErrorFdSet);
        }
        if (aMaxFd != nullptr && fd > *aMaxFd)
        {
            *aMaxFd = fd;
        }
    }

    if (aTimeout != nullptr && aTimeout->tv_sec > 1)
    {
        aTimeout->tv_sec  = 1;
        aTimeout->tv_usec = 0;
    }
}

void otSysProcessEvents(otInstance   *aInstance,
                        const fd_set *aReadFdSet,
                        const fd_set *aWriteFdSet,
                        const fd_set *aErrorFdSet)
{
    int fd = Client::Get().GetSocketFd();

    (void)aWriteFdSet;

    if (fd >= 0 &&
        ((aReadFdSet != nullptr && FD_ISSET(fd, aReadFdSet)) || (aErrorFdSet != nullptr && FD_ISSET(fd, aErrorFdSet))))
    {
        Client::Get().ProcessReadySocket(aInstance);
    }
}

//---------------------------------------------------------------------------------------------------------------------
// OpenThread Platform Abstraction Layer (otPlat*)

void *otPlatCAlloc(size_t aNum, size_t aSize) { return std::calloc(aNum, aSize); }

void otPlatFree(void *aPtr) { std::free(aPtr); }

void otPlatReset(otInstance *aInstance)
{
    (void)aInstance;
    Client::Get().RequestPseudoReset();
}

otPlatResetReason otPlatGetResetReason(otInstance *aInstance)
{
    (void)aInstance;
    return OT_PLAT_RESET_REASON_POWER_ON;
}

void otPlatWakeHost(void) {}

void otTaskletsSignalPending(otInstance *aInstance)
{
    (void)aInstance;
    Client::Get().SignalPendingTasklets();
}

otError otPlatEntropyGet(uint8_t *aOutput, uint16_t aOutputLength)
{
    if (aOutput == nullptr)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    for (uint16_t i = 0; i < aOutputLength; ++i)
    {
        aOutput[i] = Client::Get().NextRandomByte();
    }

    return OT_ERROR_NONE;
}

void otPlatLogOutput(otInstance *aInstance, otLogLevel aLogLevel, const char *aLogLine)
{
    (void)aInstance;
    Client::Get().SendLog(aLogLevel, aLogLine);
}

void otPlatOtnsStatus(const char *aStatus) { (void)aStatus; }

uint64_t otPlatTimeGet(void) { return Client::Get().GetNowUs(); }

uint16_t otPlatTimeGetXtalAccuracy(void) { return 0; }

uint32_t otPlatAlarmMilliGetNow(void) { return static_cast<uint32_t>(Client::Get().GetNowUs() / 1000ULL); }

void otPlatAlarmMilliStartAt(otInstance *aInstance, uint32_t aT0, uint32_t aDt)
{
    (void)aInstance;
    Client::Get().StartAlarmMilliAt(aT0, aDt);
}

void otPlatAlarmMilliStop(otInstance *aInstance)
{
    (void)aInstance;
    Client::Get().StopAlarmMilli();
}

uint32_t otPlatAlarmMicroGetNow(void) { return static_cast<uint32_t>(Client::Get().GetNowUs()); }

void otPlatAlarmMicroStartAt(otInstance *aInstance, uint32_t aT0, uint32_t aDt)
{
    (void)aInstance;
    Client::Get().StartAlarmMicroAt(aT0, aDt);
}

void otPlatAlarmMicroStop(otInstance *aInstance)
{
    (void)aInstance;
    Client::Get().StopAlarmMicro();
}

void otPlatSettingsInit(otInstance *aInstance, const uint16_t *aSensitiveKeys, uint16_t aSensitiveKeysLength)
{
    (void)aInstance;
    (void)aSensitiveKeys;
    (void)aSensitiveKeysLength;
}

void otPlatSettingsDeinit(otInstance *aInstance) { (void)aInstance; }

otError otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue, uint16_t *aValueLength)
{
    (void)aInstance;
    return Client::Get().SettingsGet(aKey, aIndex, aValue, aValueLength);
}

otError otPlatSettingsSet(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    (void)aInstance;
    return Client::Get().SettingsSet(aKey, aValue, aValueLength);
}

otError otPlatSettingsAdd(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    (void)aInstance;
    return Client::Get().SettingsAdd(aKey, aValue, aValueLength);
}

otError otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex)
{
    (void)aInstance;
    return Client::Get().SettingsDelete(aKey, aIndex);
}

void otPlatSettingsWipe(otInstance *aInstance)
{
    (void)aInstance;
    Client::Get().SettingsWipe();
}

otRadioCaps otPlatRadioGetCaps(otInstance *aInstance)
{
    (void)aInstance;
    return OT_RADIO_CAPS_ACK_TIMEOUT | OT_RADIO_CAPS_TRANSMIT_SEC | OT_RADIO_CAPS_ENERGY_SCAN |
           OT_RADIO_CAPS_ALT_SHORT_ADDR;
}

int8_t otPlatRadioGetReceiveSensitivity(otInstance *aInstance)
{
    (void)aInstance;
    return Client::Get().GetRxSensitivity();
}

void otPlatRadioGetIeeeEui64(otInstance *aInstance, uint8_t *aIeeeEui64)
{
    uint32_t id = Client::Get().GetNodeId();

    (void)aInstance;
    std::memset(aIeeeEui64, 0, 8);
    aIeeeEui64[0] = 0x02; // Locally administered unicast OUI
    aIeeeEui64[4] = static_cast<uint8_t>((id >> 24u) & 0xFFu);
    aIeeeEui64[5] = static_cast<uint8_t>((id >> 16u) & 0xFFu);
    aIeeeEui64[6] = static_cast<uint8_t>((id >> 8u) & 0xFFu);
    aIeeeEui64[7] = static_cast<uint8_t>(id & 0xFFu);
}

void otPlatRadioSetPanId(otInstance *aInstance, otPanId aPanId)
{
    (void)aInstance;
    Client::Get().SetPanId(aPanId);
}

void otPlatRadioSetExtendedAddress(otInstance *aInstance, const otExtAddress *aExtAddress)
{
    (void)aInstance;
    Client::Get().SetExtendedAddress(aExtAddress);
}

void otPlatRadioSetShortAddress(otInstance *aInstance, otShortAddress aShortAddress)
{
    (void)aInstance;
    Client::Get().SetShortAddress(aShortAddress);
}

void otPlatRadioSetAlternateShortAddress(otInstance *aInstance, otShortAddress aShortAddress)
{
    (void)aInstance;
    Client::Get().SetAltShortAddress(aShortAddress);
}

bool otPlatRadioGetPromiscuous(otInstance *aInstance)
{
    (void)aInstance;
    return Client::Get().GetPromiscuous();
}

void otPlatRadioSetPromiscuous(otInstance *aInstance, bool aEnable)
{
    (void)aInstance;
    Client::Get().SetPromiscuous(aEnable);
}

otRadioState otPlatRadioGetState(otInstance *aInstance)
{
    (void)aInstance;
    return Client::Get().RadioGetState();
}

otError otPlatRadioEnable(otInstance *aInstance)
{
    (void)aInstance;
    return Client::Get().RadioEnable();
}

otError otPlatRadioDisable(otInstance *aInstance)
{
    (void)aInstance;
    return Client::Get().RadioDisable();
}

bool otPlatRadioIsEnabled(otInstance *aInstance)
{
    (void)aInstance;
    return Client::Get().RadioGetState() != OT_RADIO_STATE_DISABLED;
}

otError otPlatRadioSleep(otInstance *aInstance)
{
    (void)aInstance;
    return Client::Get().RadioSleep();
}

otError otPlatRadioReceive(otInstance *aInstance, uint8_t aChannel)
{
    (void)aInstance;
    return Client::Get().RadioReceive(aChannel);
}

otRadioFrame *otPlatRadioGetTransmitBuffer(otInstance *aInstance)
{
    (void)aInstance;
    return Client::Get().RadioGetTxBuffer();
}

otError otPlatRadioTransmit(otInstance *aInstance, otRadioFrame *aFrame)
{
    (void)aInstance;
    return Client::Get().RadioTransmit(aFrame);
}

int8_t otPlatRadioGetRssi(otInstance *aInstance)
{
    (void)aInstance;
    return Client::Get().GetLastRssi();
}

otError otPlatRadioEnergyScan(otInstance *aInstance, uint8_t aScanChannel, uint16_t aScanDuration)
{
    (void)aInstance;
    return Client::Get().RadioEnergyScan(aScanChannel, aScanDuration);
}

void otPlatRadioEnableSrcMatch(otInstance *aInstance, bool aEnable)
{
    (void)aInstance;
    Client::Get().EnableSrcMatch(aEnable);
}

otError otPlatRadioAddSrcMatchShortEntry(otInstance *aInstance, otShortAddress aShortAddress)
{
    (void)aInstance;
    return Client::Get().AddSrcMatchShort(aShortAddress);
}

otError otPlatRadioAddSrcMatchExtEntry(otInstance *aInstance, const otExtAddress *aExtAddress)
{
    (void)aInstance;
    return Client::Get().AddSrcMatchExt(aExtAddress);
}

otError otPlatRadioClearSrcMatchShortEntry(otInstance *aInstance, otShortAddress aShortAddress)
{
    (void)aInstance;
    return Client::Get().ClearSrcMatchShort(aShortAddress);
}

otError otPlatRadioClearSrcMatchExtEntry(otInstance *aInstance, const otExtAddress *aExtAddress)
{
    (void)aInstance;
    return Client::Get().ClearSrcMatchExt(aExtAddress);
}

void otPlatRadioClearSrcMatchShortEntries(otInstance *aInstance)
{
    (void)aInstance;
    Client::Get().ClearAllSrcMatchShort();
}

void otPlatRadioClearSrcMatchExtEntries(otInstance *aInstance)
{
    (void)aInstance;
    Client::Get().ClearAllSrcMatchExt();
}

void otPlatRadioSetMacKey(otInstance             *aInstance,
                          uint8_t                 aKeyIdMode,
                          uint8_t                 aKeyIndex,
                          const otMacKeyMaterial *aPrevKey,
                          const otMacKeyMaterial *aCurrKey,
                          const otMacKeyMaterial *aNextKey,
                          otRadioKeyType          aKeyType)
{
    (void)aInstance;
    Client::Get().SetMacKey(aKeyIdMode, aKeyIndex, aPrevKey, aCurrKey, aNextKey, aKeyType);
}

void otPlatRadioSetMacFrameCounter(otInstance *aInstance, uint32_t aMacFrameCounter)
{
    (void)aInstance;
    Client::Get().SetMacFrameCounter(aMacFrameCounter);
}

void otPlatRadioSetMacFrameCounterIfLarger(otInstance *aInstance, uint32_t aMacFrameCounter)
{
    (void)aInstance;
    Client::Get().SetMacFrameCounterIfLarger(aMacFrameCounter);
}

uint64_t otPlatRadioGetNow(otInstance *aInstance)
{
    (void)aInstance;
    return Client::Get().GetNowUs();
}

otError otPlatRadioGetTransmitPower(otInstance *aInstance, int8_t *aPower)
{
    (void)aInstance;
    if (aPower == nullptr)
    {
        return OT_ERROR_INVALID_ARGS;
    }
    *aPower = Client::Get().GetTxPower();
    return OT_ERROR_NONE;
}

otError otPlatRadioSetTransmitPower(otInstance *aInstance, int8_t aPower)
{
    (void)aInstance;
    Client::Get().SetTxPower(aPower);
    return OT_ERROR_NONE;
}

otError otPlatRadioGetCcaEnergyDetectThreshold(otInstance *aInstance, int8_t *aThreshold)
{
    (void)aInstance;
    if (aThreshold == nullptr)
    {
        return OT_ERROR_INVALID_ARGS;
    }
    *aThreshold = Client::Get().GetCcaThreshold();
    return OT_ERROR_NONE;
}

otError otPlatRadioSetCcaEnergyDetectThreshold(otInstance *aInstance, int8_t aThreshold)
{
    (void)aInstance;
    Client::Get().SetCcaThreshold(aThreshold);
    return OT_ERROR_NONE;
}

otError otPlatRadioEnableCsl(otInstance         *aInstance,
                             uint32_t            aCslPeriod,
                             otShortAddress      aShortAddr,
                             const otExtAddress *aExtAddr)
{
    (void)aInstance;
    Client::Get().SetCslParams(aCslPeriod, aShortAddr, aExtAddr);
    return OT_ERROR_NONE;
}

otError otPlatRadioResetCsl(otInstance *aInstance)
{
    (void)aInstance;
    Client::Get().SetCslParams(0, OT_RADIO_INVALID_SHORT_ADDR, nullptr);
    return OT_ERROR_NONE;
}

void otPlatRadioUpdateCslSampleTime(otInstance *aInstance, uint32_t aCslSampleTime)
{
    (void)aInstance;
    Client::Get().UpdateCslSampleTime(aCslSampleTime);
}

uint8_t otPlatRadioGetCslAccuracy(otInstance *aInstance)
{
    (void)aInstance;
    return 0;
}

uint8_t otPlatRadioGetCslUncertainty(otInstance *aInstance)
{
    (void)aInstance;
    return 0;
}

otError otPlatRadioConfigureEnhAckProbing(otInstance          *aInstance,
                                          otLinkMetrics        aLinkMetrics,
                                          const otShortAddress aShortAddress,
                                          const otExtAddress  *aExtAddress)
{
    (void)aInstance;
    (void)aLinkMetrics;
    (void)aShortAddress;
    (void)aExtAddress;
    return OT_ERROR_NOT_IMPLEMENTED;
}

uint32_t otPlatRadioGetSupportedChannelMask(otInstance *aInstance)
{
    (void)aInstance;
    return OT_RADIO_2P4GHZ_OQPSK_CHANNEL_MASK;
}

uint32_t otPlatRadioGetPreferredChannelMask(otInstance *aInstance)
{
    (void)aInstance;
    return OT_RADIO_2P4GHZ_OQPSK_CHANNEL_MASK;
}

} // extern "C"
