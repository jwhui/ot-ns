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

#ifndef OTNS_SERVER_WS_SERVER_HPP_
#define OTNS_SERVER_WS_SERVER_HPP_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace otns {
namespace Server {

using ClientId = uint64_t;

static constexpr ClientId kInvalidClientId = 0;

/**
 * Single-threaded POSIX poll()-based HTTP/1.1 + RFC 6455 WebSocket server
 * with optional Unix domain socket newline-delimited JSON-RPC listener.
 *
 * Security invariants:
 * - Binds to 127.0.0.1 by default.
 * - Enforces loopback Origin checks on WebSocket upgrades when loopback-bound.
 * - Serves static assets with strict CSP / X-Frame-Options / nosniff headers
 *   and canonicalized path containment when `--web-dir` is enabled.
 */
class WsServer
{
public:
    static constexpr size_t kMaxHttpHeaderBytes = 16u * 1024u;
    static constexpr size_t kMaxWsFrameBytes    = 1024u * 1024u;
    static constexpr size_t kMaxTxQueueBytes    = 4u * 1024u * 1024u;
    static constexpr size_t kMaxClients         = 64u;

    using ConnectCallback    = std::function<void(ClientId aClientId)>;
    using DisconnectCallback = std::function<void(ClientId aClientId)>;
    using MessageCallback    = std::function<void(ClientId aClientId, const std::string &aMessage)>;

    WsServer(void) = default;
    ~WsServer(void);

    WsServer(const WsServer &)            = delete;
    WsServer &operator=(const WsServer &) = delete;

    bool ListenHttp(uint16_t aPort = 8999, const std::string &aBindAddr = "127.0.0.1");
    bool ListenRpcUnix(const std::string &aSocketPath);
    void SetWebDir(const std::filesystem::path &aWebDir) { mWebDir = aWebDir; }
    void Close(void);

    bool     IsListeningHttp(void) const { return mHttpListenFd >= 0; }
    bool     IsListeningUnix(void) const { return mUnixListenFd >= 0; }
    uint16_t GetHttpPort(void) const { return mHttpPort; }
    size_t   GetClientCount(void) const { return mClients.size(); }

    void SetOnConnect(ConnectCallback aCallback) { mOnConnect = std::move(aCallback); }
    void SetOnDisconnect(DisconnectCallback aCallback) { mOnDisconnect = std::move(aCallback); }
    void SetOnMessage(MessageCallback aCallback) { mOnMessage = std::move(aCallback); }

    bool SendText(ClientId aClientId, const std::string &aText);
    void BroadcastText(const std::string &aText);

    /**
     * Polls all active sockets (and optional @p aExtraReadFd such as STDIN_FILENO)
     * for up to @p aTimeoutMs milliseconds and processes ready I/O.
     */
    void PollOnce(int aTimeoutMs, int aExtraReadFd = -1, bool *aExtraFdReady = nullptr);

private:
    enum class ConnState : uint8_t
    {
        kHttpHandshake,
        kWebSocket,
        kUnixJsonRpc,
        kClosing,
    };

    struct Connection
    {
        ClientId             mId    = kInvalidClientId;
        int                  mFd    = -1;
        ConnState            mState = ConnState::kHttpHandshake;
        std::vector<uint8_t> mRxBuf;
        std::vector<uint8_t> mTxBuf;
        std::string          mWsFragment;
        uint8_t              mWsFragOpcode = 0;
        bool                 mCloseAfterTx = false;
    };

    void AcceptIncoming(int aListenFd, bool aIsUnix);
    void HandleReadable(Connection &aConn);
    void HandleWritable(Connection &aConn);
    void ProcessHttp(Connection &aConn);
    void ProcessWebSocket(Connection &aConn);
    void ProcessUnixJsonRpc(Connection &aConn);
    void QueueHttpResponse(Connection        &aConn,
                           int                aStatusCode,
                           const char        *aStatusText,
                           const std::string &aContentType,
                           const uint8_t     *aBody,
                           size_t             aBodyLen,
                           bool               aKeepAlive = false);
    void QueueWsFrame(Connection &aConn, uint8_t aOpcode, const uint8_t *aPayload, size_t aLength);
    void ServeStaticAsset(Connection &aConn, const std::string &aUrlPath);
    bool IsOriginAllowed(const std::string &aOrigin, const std::string &aHostHeader) const;
    void CloseConnection(ClientId aClientId);

    int                   mHttpListenFd           = -1;
    int                   mUnixListenFd           = -1;
    uint16_t              mHttpPort               = 0;
    bool                  mRestrictLoopbackOrigin = true;
    std::string           mUnixSocketPath;
    std::filesystem::path mWebDir;

    ClientId                       mNextClientId = 1;
    std::map<ClientId, Connection> mClients;
    ConnectCallback                mOnConnect;
    DisconnectCallback             mOnDisconnect;
    MessageCallback                mOnMessage;
};

} // namespace Server
} // namespace otns

#endif // OTNS_SERVER_WS_SERVER_HPP_
