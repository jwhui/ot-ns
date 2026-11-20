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

#include "nodes/rfsim_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>

#include "core/path_security.hpp"
#include "core/simulation.hpp"
#include "nodes/rfsim_node.hpp"
#include "protocol/rfsim_v3_codec.hpp"

namespace otns {
namespace Nodes {

namespace {

static constexpr size_t kMaxHelloBytes = 8192;

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

bool ReadOneFrame(int aFd, Protocol::Frame &aOut, int aTimeoutMs)
{
    std::vector<uint8_t> buf;
    auto                 deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(aTimeoutMs);

    while (true)
    {
        if (!buf.empty())
        {
            Protocol::DecodeResult res = Protocol::DecodeFrame(buf.data(), buf.size());

            if (res.mStatus == Protocol::DecodeStatus::kOk)
            {
                aOut = std::move(res.mFrame);
                return true;
            }
            if (res.mStatus != Protocol::DecodeStatus::kNeedMoreData)
            {
                return false;
            }
        }

        auto now = std::chrono::steady_clock::now();

        if (now >= deadline)
        {
            return false;
        }

        int remMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        struct pollfd pfd;

        pfd.fd      = aFd;
        pfd.events  = POLLIN;
        pfd.revents = 0;

        int pr = ::poll(&pfd, 1, (remMs > 0) ? remMs : 1);

        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (pr == 0)
        {
            continue;
        }

        uint8_t tmp[1024];
        ssize_t n = ::recv(aFd, tmp, sizeof(tmp), 0);

        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
            {
                continue;
            }
            return false;
        }

        if (buf.size() + static_cast<size_t>(n) > kMaxHelloBytes)
        {
            return false;
        }
        buf.insert(buf.end(), tmp, tmp + n);
    }
}

void KillAndReap(pid_t aPid)
{
    if (aPid <= 0)
    {
        return;
    }

    int status = 0;

    (void)::kill(aPid, SIGTERM);
    for (int i = 0; i < 20; ++i)
    {
        if (::waitpid(aPid, &status, WNOHANG) != 0)
        {
            return;
        }
        ::usleep(10000);
    }
    (void)::kill(aPid, SIGKILL);
    (void)::waitpid(aPid, &status, 0);
}

void SetCloseOnExec(int aFd)
{
    if (aFd >= 0)
    {
        int flags = ::fcntl(aFd, F_GETFD, 0);

        if (flags >= 0)
        {
            (void)::fcntl(aFd, F_SETFD, flags | FD_CLOEXEC);
        }
    }
}

void ReplaceAll(std::string &aStr, const std::string &aFrom, const std::string &aTo)
{
    if (aFrom.empty())
    {
        return;
    }

    size_t pos = 0;

    while ((pos = aStr.find(aFrom, pos)) != std::string::npos)
    {
        aStr.replace(pos, aFrom.size(), aTo);
        pos += aTo.size();
    }
}

} // namespace

RfSimServer::RfSimServer(Core::Simulation &aSim)
    : mSim(aSim)
{
}

RfSimServer::~RfSimServer(void) { Close(); }

bool RfSimServer::ListenUnix(const std::string &aSocketPath)
{
    Close();

    struct sockaddr_un addr;

    if (aSocketPath.empty() || aSocketPath.size() >= sizeof(addr.sun_path))
    {
        return false;
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
    {
        return false;
    }

    (void)::unlink(aSocketPath.c_str());
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, aSocketPath.c_str(), aSocketPath.size() + 1);

    if (::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        (void)::unlink(aSocketPath.c_str());
        return false;
    }

    (void)::chmod(aSocketPath.c_str(), 0600);

    if (::listen(fd, 64) < 0)
    {
        ::close(fd);
        (void)::unlink(aSocketPath.c_str());
        return false;
    }

    SetCloseOnExec(fd);
    mListenFd = fd;
    mUnixPath = aSocketPath;
    mTcpPort  = 0;
    mEndpoint = "unix://" + aSocketPath;

    return true;
}

bool RfSimServer::ListenTcp(uint16_t aPort, const std::string &aBindAddr)
{
    Close();

    struct sockaddr_in addr;

    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(aPort);

    if (::inet_pton(AF_INET, aBindAddr.c_str(), &addr.sin_addr) != 1)
    {
        return false;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        return false;
    }

    int one = 1;

    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(fd, 64) < 0)
    {
        ::close(fd);
        return false;
    }

    socklen_t len = sizeof(addr);

    if (::getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len) < 0)
    {
        ::close(fd);
        return false;
    }

    SetCloseOnExec(fd);
    mListenFd = fd;
    mTcpPort  = ntohs(addr.sin_port);
    mUnixPath.clear();
    mEndpoint = "tcp://" + aBindAddr + ":" + std::to_string(mTcpPort);

    return true;
}

void RfSimServer::Close(void)
{
    if (mListenFd >= 0)
    {
        ::close(mListenFd);
        mListenFd = -1;
    }
    if (!mUnixPath.empty())
    {
        (void)::unlink(mUnixPath.c_str());
        mUnixPath.clear();
    }
    mTcpPort = 0;
    mEndpoint.clear();
}

Rf::NodeId RfSimServer::AcceptExternalNode(const Core::NodeConfig &aConfig, int aTimeoutMs)
{
    if (mListenFd < 0)
    {
        return Rf::kInvalidNodeId;
    }

    struct pollfd pfd;

    pfd.fd      = mListenFd;
    pfd.events  = POLLIN;
    pfd.revents = 0;

    int pr = ::poll(&pfd, 1, aTimeoutMs);

    if (pr <= 0)
    {
        return Rf::kInvalidNodeId;
    }

    int clientFd = ::accept(mListenFd, nullptr, nullptr);

    if (clientFd < 0)
    {
        return Rf::kInvalidNodeId;
    }

    SetCloseOnExec(clientFd);
    if (mTcpPort > 0)
    {
        int one = 1;

        (void)::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    int handshakeTimeoutMs = (aTimeoutMs > 0) ? aTimeoutMs : 2000;

    return HandshakeAndRegister(clientFd, aConfig, -1, handshakeTimeoutMs);
}

Rf::NodeId RfSimServer::SpawnExternalNode(const std::string              &aExecutable,
                                          const Core::NodeConfig         &aConfig,
                                          const std::vector<std::string> &aExtraArgs,
                                          int                             aTimeoutMs,
                                          SpawnArgStyle                   aArgStyle)
{
    if (mListenFd < 0 || aExecutable.empty())
    {
        return Rf::kInvalidNodeId;
    }

    Rf::NodeId               assignedId = mSim.AllocateNodeId();
    uint64_t                 spawnSeed  = mSim.GetPlatformPrng().NextU64();
    std::string              sockOrEp   = mUnixPath.empty() ? mEndpoint : mUnixPath;
    std::string              cleanLabel = Core::SanitizeNodeLabel(aConfig.mLabel);
    std::vector<std::string> args;

    args.push_back(aExecutable);

    if (aArgStyle == SpawnArgStyle::kOtns)
    {
        args.push_back("--endpoint");
        args.push_back(mEndpoint);
        args.push_back("--node-id");
        args.push_back(std::to_string(assignedId));
        if (!cleanLabel.empty())
        {
            args.push_back("--label");
            args.push_back(cleanLabel);
        }
        for (const auto &extra : aExtraArgs)
        {
            args.push_back(extra);
        }
    }
    else if (aArgStyle == SpawnArgStyle::kMatter)
    {
        args.push_back("--thread-args=" + std::to_string(assignedId));
        args.push_back("--thread-args=" + sockOrEp);
        args.push_back("--thread-args=" + std::to_string(spawnSeed));
        for (const auto &extra : aExtraArgs)
        {
            args.push_back(extra);
        }
    }
    else
    {
        std::string idStr   = std::to_string(assignedId);
        std::string seedStr = std::to_string(spawnSeed);

        for (const auto &extra : aExtraArgs)
        {
            std::string expanded = extra;

            ReplaceAll(expanded, "{endpoint}", mEndpoint);
            ReplaceAll(expanded, "{socket}", sockOrEp);
            ReplaceAll(expanded, "{node_id}", idStr);
            ReplaceAll(expanded, "{seed}", seedStr);
            args.push_back(std::move(expanded));
        }
    }

    std::vector<char *> argv;

    argv.reserve(args.size() + 1);
    for (auto &s : args)
    {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    pid_t pid = ::fork();

    if (pid < 0)
    {
        return Rf::kInvalidNodeId;
    }

    if (pid == 0)
    {
        // Child process: detach stdio to /dev/null and close inherited descriptors before execv.
        int devNull = ::open("/dev/null", O_RDWR);

        if (devNull >= 0)
        {
            (void)::dup2(devNull, STDIN_FILENO);
            (void)::dup2(devNull, STDOUT_FILENO);
            (void)::dup2(devNull, STDERR_FILENO);
            if (devNull > STDERR_FILENO)
            {
                ::close(devNull);
            }
        }

        long maxFd = ::sysconf(_SC_OPEN_MAX);

        if (maxFd <= 0 || maxFd > 4096)
        {
            maxFd = 4096;
        }
        for (int fd = 3; fd < static_cast<int>(maxFd); ++fd)
        {
            ::close(fd);
        }

        ::execv(aExecutable.c_str(), argv.data());
        ::_exit(127);
    }

    struct pollfd pfd;

    pfd.fd      = mListenFd;
    pfd.events  = POLLIN;
    pfd.revents = 0;

    int pr = ::poll(&pfd, 1, aTimeoutMs);

    if (pr <= 0)
    {
        KillAndReap(pid);
        return Rf::kInvalidNodeId;
    }

    int clientFd = ::accept(mListenFd, nullptr, nullptr);

    if (clientFd < 0)
    {
        KillAndReap(pid);
        return Rf::kInvalidNodeId;
    }

    SetCloseOnExec(clientFd);
    if (mTcpPort > 0)
    {
        int one = 1;

        (void)::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    Rf::NodeId registeredId = HandshakeAndRegister(clientFd, aConfig, pid, aTimeoutMs);

    if (registeredId == Rf::kInvalidNodeId)
    {
        KillAndReap(pid);
        return Rf::kInvalidNodeId;
    }

    // Configure initial dataset/mode on auto-start external CLI nodes via CLI if requested.
    if (aConfig.mAutoStart)
    {
        const auto &cfg = mSim.GetConfig();
        char        panBuf[16];

        std::snprintf(panBuf, sizeof(panBuf), "0x%04x", static_cast<unsigned>(cfg.mPanId));

        (void)mSim.ExecuteCli(registeredId, "dataset clear");
        (void)mSim.ExecuteCli(registeredId, "dataset channel " + std::to_string(cfg.mChannel));
        (void)mSim.ExecuteCli(registeredId, std::string("dataset panid ") + panBuf);
        (void)mSim.ExecuteCli(registeredId, "dataset extpanid 1111111122222222");
        (void)mSim.ExecuteCli(registeredId, "dataset networkname OTNSv3");
        (void)mSim.ExecuteCli(registeredId, "dataset networkkey " + cfg.mNetworkKey);
        (void)mSim.ExecuteCli(registeredId, "dataset activetimestamp 1");
        (void)mSim.ExecuteCli(registeredId, "dataset commit active");

        switch (aConfig.mType)
        {
        case Core::NodeType::kRouter:
            (void)mSim.ExecuteCli(registeredId, "mode rdn");
            break;
        case Core::NodeType::kFed:
            (void)mSim.ExecuteCli(registeredId, "mode rdn");
            (void)mSim.ExecuteCli(registeredId, "routereligible disable");
            break;
        case Core::NodeType::kMed:
            (void)mSim.ExecuteCli(registeredId, "mode rn");
            break;
        case Core::NodeType::kSed:
            (void)mSim.ExecuteCli(registeredId, "mode -");
            break;
        }

        (void)mSim.ExecuteCli(registeredId, "ifconfig up");
        (void)mSim.ExecuteCli(registeredId, "thread start");
    }

    return registeredId;
}

Rf::NodeId RfSimServer::HandshakeAndRegister(int                     aClientFd,
                                             const Core::NodeConfig &aConfig,
                                             pid_t                   aChildPid,
                                             int                     aTimeoutMs)
{
    Protocol::Frame hello;

    if (!ReadOneFrame(aClientFd, hello, aTimeoutMs) || hello.mType != Protocol::MsgType::kHello)
    {
        ::close(aClientFd);
        return Rf::kInvalidNodeId;
    }

    const Protocol::Tlv *magic = hello.FindTlv(Protocol::TlvType::kMagic);
    auto                 ver   = hello.GetVersion();

    if (magic == nullptr || magic->mValue.size() != Protocol::kProtocolMagic.size() ||
        std::memcmp(magic->mValue.data(), Protocol::kProtocolMagic.data(), Protocol::kProtocolMagic.size()) != 0 ||
        !ver.has_value() || ver->mMajor != Protocol::kProtocolMajor)
    {
        ::close(aClientFd);
        return Rf::kInvalidNodeId;
    }

    Rf::NodeId           preferredId = hello.GetU32(Protocol::TlvType::kNodeId).value_or(Rf::kInvalidNodeId);
    Rf::NodeId           nodeId      = mSim.AllocateNodeId(preferredId);
    uint64_t             nodeSeed    = mSim.GetPlatformPrng().NextU64();
    Protocol::Frame      ack;
    std::vector<uint8_t> wire;

    ack.mType        = Protocol::MsgType::kHelloAck;
    ack.mTimestampUs = mSim.GetNowUs();
    ack.AddVersion(Protocol::kProtocolMajor, Protocol::kProtocolMinor);
    ack.AddU32(Protocol::TlvType::kNodeId, nodeId);
    ack.AddU64(Protocol::TlvType::kSeed, nodeSeed);

    if (!Protocol::EncodeFrame(ack, wire) || !SendAllFd(aClientFd, wire.data(), wire.size()))
    {
        ::close(aClientFd);
        return Rf::kInvalidNodeId;
    }

    Core::NodeConfig effectiveCfg = aConfig;

    if (effectiveCfg.mLabel.empty())
    {
        auto peerText = hello.GetText(Protocol::TlvType::kText);

        if (peerText.has_value() && !peerText->empty() && *peerText != "ot-rfsim-v3")
        {
            effectiveCfg.mLabel = Core::SanitizeNodeLabel(*peerText);
        }
    }
    else
    {
        effectiveCfg.mLabel = Core::SanitizeNodeLabel(effectiveCfg.mLabel);
    }

    auto       node    = std::make_unique<RfSimNode>(mSim, nodeId, effectiveCfg, aClientFd, aChildPid);
    RfSimNode *rawNode = node.get();

    if (mSim.RegisterExternalNode(std::move(node), effectiveCfg) == Rf::kInvalidNodeId)
    {
        return Rf::kInvalidNodeId;
    }

    if (!rawNode->CompleteInitialSync())
    {
        mSim.RemoveNode(nodeId);
        return Rf::kInvalidNodeId;
    }

    return nodeId;
}

} // namespace Nodes
} // namespace otns
