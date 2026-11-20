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
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include <openthread-core-config.h>
#include <openthread-select.h>
#include <openthread-system.h>
#include <openthread/config.h>
#if !OPENTHREAD_RADIO && (OPENTHREAD_FTD || OPENTHREAD_MTD)
#include <openthread/ip6.h>
#include <openthread/link.h>
#include <openthread/message.h>
#include <openthread/thread.h>
#endif
#include <openthread/tasklet.h>
#include <openthread/platform/alarm-micro.h>
#include <openthread/platform/alarm-milli.h>
#if OPENTHREAD_CONFIG_DIAG_ENABLE
#include <openthread/platform/diag.h>
#endif
#include <openthread/platform/entropy.h>
#include <openthread/platform/memory.h>
#include <openthread/platform/misc.h>
#include <openthread/platform/otns.h>
#include <openthread/platform/settings.h>
#include <openthread/platform/time.h>

#include "utils/uart.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace otns {
namespace RfSim {

namespace {

static constexpr size_t kMaxRxBufferBytes = 256u * 1024u;

std::array<uint8_t, 16> MakeBackboneIp6(uint32_t aNodeId)
{
    std::array<uint8_t, 16> addr{};

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

std::array<uint8_t, 16> MakeLinkLocalIp6(uint32_t aNodeId)
{
    std::array<uint8_t, 16> addr{};

    addr[0]  = 0xFE;
    addr[1]  = 0x80;
    addr[12] = static_cast<uint8_t>((aNodeId >> 24u) & 0xFFu);
    addr[13] = static_cast<uint8_t>((aNodeId >> 16u) & 0xFFu);
    addr[14] = static_cast<uint8_t>((aNodeId >> 8u) & 0xFFu);
    addr[15] = static_cast<uint8_t>(aNodeId & 0xFFu);

    return addr;
}

bool IsEndpointToken(const std::string &aToken)
{
    return aToken.rfind("unix://", 0) == 0 || aToken.rfind("tcp://", 0) == 0 || (!aToken.empty() && aToken[0] == '/') ||
           aToken.find(".sock") != std::string::npos;
}

bool ParseUnsignedToken(const std::string &aToken, uint64_t &aOut)
{
    if (aToken.empty())
    {
        return false;
    }

    char              *endPtr = nullptr;
    unsigned long long val    = std::strtoull(aToken.c_str(), &endPtr, 0);

    if (endPtr == aToken.c_str() || *endPtr != '\0')
    {
        return false;
    }

    aOut = static_cast<uint64_t>(val);

    return true;
}

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

#if !OPENTHREAD_RADIO && (OPENTHREAD_FTD || OPENTHREAD_MTD)
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
#endif

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

void Client::SetAppName(const std::string &aName)
{
    if (!aName.empty())
    {
        mAppName = aName.substr(0, 32);
    }
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
    mDisconnected     = false;
    mCompletedStartup = false;
    mHasPushedStatus  = false;
    mTrelEnabled      = false;
    mTrelPort         = 0;
    mTrelCounters     = {};
    mInfraIfEnabled   = false;
    mDhcp6PdListening = false;
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
    hello.AddText(Protocol::TlvType::kText, mAppName);

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
    if (mCompletedStartup)
    {
        return !mDisconnected;
    }

    mInstance         = aInstance;
    mCompletedStartup = true;
    DrainTasklets(aInstance);
    SyncAlarmToSim();
    SyncStatusToSim(aInstance, true);
    SendStepDone();

    return !mDisconnected;
}

void Client::EnsureStartupCompleted(otInstance *aInstance)
{
    if (!mCompletedStartup && !mDisconnected && aInstance != nullptr)
    {
        (void)CompleteStartup(aInstance);
    }
}

void Client::ResetForPseudoReset(void)
{
    mCompletedStartup = false;
    mAlarmMilliUs.reset();
    mAlarmMicroUs.reset();
    mLastSentAlarmUs.reset();
    mRadioState      = OT_RADIO_STATE_DISABLED;
    mPanId           = 0xFFFF;
    mPromiscuous     = false;
    mSrcMatchEnabled = false;
    mSrcMatchShort.clear();
    mSrcMatchExt.clear();
    std::memset(&mRadioCtx, 0, sizeof(mRadioCtx));
    mRadioCtx.mShortAddress          = OT_RADIO_INVALID_SHORT_ADDR;
    mRadioCtx.mAlternateShortAddress = OT_RADIO_INVALID_SHORT_ADDR;
    mUartTxBuf                       = nullptr;
    mUartTxLen                       = 0;
    mHasPushedStatus                 = false;
    TrelDisable();
    InfraIfDeinit(nullptr);
    SendRadioState();
    SyncAlarmToSim();
}

otError Client::UartEnable(void)
{
#if OPENTHREAD_RADIO
    if (mUartInFd < 0)
    {
        mUartInFd  = ::dup(STDIN_FILENO);
        mUartOutFd = ::dup(STDOUT_FILENO);
        if (mUartInFd < 0 || mUartOutFd < 0)
        {
            return OT_ERROR_FAILED;
        }

        // Redirect stdout to stderr so incidental logs never corrupt Spinel HDLC frames.
        (void)::dup2(STDERR_FILENO, STDOUT_FILENO);
        std::signal(SIGPIPE, SIG_IGN);

        auto configureRawTty = [](int aFd) {
            if (::isatty(aFd) != 0)
            {
                struct termios tios;

                if (::tcgetattr(aFd, &tios) == 0)
                {
                    ::cfmakeraw(&tios);
                    tios.c_oflag = 0;
                    tios.c_cflag |= HUPCL | CREAD | CLOCAL;
                    (void)::tcsetattr(aFd, TCSANOW, &tios);
                }
            }
        };

        configureRawTty(mUartInFd);
        configureRawTty(mUartOutFd);

        int inFlags = ::fcntl(mUartInFd, F_GETFL, 0);

        if (inFlags >= 0)
        {
            (void)::fcntl(mUartInFd, F_SETFL, inFlags | O_NONBLOCK);
        }

        int outFlags = ::fcntl(mUartOutFd, F_GETFL, 0);

        if (outFlags >= 0)
        {
            (void)::fcntl(mUartOutFd, F_SETFL, outFlags | O_NONBLOCK);
        }
    }
#endif

    mUartEnabled = true;
    mUartTxBuf   = nullptr;
    mUartTxLen   = 0;

    return OT_ERROR_NONE;
}

otError Client::UartDisable(void)
{
    mUartEnabled = false;
    mUartTxBuf   = nullptr;
    mUartTxLen   = 0;

    return OT_ERROR_NONE;
}

otError Client::UartSend(const uint8_t *aBuf, uint16_t aBufLength)
{
    if (!mUartEnabled || aBuf == nullptr)
    {
        return OT_ERROR_FAILED;
    }

#if !OPENTHREAD_RADIO
    // Library / FTD / Matter mode: route OpenThread cli_uart output directly over ot-rfsim v3 kCliData,
    // stripping any byte-for-byte echo of the command line just fed into otPlatUartReceived().
    const char *data = reinterpret_cast<const char *>(aBuf);
    size_t      len  = aBufLength;

    if (mUartEchoOffset < mUartEchoExpected.size())
    {
        size_t remaining = mUartEchoExpected.size() - mUartEchoOffset;
        size_t cmpLen    = std::min(len, remaining);

        if (std::memcmp(data, mUartEchoExpected.data() + mUartEchoOffset, cmpLen) == 0)
        {
            mUartEchoOffset += cmpLen;
            data += cmpLen;
            len -= cmpLen;
        }
        else
        {
            mUartEchoExpected.clear();
            mUartEchoOffset = 0;
        }
    }

    if (len > 0)
    {
        SendCliOutput(data, len);
    }
    otPlatUartSendDone();
    return OT_ERROR_NONE;
#else
    if (mUartOutFd < 0)
    {
        return OT_ERROR_FAILED;
    }
    if (mUartTxLen > 0)
    {
        return OT_ERROR_BUSY;
    }

    mUartTxBuf = aBuf;
    mUartTxLen = aBufLength;

    return OT_ERROR_NONE;
#endif
}

otError Client::UartFlush(void)
{
#if !OPENTHREAD_RADIO
    return mUartEnabled ? OT_ERROR_NONE : OT_ERROR_INVALID_STATE;
#else
    if (!mUartEnabled || mUartOutFd < 0 || mUartTxBuf == nullptr || mUartTxLen == 0)
    {
        return OT_ERROR_INVALID_STATE;
    }

    while (mUartTxLen > 0)
    {
        ssize_t n = ::write(mUartOutFd, mUartTxBuf, mUartTxLen);

        if (n > 0)
        {
            mUartTxBuf += n;
            mUartTxLen = static_cast<uint16_t>(mUartTxLen - static_cast<uint16_t>(n));
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        {
            struct pollfd pfd = {mUartOutFd, POLLOUT, 0};

            (void)::poll(&pfd, 1, 10);
            continue;
        }

        mDisconnected = true;
        return OT_ERROR_FAILED;
    }

    mUartTxBuf = nullptr;
    otPlatUartSendDone();

    return OT_ERROR_NONE;
#endif
}

void Client::FlushUartNonBlocking(void)
{
    if (!mUartEnabled || mUartOutFd < 0)
    {
        return;
    }

    // Loop briefly so chained Spinel frames queued via otPlatUartSendDone() drain promptly.
    for (int iter = 0; iter < 16 && mUartTxLen > 0 && mUartTxBuf != nullptr; ++iter)
    {
        ssize_t n = ::write(mUartOutFd, mUartTxBuf, mUartTxLen);

        if (n > 0)
        {
            mUartTxBuf += n;
            mUartTxLen = static_cast<uint16_t>(mUartTxLen - static_cast<uint16_t>(n));
            if (mUartTxLen == 0)
            {
                mUartTxBuf = nullptr;
                otPlatUartSendDone();
            }
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            break;
        }
        mDisconnected = true;
        break;
    }
}

void Client::ProcessDrivers(otInstance *aInstance)
{
    mInstance = aInstance;

    if (mDisconnected)
    {
        return;
    }

    EnsureStartupCompleted(aInstance);

    if (!mUartEnabled || mUartInFd < 0)
    {
        DrainFrames(aInstance);
        if (!mDisconnected && mRxBuffer.empty())
        {
            if (RecvMoreBytes(250))
            {
                DrainFrames(aInstance);
            }
        }
        return;
    }

    // RCP mode: multiplex simulator socket + Spinel UART PTY descriptors.
    DrainTasklets(aInstance);
    FlushUartNonBlocking();
    DrainFrames(aInstance);
    if (mDisconnected || mPseudoResetRequested)
    {
        return;
    }

    struct pollfd pfds[3];
    nfds_t        nfds    = 0;
    int           sockIdx = -1;
    int           inIdx   = -1;
    int           outIdx  = -1;

    if (mSockFd >= 0)
    {
        sockIdx      = static_cast<int>(nfds);
        pfds[nfds++] = {mSockFd, POLLIN, 0};
    }
    if (mUartInFd >= 0)
    {
        inIdx        = static_cast<int>(nfds);
        pfds[nfds++] = {mUartInFd, POLLIN, 0};
    }
    if (mUartOutFd >= 0 && mUartTxLen > 0)
    {
        outIdx       = static_cast<int>(nfds);
        pfds[nfds++] = {mUartOutFd, POLLOUT, 0};
    }

    int timeoutMs = (mHasPendingTasklets || (aInstance != nullptr && otTaskletsArePending(aInstance))) ? 0 : 10;
    int pr        = ::poll(pfds, nfds, timeoutMs);

    if (pr < 0)
    {
        if (errno != EINTR)
        {
            mDisconnected = true;
        }
        return;
    }

    if (inIdx >= 0)
    {
        short rev = pfds[inIdx].revents;

        if ((rev & (POLLERR | POLLNVAL)) != 0)
        {
            mDisconnected = true;
            return;
        }
        if ((rev & POLLIN) != 0)
        {
            uint8_t rxBuf[256];

            while (!mDisconnected && !mPseudoResetRequested)
            {
                ssize_t n = ::read(mUartInFd, rxBuf, sizeof(rxBuf));

                if (n > 0)
                {
                    otPlatUartReceived(rxBuf, static_cast<uint16_t>(n));
                    DrainTasklets(aInstance);
                    FlushUartNonBlocking();
                    continue;
                }
                if (n == 0)
                {
                    mDisconnected = true;
                    return;
                }
                if (errno == EINTR)
                {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    break;
                }
                mDisconnected = true;
                return;
            }
        }
        else if ((rev & POLLHUP) != 0)
        {
            mDisconnected = true;
            return;
        }
    }

    if (outIdx >= 0 && (pfds[outIdx].revents & POLLOUT) != 0)
    {
        FlushUartNonBlocking();
    }

    if (sockIdx >= 0 && !mDisconnected && !mPseudoResetRequested)
    {
        short rev = pfds[sockIdx].revents;

        if ((rev & (POLLERR | POLLNVAL)) != 0)
        {
            mDisconnected = true;
            return;
        }
        if ((rev & (POLLIN | POLLHUP)) != 0)
        {
            if (RecvMoreBytes(0))
            {
                DrainFrames(aInstance);
            }
        }
    }

    if (!mDisconnected && !mPseudoResetRequested)
    {
        DrainTasklets(aInstance);
        FlushUartNonBlocking();
        SyncAlarmToSim();
        SyncStatusToSim(aInstance);
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

bool Client::HasPendingWork(otInstance *aInstance) const
{
    if (mDisconnected)
    {
        return false;
    }

    return !mRxBuffer.empty() || mHasPendingTasklets || (aInstance != nullptr && otTaskletsArePending(aInstance)) ||
           (mUartEnabled && mUartOutFd >= 0 && mUartTxLen > 0);
}

void Client::ProcessPendingWork(otInstance *aInstance)
{
    mInstance = aInstance;

    if (mDisconnected)
    {
        return;
    }

    DrainFrames(aInstance);
    DrainTasklets(aInstance);
    FlushUartNonBlocking();
    SyncAlarmToSim();
    SyncStatusToSim(aInstance);
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

#if !OPENTHREAD_RADIO && (OPENTHREAD_FTD || OPENTHREAD_MTD)
    info.mRole        = ToWireRole(otThreadGetDeviceRole(aInstance));
    info.mRloc16      = otThreadGetRloc16(aInstance);
    info.mPartitionId = otThreadGetPartitionId(aInstance);

    if (const otExtAddress *ext = otLinkGetExtendedAddress(aInstance))
    {
        std::memcpy(info.mExtAddr.data(), ext->m8, 8);
    }
#else
    // RCP mode: derive observable address & role hint from configured MAC short/extended address.
    info.mRloc16      = mRadioCtx.mShortAddress;
    info.mPartitionId = 0;
    if (mRadioState == OT_RADIO_STATE_DISABLED)
    {
        info.mRole = 0; // disabled
    }
    else if (mRadioCtx.mShortAddress == OT_RADIO_INVALID_SHORT_ADDR)
    {
        info.mRole = 1; // detached
    }
    else if ((mRadioCtx.mShortAddress & 0x03FFu) == 0)
    {
        info.mRole = 3; // router
    }
    else
    {
        info.mRole = 2; // child
    }

    // mRadioCtx.mExtAddress is stored in big-endian byte order (reversed from little-endian in SetExtendedAddress).
    std::memcpy(info.mExtAddr.data(), mRadioCtx.mExtAddress.m8, 8);
#endif

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
    FlushUartNonBlocking();

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
#if OPENTHREAD_CONFIG_PLATFORM_USEC_TIMER_ENABLE
            if (aInstance != nullptr)
            {
                otPlatAlarmMicroFired(aInstance);
            }
#endif
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

        if (line.has_value())
        {
            if (mCliCallback != nullptr)
            {
                mCliCallback(line->c_str(), mCliContext);
            }
            else if (mUartEnabled)
            {
                std::string uartLine = *line;

                while (!uartLine.empty() && (uartLine.back() == '\n' || uartLine.back() == '\r'))
                {
                    uartLine.pop_back();
                }
                uartLine.append("\r\n");

                mUartEchoExpected = uartLine;
                mUartEchoOffset   = 0;
                otPlatUartReceived(reinterpret_cast<const uint8_t *>(uartLine.data()),
                                   static_cast<uint16_t>(uartLine.size()));
                mUartEchoExpected.clear();
                mUartEchoOffset = 0;
            }
        }
        DrainTasklets(aInstance);
        SyncAlarmToSim();
        SyncStatusToSim(aInstance);
        SendStepDone();
        return;
    }

    case Protocol::MsgType::kTrelPeer:
    {
        auto                 addr    = aFrame.GetNetAddr();
        const Protocol::Tlv *payload = aFrame.FindTlv(Protocol::TlvType::kPayload);
        bool                 removed = (aFrame.mFlags & Protocol::kFlagCancel) != 0;

#if !OPENTHREAD_RADIO && OPENTHREAD_CONFIG_RADIO_LINK_TREL_ENABLE && !OPENTHREAD_CONFIG_TREL_MANAGE_DNSSD_ENABLE
        if (aInstance != nullptr && mTrelEnabled && addr.has_value())
        {
            otPlatTrelPeerInfo info;

            std::memset(&info, 0, sizeof(info));
            info.mRemoved   = removed;
            info.mTxtData   = (payload != nullptr && !payload->mValue.empty()) ? payload->mValue.data() : nullptr;
            info.mTxtLength = (payload != nullptr) ? static_cast<uint16_t>(payload->mValue.size()) : 0;
            std::memcpy(info.mSockAddr.mAddress.mFields.m8, addr->mSrcIp6.data(), 16);
            info.mSockAddr.mPort = addr->mSrcPort;
            otPlatTrelHandleDiscoveredPeerInfo(aInstance, &info);
        }
#else
        (void)addr;
        (void)payload;
        (void)removed;
#endif
        DrainTasklets(aInstance);
        SyncAlarmToSim();
        SyncStatusToSim(aInstance);
        SendStepDone();
        return;
    }

    case Protocol::MsgType::kTrelPacket:
    {
        auto                 addr    = aFrame.GetNetAddr();
        const Protocol::Tlv *payload = aFrame.FindTlv(Protocol::TlvType::kPayload);

        if (addr.has_value() && payload != nullptr)
        {
            mTrelCounters.mRxPackets += 1;
            mTrelCounters.mRxBytes += payload->mValue.size();

            if (mTrelPacketCallback != nullptr)
            {
                mTrelPacketCallback(addr->mSrcIp6.data(), addr->mSrcPort, addr->mDstIp6.data(), addr->mDstPort,
                                    payload->mValue.data(), static_cast<uint16_t>(payload->mValue.size()),
                                    mTrelPacketContext);
            }

#if !OPENTHREAD_RADIO && OPENTHREAD_CONFIG_RADIO_LINK_TREL_ENABLE
            if (aInstance != nullptr && mTrelEnabled && !payload->mValue.empty())
            {
                std::vector<uint8_t> mutableBuf = payload->mValue;
                otSockAddr           sender;

                std::memset(&sender, 0, sizeof(sender));
                std::memcpy(sender.mAddress.mFields.m8, addr->mSrcIp6.data(), 16);
                sender.mPort = addr->mSrcPort;
                otPlatTrelHandleReceived(aInstance, mutableBuf.data(), static_cast<uint16_t>(mutableBuf.size()),
                                         &sender);
            }
#endif
        }
        DrainTasklets(aInstance);
        SyncAlarmToSim();
        SyncStatusToSim(aInstance);
        SendStepDone();
        return;
    }

    case Protocol::MsgType::kInfraIfPacket:
    {
        auto                 addr    = aFrame.GetNetAddr();
        const Protocol::Tlv *payload = aFrame.FindTlv(Protocol::TlvType::kPayload);

        if (addr.has_value() && payload != nullptr)
        {
            if (mInfraIfPacketCallback != nullptr)
            {
                mInfraIfPacketCallback(addr->mSrcIp6.data(), addr->mSrcPort, addr->mDstIp6.data(), addr->mDstPort,
                                       payload->mValue.data(), static_cast<uint16_t>(payload->mValue.size()),
                                       mInfraIfPacketContext);
            }

#if !OPENTHREAD_RADIO && OPENTHREAD_CONFIG_BORDER_ROUTING_ENABLE
            if (aInstance != nullptr && mInfraIfEnabled && !payload->mValue.empty())
            {
                if (addr->mDstPort == 0)
                {
                    otIp6Address srcAddr;

                    std::memcpy(srcAddr.mFields.m8, addr->mSrcIp6.data(), 16);
                    otPlatInfraIfRecvIcmp6Nd(aInstance, mInfraIfIndex, &srcAddr, payload->mValue.data(),
                                             static_cast<uint16_t>(payload->mValue.size()));
                }
#if OPENTHREAD_CONFIG_BORDER_ROUTING_DHCP6_PD_CLIENT_ENABLE
                else if (mDhcp6PdListening && addr->mDstPort == 546)
                {
                    otMessage *msg = otIp6NewMessage(aInstance, nullptr);

                    if (msg != nullptr)
                    {
                        if (otMessageAppend(msg, payload->mValue.data(),
                                            static_cast<uint16_t>(payload->mValue.size())) == OT_ERROR_NONE)
                        {
                            otPlatInfraIfDhcp6PdClientHandleReceived(aInstance, msg, mInfraIfIndex);
                        }
                        else
                        {
                            otMessageFree(msg);
                        }
                    }
                }
#endif
            }
#endif
        }
        DrainTasklets(aInstance);
        SyncAlarmToSim();
        SyncStatusToSim(aInstance);
        SendStepDone();
        return;
    }

    case Protocol::MsgType::kHostSocket:
    {
        auto                 addr    = aFrame.GetNetAddr();
        const Protocol::Tlv *payload = aFrame.FindTlv(Protocol::TlvType::kPayload);

        if (addr.has_value() && payload != nullptr && mHostUdpPacketCallback != nullptr)
        {
            mHostUdpPacketCallback(addr->mSrcIp6.data(), addr->mSrcPort, addr->mDstIp6.data(), addr->mDstPort,
                                   payload->mValue.data(), static_cast<uint16_t>(payload->mValue.size()),
                                   mHostUdpPacketContext);
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

bool Client::GetInfraIfLinkLocalAddress(uint8_t aAddr[16]) const
{
    if (aAddr == nullptr || mNodeId == 0)
    {
        return false;
    }

    auto ll = MakeLinkLocalIp6(mNodeId);

    std::memcpy(aAddr, ll.data(), 16);

    return true;
}

bool Client::GetBackboneAddress(uint8_t aAddr[16]) const
{
    if (aAddr == nullptr || mNodeId == 0)
    {
        return false;
    }

    auto bb = MakeBackboneIp6(mNodeId);

    std::memcpy(aAddr, bb.data(), 16);

    return true;
}

void Client::SetTrelPacketHandler(otRfSimPacketCallback aCallback, void *aContext)
{
    mTrelPacketCallback = aCallback;
    mTrelPacketContext  = aContext;
}

void Client::SetInfraIfPacketHandler(otRfSimPacketCallback aCallback, void *aContext)
{
    mInfraIfPacketCallback = aCallback;
    mInfraIfPacketContext  = aContext;
}

void Client::SetHostUdpPacketHandler(otRfSimPacketCallback aCallback, void *aContext)
{
    mHostUdpPacketCallback = aCallback;
    mHostUdpPacketContext  = aContext;
}

void Client::TrelEnable(uint16_t *aUdpPort)
{
    uint16_t port = static_cast<uint16_t>(49152u + (mNodeId % 16000u));

    mTrelEnabled = true;
    mTrelPort    = port;
    if (aUdpPort != nullptr)
    {
        *aUdpPort = port;
    }
}

void Client::TrelDisable(void)
{
    if (!mTrelEnabled)
    {
        return;
    }

    mTrelEnabled = false;
    mTrelPort    = 0;

    if (!mDisconnected)
    {
        Protocol::Frame msg;

        msg.mType        = Protocol::MsgType::kTrelPeer;
        msg.mFlags       = Protocol::kFlagCancel;
        msg.mTimestampUs = mNowUs;
        (void)SendFrame(msg);
    }
}

bool Client::TrelRegisterService(uint16_t aPort, const uint8_t *aTxtData, uint16_t aTxtLength)
{
    if (mDisconnected)
    {
        return false;
    }

    mTrelEnabled = true;
    mTrelPort    = (aPort != 0) ? aPort : static_cast<uint16_t>(49152u + (mNodeId % 16000u));

    Protocol::Frame        msg;
    Protocol::NetAddrValue addr;

    addr.mSrcPort = mTrelPort;
    addr.mSrcIp6  = MakeBackboneIp6(mNodeId);

    msg.mType        = Protocol::MsgType::kTrelPeer;
    msg.mTimestampUs = mNowUs;
    msg.AddNetAddr(addr);
    if (aTxtData != nullptr && aTxtLength > 0)
    {
        msg.AddBytes(Protocol::TlvType::kPayload, aTxtData, aTxtLength);
    }

    return SendFrame(msg);
}

bool Client::TrelSend(const uint8_t aDstIp6[16], uint16_t aDstPort, const uint8_t *aPayload, uint16_t aLength)
{
    if (!mTrelEnabled || mDisconnected || aDstIp6 == nullptr || (aPayload == nullptr && aLength > 0))
    {
        mTrelCounters.mTxFailure += 1;
        return false;
    }

    Protocol::Frame        msg;
    Protocol::NetAddrValue addr;

    addr.mSrcPort = mTrelPort;
    addr.mDstPort = aDstPort;
    addr.mSrcIp6  = MakeBackboneIp6(mNodeId);
    std::memcpy(addr.mDstIp6.data(), aDstIp6, 16);

    msg.mType        = Protocol::MsgType::kTrelPacket;
    msg.mTimestampUs = mNowUs;
    msg.AddNetAddr(addr);
    msg.AddBytes(Protocol::TlvType::kPayload, aPayload, aLength);

    if (!SendFrame(msg))
    {
        mTrelCounters.mTxFailure += 1;
        return false;
    }

    mTrelCounters.mTxPackets += 1;
    mTrelCounters.mTxBytes += aLength;

    return true;
}

void Client::TrelResetCounters(void) { std::memset(&mTrelCounters, 0, sizeof(mTrelCounters)); }

void Client::EnsureInfraIfJoined(uint32_t aInfraIfIndex)
{
    if (mInfraIfEnabled && mInfraIfIndex == aInfraIfIndex)
    {
        return;
    }

    mInfraIfIndex   = (aInfraIfIndex != 0) ? aInfraIfIndex : 1u;
    mInfraIfEnabled = true;

    if (!mDisconnected)
    {
        Protocol::Frame msg;

        msg.mType        = Protocol::MsgType::kInfraIfState;
        msg.mTimestampUs = mNowUs;
        (void)SendFrame(msg);
    }
}

bool Client::InfraIfInit(otInstance *aInstance, uint32_t aInfraIfIndex)
{
    if (mDisconnected)
    {
        return false;
    }

    EnsureInfraIfJoined(aInfraIfIndex);

#if !OPENTHREAD_RADIO && OPENTHREAD_CONFIG_BORDER_ROUTING_ENABLE
    if (aInstance != nullptr)
    {
        (void)otPlatInfraIfStateChanged(aInstance, mInfraIfIndex, true);
    }
#else
    (void)aInstance;
#endif

    return true;
}

void Client::InfraIfDeinit(otInstance *aInstance)
{
    if (!mInfraIfEnabled)
    {
        return;
    }

    uint32_t prevIndex = mInfraIfIndex;

    mInfraIfEnabled   = false;
    mDhcp6PdListening = false;

    if (!mDisconnected)
    {
        Protocol::Frame msg;

        msg.mType        = Protocol::MsgType::kInfraIfState;
        msg.mFlags       = Protocol::kFlagCancel;
        msg.mTimestampUs = mNowUs;
        (void)SendFrame(msg);
    }

#if !OPENTHREAD_RADIO && OPENTHREAD_CONFIG_BORDER_ROUTING_ENABLE
    if (aInstance != nullptr)
    {
        (void)otPlatInfraIfStateChanged(aInstance, prevIndex, false);
    }
#else
    (void)aInstance;
    (void)prevIndex;
#endif
}

bool Client::InfraIfHasAddress(uint32_t aInfraIfIndex, const otIp6Address *aAddress)
{
    if (aAddress == nullptr)
    {
        return false;
    }

    EnsureInfraIfJoined(aInfraIfIndex);

    if (aInfraIfIndex != mInfraIfIndex)
    {
        return false;
    }

    auto ll = MakeLinkLocalIp6(mNodeId);
    auto bb = MakeBackboneIp6(mNodeId);

    return (std::memcmp(aAddress->mFields.m8, ll.data(), 16) == 0) ||
           (std::memcmp(aAddress->mFields.m8, bb.data(), 16) == 0);
}

otError Client::InfraIfSendIcmp6Nd(uint32_t            aInfraIfIndex,
                                   const otIp6Address *aDestAddress,
                                   const uint8_t      *aBuffer,
                                   uint16_t            aBufferLength)
{
    if (mDisconnected || aDestAddress == nullptr || aBuffer == nullptr || aBufferLength == 0)
    {
        return OT_ERROR_FAILED;
    }

    EnsureInfraIfJoined(aInfraIfIndex);
    if (aInfraIfIndex != mInfraIfIndex)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    return InfraIfSendPacket(aDestAddress->mFields.m8, 0, aBuffer, aBufferLength) ? OT_ERROR_NONE : OT_ERROR_FAILED;
}

bool Client::InfraIfSendPacket(const uint8_t aDstIp6[16], uint16_t aDstPort, const uint8_t *aPayload, uint16_t aLength)
{
    if (mDisconnected || aDstIp6 == nullptr || (aPayload == nullptr && aLength > 0))
    {
        return false;
    }

    EnsureInfraIfJoined(mInfraIfIndex);

    Protocol::Frame        msg;
    Protocol::NetAddrValue addr;

    addr.mSrcPort = static_cast<uint16_t>(mInfraIfIndex & 0xFFFFu);
    addr.mDstPort = aDstPort;
    addr.mSrcIp6  = MakeLinkLocalIp6(mNodeId);
    std::memcpy(addr.mDstIp6.data(), aDstIp6, 16);

    msg.mType        = Protocol::MsgType::kInfraIfPacket;
    msg.mTimestampUs = mNowUs;
    msg.AddNetAddr(addr);
    msg.AddBytes(Protocol::TlvType::kPayload, aPayload, aLength);

    return SendFrame(msg);
}

otError Client::InfraIfGetLinkLayerAddress(uint32_t aInfraIfIndex, otPlatInfraIfLinkLayerAddress *aAddr)
{
    if (aAddr == nullptr)
    {
        return OT_ERROR_FAILED;
    }

    EnsureInfraIfJoined(aInfraIfIndex);
    if (aInfraIfIndex != mInfraIfIndex)
    {
        return OT_ERROR_FAILED;
    }

    std::memset(aAddr, 0, sizeof(*aAddr));
    aAddr->mLength     = 6;
    aAddr->mAddress[0] = 0x02; // Locally administered unicast MAC
    aAddr->mAddress[1] = 0x00;
    aAddr->mAddress[2] = static_cast<uint8_t>((mNodeId >> 24u) & 0xFFu);
    aAddr->mAddress[3] = static_cast<uint8_t>((mNodeId >> 16u) & 0xFFu);
    aAddr->mAddress[4] = static_cast<uint8_t>((mNodeId >> 8u) & 0xFFu);
    aAddr->mAddress[5] = static_cast<uint8_t>(mNodeId & 0xFFu);

    return OT_ERROR_NONE;
}

void Client::InfraIfDhcp6PdSetListening(bool aEnable, uint32_t aInfraIfIndex)
{
    if (aEnable)
    {
        EnsureInfraIfJoined(aInfraIfIndex);
    }
    mDhcp6PdListening = aEnable;
}

void Client::InfraIfDhcp6PdSend(otMessage *aMessage, const otIp6Address *aDestAddress, uint32_t aInfraIfIndex)
{
#if !OPENTHREAD_RADIO && (OPENTHREAD_FTD || OPENTHREAD_MTD)
    if (aMessage == nullptr)
    {
        return;
    }

    if (!mDisconnected && aDestAddress != nullptr)
    {
        uint16_t             len = otMessageGetLength(aMessage);
        std::vector<uint8_t> buf(len);

        if (len > 0)
        {
            (void)otMessageRead(aMessage, 0, buf.data(), len);
        }

        EnsureInfraIfJoined(aInfraIfIndex);

        Protocol::Frame        msg;
        Protocol::NetAddrValue addr;

        addr.mSrcPort = 546;
        addr.mDstPort = 547;
        addr.mSrcIp6  = MakeLinkLocalIp6(mNodeId);
        std::memcpy(addr.mDstIp6.data(), aDestAddress->mFields.m8, 16);

        msg.mType        = Protocol::MsgType::kInfraIfPacket;
        msg.mTimestampUs = mNowUs;
        msg.AddNetAddr(addr);
        msg.AddBytes(Protocol::TlvType::kPayload, buf.data(), buf.size());
        (void)SendFrame(msg);
    }

    otMessageFree(aMessage);
#else
    (void)aMessage;
    (void)aDestAddress;
    (void)aInfraIfIndex;
#endif
}

bool Client::HostUdpBind(uint16_t aPort)
{
    if (mDisconnected || aPort == 0)
    {
        return false;
    }

    Protocol::Frame        msg;
    Protocol::NetAddrValue addr;

    addr.mSrcPort = aPort;
    addr.mSrcIp6  = MakeBackboneIp6(mNodeId);

    msg.mType        = Protocol::MsgType::kHostSocket;
    msg.mTimestampUs = mNowUs;
    msg.AddNetAddr(addr);

    return SendFrame(msg);
}

bool Client::HostUdpUnbind(uint16_t aPort)
{
    if (mDisconnected || aPort == 0)
    {
        return false;
    }

    Protocol::Frame        msg;
    Protocol::NetAddrValue addr;

    addr.mSrcPort = aPort;
    addr.mSrcIp6  = MakeBackboneIp6(mNodeId);

    msg.mType        = Protocol::MsgType::kHostSocket;
    msg.mFlags       = Protocol::kFlagCancel;
    msg.mTimestampUs = mNowUs;
    msg.AddNetAddr(addr);

    return SendFrame(msg);
}

bool Client::HostUdpSend(uint16_t       aSrcPort,
                         const uint8_t  aDstIp6[16],
                         uint16_t       aDstPort,
                         const uint8_t *aPayload,
                         uint16_t       aLength)
{
    if (mDisconnected || aDstIp6 == nullptr || (aPayload == nullptr && aLength > 0))
    {
        return false;
    }

    Protocol::Frame        msg;
    Protocol::NetAddrValue addr;

    addr.mSrcPort = aSrcPort;
    addr.mDstPort = aDstPort;
    addr.mSrcIp6  = MakeBackboneIp6(mNodeId);
    std::memcpy(addr.mDstIp6.data(), aDstIp6, 16);

    msg.mType        = Protocol::MsgType::kHostSocket;
    msg.mTimestampUs = mNowUs;
    msg.AddNetAddr(addr);
    msg.AddBytes(Protocol::TlvType::kPayload, aPayload, aLength);

    return SendFrame(msg);
}

} // namespace RfSim
} // namespace otns

using otns::RfSim::Client;

extern "C" {

//---------------------------------------------------------------------------------------------------------------------
// Public ot-rfsim v3 Library API

void otRfSimSetAppName(const char *aName) { Client::Get().SetAppName((aName != nullptr) ? aName : ""); }

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

bool otRfSimInitInfraIf(otInstance *aInstance, uint32_t aInfraIfIndex)
{
    return Client::Get().InfraIfInit(aInstance, aInfraIfIndex);
}

void otRfSimDeinitInfraIf(otInstance *aInstance) { Client::Get().InfraIfDeinit(aInstance); }

bool otRfSimGetInfraIfLinkLocalAddress(uint8_t aAddr[16]) { return Client::Get().GetInfraIfLinkLocalAddress(aAddr); }

bool otRfSimGetBackboneAddress(uint8_t aAddr[16]) { return Client::Get().GetBackboneAddress(aAddr); }

void otRfSimSetTrelPacketHandler(otRfSimPacketCallback aCallback, void *aContext)
{
    Client::Get().SetTrelPacketHandler(aCallback, aContext);
}

void otRfSimSetInfraIfPacketHandler(otRfSimPacketCallback aCallback, void *aContext)
{
    Client::Get().SetInfraIfPacketHandler(aCallback, aContext);
}

void otRfSimSetHostUdpPacketHandler(otRfSimPacketCallback aCallback, void *aContext)
{
    Client::Get().SetHostUdpPacketHandler(aCallback, aContext);
}

bool otRfSimRegisterTrelService(uint16_t aUdpPort, const uint8_t *aTxtData, uint16_t aTxtLength)
{
    return Client::Get().TrelRegisterService(aUdpPort, aTxtData, aTxtLength);
}

void otRfSimUnregisterTrelService(void) { Client::Get().TrelDisable(); }

bool otRfSimSendTrelPacket(const uint8_t aDstIp6[16], uint16_t aDstPort, const uint8_t *aPayload, uint16_t aLength)
{
    return Client::Get().TrelSend(aDstIp6, aDstPort, aPayload, aLength);
}

bool otRfSimSendInfraIfPacket(const uint8_t aDstIp6[16], uint16_t aDstPort, const uint8_t *aPayload, uint16_t aLength)
{
    return Client::Get().InfraIfSendPacket(aDstIp6, aDstPort, aPayload, aLength);
}

bool otRfSimBindHostUdp(uint16_t aPort) { return Client::Get().HostUdpBind(aPort); }

bool otRfSimUnbindHostUdp(uint16_t aPort) { return Client::Get().HostUdpUnbind(aPort); }

bool otRfSimSendHostUdp(uint16_t       aSrcPort,
                        const uint8_t  aDstIp6[16],
                        uint16_t       aDstPort,
                        const uint8_t *aPayload,
                        uint16_t       aLength)
{
    return Client::Get().HostUdpSend(aSrcPort, aDstIp6, aDstPort, aPayload, aLength);
}

//---------------------------------------------------------------------------------------------------------------------
// OpenThread System & Select API (for Standalone CLI, OTBR, and Matter Library Mode)

void otSysInit(int aArgCount, char *aArgVector[])
{
    if (Client::Get().PseudoResetRequested())
    {
        Client::Get().ClearPseudoReset();
        Client::Get().ResetForPseudoReset();
        return;
    }

    std::string              endpoint;
    std::string              appName;
    uint32_t                 nodeId  = 0;
    uint64_t                 seed    = 1;
    bool                     hasSeed = false;
    std::vector<std::string> positional;

    auto extractEq = [](const std::string &aArg, const char *aPrefix, std::string &aVal) -> bool {
        size_t len = std::strlen(aPrefix);

        if (aArg.size() > len && aArg.compare(0, len, aPrefix) == 0)
        {
            aVal = aArg.substr(len);
            return true;
        }
        return false;
    };

    for (int i = 1; i < aArgCount; ++i)
    {
        if (aArgVector[i] == nullptr)
        {
            continue;
        }

        std::string arg = aArgVector[i];
        std::string eqVal;

        if ((arg == "--endpoint" || arg == "--socket") && i + 1 < aArgCount)
        {
            endpoint = aArgVector[++i];
        }
        else if (extractEq(arg, "--endpoint=", eqVal) || extractEq(arg, "--socket=", eqVal))
        {
            endpoint = eqVal;
        }
        else if (arg == "--node-id" && i + 1 < aArgCount)
        {
            nodeId = static_cast<uint32_t>(std::strtoul(aArgVector[++i], nullptr, 0));
        }
        else if (extractEq(arg, "--node-id=", eqVal))
        {
            nodeId = static_cast<uint32_t>(std::strtoul(eqVal.c_str(), nullptr, 0));
        }
        else if (arg == "--seed" && i + 1 < aArgCount)
        {
            seed    = static_cast<uint64_t>(std::strtoull(aArgVector[++i], nullptr, 0));
            hasSeed = true;
        }
        else if (extractEq(arg, "--seed=", eqVal))
        {
            seed    = static_cast<uint64_t>(std::strtoull(eqVal.c_str(), nullptr, 0));
            hasSeed = true;
        }
        else if ((arg == "--label" || arg == "--app-name") && i + 1 < aArgCount)
        {
            appName = aArgVector[++i];
        }
        else if (extractEq(arg, "--label=", eqVal) || extractEq(arg, "--app-name=", eqVal))
        {
            appName = eqVal;
        }
        else if ((arg == "--thread-arg" || arg == "--thread-args") && i + 1 < aArgCount)
        {
            positional.push_back(aArgVector[++i]);
        }
        else if (extractEq(arg, "--thread-arg=", eqVal) || extractEq(arg, "--thread-args=", eqVal))
        {
            positional.push_back(eqVal);
        }
        else if (!arg.empty() && arg[0] != '-')
        {
            positional.push_back(arg);
        }
    }

    // Parse Matter / OTNS v2 positional or --thread-args tokens: <nodeId> <socket> [seed]
    for (const auto &tok : positional)
    {
        if (endpoint.empty() && otns::RfSim::IsEndpointToken(tok))
        {
            endpoint = tok;
            continue;
        }

        uint64_t num = 0;

        if (otns::RfSim::ParseUnsignedToken(tok, num))
        {
            if (nodeId == 0)
            {
                nodeId = static_cast<uint32_t>(num);
            }
            else if (!hasSeed)
            {
                seed    = num;
                hasSeed = true;
            }
        }
    }

    if (endpoint.empty())
    {
        if (const char *envEp = std::getenv("OTNS_RFSIM_ENDPOINT"))
        {
            endpoint = envEp;
        }
    }
    if (nodeId == 0)
    {
        if (const char *envId = std::getenv("OTNS_NODE_ID"))
        {
            nodeId = static_cast<uint32_t>(std::strtoul(envId, nullptr, 0));
        }
    }

    if (!appName.empty())
    {
        Client::Get().SetAppName(appName);
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
    Client::Get().EnsureStartupCompleted(aInstance);

    int fd = Client::Get().IsConnected() ? Client::Get().GetSocketFd() : -1;

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

    if (aTimeout != nullptr)
    {
        if (Client::Get().HasPendingWork(aInstance))
        {
            aTimeout->tv_sec  = 0;
            aTimeout->tv_usec = 0;
        }
        else if (aTimeout->tv_sec > 1)
        {
            aTimeout->tv_sec  = 1;
            aTimeout->tv_usec = 0;
        }
    }
}

void otSysProcessEvents(otInstance   *aInstance,
                        const fd_set *aReadFdSet,
                        const fd_set *aWriteFdSet,
                        const fd_set *aErrorFdSet)
{
    int fd = Client::Get().IsConnected() ? Client::Get().GetSocketFd() : -1;

    (void)aWriteFdSet;

    if (fd >= 0 &&
        ((aReadFdSet != nullptr && FD_ISSET(fd, aReadFdSet)) || (aErrorFdSet != nullptr && FD_ISSET(fd, aErrorFdSet))))
    {
        Client::Get().ProcessReadySocket(aInstance);
    }

    Client::Get().ProcessPendingWork(aInstance);
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

otError otPlatSetMcuPowerState(otInstance *aInstance, otPlatMcuPowerState aState)
{
    (void)aInstance;
    return (aState == OT_PLAT_MCU_POWER_STATE_ON || aState == OT_PLAT_MCU_POWER_STATE_LOW_POWER) ? OT_ERROR_NONE
                                                                                                 : OT_ERROR_FAILED;
}

otPlatMcuPowerState otPlatGetMcuPowerState(otInstance *aInstance)
{
    (void)aInstance;
    return OT_PLAT_MCU_POWER_STATE_ON;
}

#if OPENTHREAD_CONFIG_PLATFORM_ASSERT_MANAGEMENT
void otPlatAssertFail(const char *aFilename, int aLineNumber)
{
    std::fprintf(stderr, "ot-rfsim v3 assert failed at %s:%d\n", (aFilename != nullptr) ? aFilename : "?", aLineNumber);
    std::abort();
}
#endif

#if OPENTHREAD_CONFIG_DIAG_ENABLE
static bool sDiagMode = false;

void otPlatDiagSetOutputCallback(otInstance *aInstance, otPlatDiagOutputCallback aCallback, void *aContext)
{
    (void)aInstance;
    (void)aCallback;
    (void)aContext;
}

void otPlatDiagModeSet(bool aMode) { sDiagMode = aMode; }

bool otPlatDiagModeGet(void) { return sDiagMode; }

void otPlatDiagChannelSet(uint8_t aChannel) { (void)aChannel; }

void otPlatDiagTxPowerSet(int8_t aTxPower) { (void)aTxPower; }

void otPlatDiagRadioReceived(otInstance *aInstance, otRadioFrame *aFrame, otError aError)
{
    (void)aInstance;
    (void)aFrame;
    (void)aError;
}

void otPlatDiagAlarmCallback(otInstance *aInstance) { (void)aInstance; }
#endif

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

void otPlatLog(otLogLevel aLogLevel, otLogRegion aLogRegion, const char *aFormat, ...)
{
    char    buf[512];
    va_list args;

    (void)aLogRegion;
    va_start(args, aFormat);
    std::vsnprintf(buf, sizeof(buf), aFormat, args);
    va_end(args);

    Client::Get().SendLog(aLogLevel, buf);
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

OT_TOOL_WEAK void otPlatSettingsInit(otInstance     *aInstance,
                                     const uint16_t *aSensitiveKeys,
                                     uint16_t        aSensitiveKeysLength)
{
    (void)aInstance;
    (void)aSensitiveKeys;
    (void)aSensitiveKeysLength;
}

OT_TOOL_WEAK void otPlatSettingsDeinit(otInstance *aInstance) { (void)aInstance; }

OT_TOOL_WEAK otError
otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue, uint16_t *aValueLength)
{
    (void)aInstance;
    return Client::Get().SettingsGet(aKey, aIndex, aValue, aValueLength);
}

OT_TOOL_WEAK otError otPlatSettingsSet(otInstance    *aInstance,
                                       uint16_t       aKey,
                                       const uint8_t *aValue,
                                       uint16_t       aValueLength)
{
    (void)aInstance;
    return Client::Get().SettingsSet(aKey, aValue, aValueLength);
}

OT_TOOL_WEAK otError otPlatSettingsAdd(otInstance    *aInstance,
                                       uint16_t       aKey,
                                       const uint8_t *aValue,
                                       uint16_t       aValueLength)
{
    (void)aInstance;
    return Client::Get().SettingsAdd(aKey, aValue, aValueLength);
}

OT_TOOL_WEAK otError otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex)
{
    (void)aInstance;
    return Client::Get().SettingsDelete(aKey, aIndex);
}

OT_TOOL_WEAK void otPlatSettingsWipe(otInstance *aInstance)
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

uint32_t otPlatRadioGetBusSpeed(otInstance *aInstance)
{
    (void)aInstance;
    return 1000000u;
}

void otPlatRadioSetRxOnWhenIdle(otInstance *aInstance, bool aEnable)
{
    (void)aInstance;
    (void)aEnable;
}

//---------------------------------------------------------------------------------------------------------------------
// Spinel UART Platform Hooks (used by ot-rcp-rfsim)

OT_TOOL_WEAK void otPlatUartReceived(const uint8_t *aBuf, uint16_t aBufLength)
{
    (void)aBuf;
    (void)aBufLength;
}

OT_TOOL_WEAK void otPlatUartSendDone(void) {}

otError otPlatUartEnable(void) { return Client::Get().UartEnable(); }

otError otPlatUartDisable(void) { return Client::Get().UartDisable(); }

otError otPlatUartSend(const uint8_t *aBuf, uint16_t aBufLength) { return Client::Get().UartSend(aBuf, aBufLength); }

otError otPlatUartFlush(void) { return Client::Get().UartFlush(); }

//---------------------------------------------------------------------------------------------------------------------
// TREL Platform Hooks (OPENTHREAD_CONFIG_RADIO_LINK_TREL_ENABLE)

#if !OPENTHREAD_RADIO && OPENTHREAD_CONFIG_RADIO_LINK_TREL_ENABLE

void otPlatTrelEnable(otInstance *aInstance, uint16_t *aUdpPort)
{
    (void)aInstance;
    Client::Get().TrelEnable(aUdpPort);
}

void otPlatTrelDisable(otInstance *aInstance)
{
    (void)aInstance;
    Client::Get().TrelDisable();
}

void otPlatTrelNotifyPeerSocketAddressDifference(otInstance       *aInstance,
                                                 const otSockAddr *aPeerSockAddr,
                                                 const otSockAddr *aRxSockAddr)
{
    (void)aInstance;
    (void)aPeerSockAddr;
    (void)aRxSockAddr;
}

void otPlatTrelRegisterService(otInstance *aInstance, uint16_t aPort, const uint8_t *aTxtData, uint8_t aTxtLength)
{
    (void)aInstance;
    (void)Client::Get().TrelRegisterService(aPort, aTxtData, aTxtLength);
}

void otPlatTrelSend(otInstance       *aInstance,
                    const uint8_t    *aUdpPayload,
                    uint16_t          aUdpPayloadLen,
                    const otSockAddr *aDestSockAddr)
{
    (void)aInstance;
    if (aDestSockAddr == nullptr)
    {
        return;
    }
    (void)Client::Get().TrelSend(aDestSockAddr->mAddress.mFields.m8, aDestSockAddr->mPort, aUdpPayload, aUdpPayloadLen);
}

const otPlatTrelCounters *otPlatTrelGetCounters(otInstance *aInstance)
{
    (void)aInstance;
    return Client::Get().TrelGetCounters();
}

void otPlatTrelResetCounters(otInstance *aInstance)
{
    (void)aInstance;
    Client::Get().TrelResetCounters();
}

#endif // !OPENTHREAD_RADIO && OPENTHREAD_CONFIG_RADIO_LINK_TREL_ENABLE

//---------------------------------------------------------------------------------------------------------------------
// Adjacent Infrastructure Link (Infra-If) Platform Hooks (OPENTHREAD_CONFIG_BORDER_ROUTING_ENABLE)

#if !OPENTHREAD_RADIO && OPENTHREAD_CONFIG_BORDER_ROUTING_ENABLE

bool otPlatInfraIfHasAddress(otInstance *aInstance, uint32_t aInfraIfIndex, const otIp6Address *aAddress)
{
    (void)aInstance;
    return Client::Get().InfraIfHasAddress(aInfraIfIndex, aAddress);
}

otError otPlatInfraIfSendIcmp6Nd(otInstance         *aInstance,
                                 uint32_t            aInfraIfIndex,
                                 const otIp6Address *aDestAddress,
                                 const uint8_t      *aBuffer,
                                 uint16_t            aBufferLength)
{
    (void)aInstance;
    return Client::Get().InfraIfSendIcmp6Nd(aInfraIfIndex, aDestAddress, aBuffer, aBufferLength);
}

otError otPlatInfraIfDiscoverNat64Prefix(otInstance *aInstance, uint32_t aInfraIfIndex)
{
    (void)aInstance;
    (void)aInfraIfIndex;
    return OT_ERROR_NOT_IMPLEMENTED;
}

otError otPlatGetInfraIfLinkLayerAddress(otInstance                    *aInstance,
                                         uint32_t                       aIfIndex,
                                         otPlatInfraIfLinkLayerAddress *aInfraIfLinkLayerAddress)
{
    (void)aInstance;
    return Client::Get().InfraIfGetLinkLayerAddress(aIfIndex, aInfraIfLinkLayerAddress);
}

#if OPENTHREAD_CONFIG_BORDER_ROUTING_DHCP6_PD_CLIENT_ENABLE

void otPlatInfraIfDhcp6PdClientSetListeningEnabled(otInstance *aInstance, bool aEnable, uint32_t aInfraIfIndex)
{
    (void)aInstance;
    Client::Get().InfraIfDhcp6PdSetListening(aEnable, aInfraIfIndex);
}

void otPlatInfraIfDhcp6PdClientSend(otInstance   *aInstance,
                                    otMessage    *aMessage,
                                    otIp6Address *aDestAddress,
                                    uint32_t      aInfraIfIndex)
{
    (void)aInstance;
    Client::Get().InfraIfDhcp6PdSend(aMessage, aDestAddress, aInfraIfIndex);
}

#endif // OPENTHREAD_CONFIG_BORDER_ROUTING_DHCP6_PD_CLIENT_ENABLE

#endif // !OPENTHREAD_RADIO && OPENTHREAD_CONFIG_BORDER_ROUTING_ENABLE

} // extern "C"
