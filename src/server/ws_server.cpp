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

#include "server/ws_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "core/path_security.hpp"
#include "server/embedded_assets.hpp"
#include "server/sha1_base64.hpp"

namespace otns {
namespace Server {

namespace {

static constexpr size_t kMaxStaticFileBytes = 16u * 1024u * 1024u;

bool SetNonBlocking(int aFd)
{
    int flags = ::fcntl(aFd, F_GETFL, 0);

    return (flags >= 0) && (::fcntl(aFd, F_SETFL, flags | O_NONBLOCK) == 0);
}

std::string ToLowerAscii(const std::string &aText)
{
    std::string out = aText;

    for (char &ch : out)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    return out;
}

std::string TrimWhitespace(const std::string &aText)
{
    size_t start = 0;
    size_t end   = aText.size();

    while (start < end && (aText[start] == ' ' || aText[start] == '\t' || aText[start] == '\r' || aText[start] == '\n'))
    {
        ++start;
    }
    while (end > start &&
           (aText[end - 1] == ' ' || aText[end - 1] == '\t' || aText[end - 1] == '\r' || aText[end - 1] == '\n'))
    {
        --end;
    }

    return aText.substr(start, end - start);
}

const char *GuessContentType(const std::string &aPath)
{
    size_t dot = aPath.rfind('.');

    if (dot == std::string::npos)
    {
        return "application/octet-stream";
    }

    std::string ext = ToLowerAscii(aPath.substr(dot));

    if (ext == ".html" || ext == ".htm")
    {
        return "text/html; charset=utf-8";
    }
    if (ext == ".js" || ext == ".mjs")
    {
        return "application/javascript; charset=utf-8";
    }
    if (ext == ".css")
    {
        return "text/css; charset=utf-8";
    }
    if (ext == ".json")
    {
        return "application/json; charset=utf-8";
    }
    if (ext == ".svg")
    {
        return "image/svg+xml";
    }
    if (ext == ".png")
    {
        return "image/png";
    }
    if (ext == ".ico")
    {
        return "image/x-icon";
    }

    return "application/octet-stream";
}

bool MatchesHostPrefix(const std::string &aOriginLower, const char *aPrefix)
{
    size_t prefixLen = std::strlen(aPrefix);

    if (aOriginLower.size() < prefixLen || aOriginLower.compare(0, prefixLen, aPrefix) != 0)
    {
        return false;
    }

    if (aOriginLower.size() == prefixLen)
    {
        return true;
    }

    if (aOriginLower[prefixLen] != ':')
    {
        return false;
    }

    // Remaining characters must be decimal port digits.
    if (prefixLen + 1 >= aOriginLower.size())
    {
        return false;
    }

    for (size_t i = prefixLen + 1; i < aOriginLower.size(); ++i)
    {
        if (aOriginLower[i] < '0' || aOriginLower[i] > '9')
        {
            return false;
        }
    }

    return true;
}

} // namespace

WsServer::~WsServer(void) { Close(); }

bool WsServer::ListenHttp(uint16_t aPort, const std::string &aBindAddr)
{
    if (mHttpListenFd >= 0)
    {
        ::close(mHttpListenFd);
        mHttpListenFd = -1;
    }

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
    if (!SetNonBlocking(fd) || ::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0 ||
        ::listen(fd, 64) < 0)
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

    mHttpListenFd           = fd;
    mHttpPort               = ntohs(addr.sin_port);
    mRestrictLoopbackOrigin = (aBindAddr == "127.0.0.1" || aBindAddr == "localhost" || aBindAddr == "::1");

    return true;
}

bool WsServer::ListenRpcUnix(const std::string &aSocketPath)
{
    if (mUnixListenFd >= 0)
    {
        ::close(mUnixListenFd);
        mUnixListenFd = -1;
    }
    if (!mUnixSocketPath.empty())
    {
        (void)::unlink(mUnixSocketPath.c_str());
        mUnixSocketPath.clear();
    }

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

    if (!SetNonBlocking(fd) || ::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0 ||
        ::listen(fd, 32) < 0)
    {
        ::close(fd);
        (void)::unlink(aSocketPath.c_str());
        return false;
    }

    mUnixListenFd   = fd;
    mUnixSocketPath = aSocketPath;

    return true;
}

void WsServer::Close(void)
{
    for (auto &pair : mClients)
    {
        if (pair.second.mFd >= 0)
        {
            ::close(pair.second.mFd);
            pair.second.mFd = -1;
        }
    }
    mClients.clear();

    if (mHttpListenFd >= 0)
    {
        ::close(mHttpListenFd);
        mHttpListenFd = -1;
    }
    if (mUnixListenFd >= 0)
    {
        ::close(mUnixListenFd);
        mUnixListenFd = -1;
    }
    if (!mUnixSocketPath.empty())
    {
        (void)::unlink(mUnixSocketPath.c_str());
        mUnixSocketPath.clear();
    }
    mHttpPort = 0;
}

bool WsServer::IsOriginAllowed(const std::string &aOrigin) const
{
    if (!mRestrictLoopbackOrigin || aOrigin.empty())
    {
        return true;
    }

    std::string lower = ToLowerAscii(TrimWhitespace(aOrigin));

    return MatchesHostPrefix(lower, "http://127.0.0.1") || MatchesHostPrefix(lower, "http://localhost") ||
           MatchesHostPrefix(lower, "http://[::1]") || MatchesHostPrefix(lower, "https://127.0.0.1") ||
           MatchesHostPrefix(lower, "https://localhost");
}

bool WsServer::SendText(ClientId aClientId, const std::string &aText)
{
    auto it = mClients.find(aClientId);

    if (it == mClients.end())
    {
        return false;
    }

    Connection &conn = it->second;

    if (conn.mState == ConnState::kWebSocket)
    {
        QueueWsFrame(conn, 0x01u, reinterpret_cast<const uint8_t *>(aText.data()), aText.size());
        return true;
    }

    if (conn.mState == ConnState::kUnixJsonRpc)
    {
        if (conn.mTxBuf.size() + aText.size() + 1 > kMaxTxQueueBytes)
        {
            conn.mState = ConnState::kClosing;
            return false;
        }
        conn.mTxBuf.insert(conn.mTxBuf.end(), aText.begin(), aText.end());
        conn.mTxBuf.push_back('\n');
        return true;
    }

    return false;
}

void WsServer::BroadcastText(const std::string &aText)
{
    for (auto &pair : mClients)
    {
        if (pair.second.mState == ConnState::kWebSocket || pair.second.mState == ConnState::kUnixJsonRpc)
        {
            (void)SendText(pair.first, aText);
        }
    }
}

void WsServer::QueueWsFrame(Connection &aConn, uint8_t aOpcode, const uint8_t *aPayload, size_t aLength)
{
    if (aConn.mTxBuf.size() + aLength + 16u > kMaxTxQueueBytes)
    {
        aConn.mState = ConnState::kClosing;
        return;
    }

    aConn.mTxBuf.push_back(static_cast<uint8_t>(0x80u | (aOpcode & 0x0Fu)));

    if (aLength <= 125u)
    {
        aConn.mTxBuf.push_back(static_cast<uint8_t>(aLength));
    }
    else if (aLength <= 65535u)
    {
        aConn.mTxBuf.push_back(126u);
        aConn.mTxBuf.push_back(static_cast<uint8_t>((aLength >> 8u) & 0xFFu));
        aConn.mTxBuf.push_back(static_cast<uint8_t>(aLength & 0xFFu));
    }
    else
    {
        aConn.mTxBuf.push_back(127u);
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            aConn.mTxBuf.push_back(static_cast<uint8_t>((static_cast<uint64_t>(aLength) >> shift) & 0xFFu));
        }
    }

    if (aPayload != nullptr && aLength > 0)
    {
        aConn.mTxBuf.insert(aConn.mTxBuf.end(), aPayload, aPayload + aLength);
    }
}

void WsServer::QueueHttpResponse(Connection        &aConn,
                                 int                aStatusCode,
                                 const char        *aStatusText,
                                 const std::string &aContentType,
                                 const uint8_t     *aBody,
                                 size_t             aBodyLen,
                                 bool               aKeepAlive)
{
    std::string header;

    header.reserve(512);
    header += "HTTP/1.1 " + std::to_string(aStatusCode) + " " + aStatusText + "\r\n";
    header += "Content-Type: " + aContentType + "\r\n";
    header += "Content-Length: " + std::to_string(aBodyLen) + "\r\n";
    header += "X-Content-Type-Options: nosniff\r\n";
    header += "X-Frame-Options: DENY\r\n";
    header += "Referrer-Policy: no-referrer\r\n";
    header += "Content-Security-Policy: default-src 'self'; connect-src 'self' ws://127.0.0.1:* ws://localhost:* "
              "ws://[::1]:*; style-src 'self' 'unsafe-inline'; object-src 'none'; base-uri 'none'\r\n";
    header += aKeepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";

    aConn.mTxBuf.insert(aConn.mTxBuf.end(), header.begin(), header.end());
    if (aBody != nullptr && aBodyLen > 0)
    {
        aConn.mTxBuf.insert(aConn.mTxBuf.end(), aBody, aBody + aBodyLen);
    }
    aConn.mCloseAfterTx = !aKeepAlive;
}

void WsServer::ServeStaticAsset(Connection &aConn, const std::string &aUrlPath)
{
    std::string cleanPath = aUrlPath;
    size_t      qpos      = cleanPath.find('?');

    if (qpos != std::string::npos)
    {
        cleanPath.resize(qpos);
    }
    if (cleanPath.empty() || cleanPath == "/")
    {
        cleanPath = "/index.html";
    }

    if (cleanPath == "/healthz")
    {
        static constexpr char kOkBody[] = "{\"status\":\"ok\"}\n";

        QueueHttpResponse(aConn, 200, "OK", "application/json; charset=utf-8",
                          reinterpret_cast<const uint8_t *>(kOkBody), sizeof(kOkBody) - 1);
        return;
    }

    // Reject path traversal attempts up front.
    if (cleanPath[0] != '/' || cleanPath.find("..") != std::string::npos || cleanPath.find('\\') != std::string::npos ||
        cleanPath.find('\0') != std::string::npos)
    {
        static constexpr char kBad[] = "400 Bad Request\n";

        QueueHttpResponse(aConn, 400, "Bad Request", "text/plain; charset=utf-8",
                          reinterpret_cast<const uint8_t *>(kBad), sizeof(kBad) - 1);
        return;
    }

    // 1. Optional --web-dir development overlay with strict canonicalized containment.
    if (!mWebDir.empty())
    {
        std::filesystem::path resolved;
        std::string           rel = cleanPath.substr(1);

        if (Core::ResolveContainedPath(mWebDir, rel, resolved))
        {
            std::error_code ec;
            uintmax_t       sz = std::filesystem::file_size(resolved, ec);

            if (!ec && std::filesystem::is_regular_file(resolved, ec) && !ec && sz <= kMaxStaticFileBytes)
            {
                std::FILE *fp = std::fopen(resolved.c_str(), "rb");

                if (fp != nullptr)
                {
                    std::vector<uint8_t> data(static_cast<size_t>(sz));
                    size_t               n = (sz > 0) ? std::fread(data.data(), 1, data.size(), fp) : 0;

                    std::fclose(fp);
                    if (n == data.size())
                    {
                        QueueHttpResponse(aConn, 200, "OK", GuessContentType(cleanPath), data.data(), data.size());
                        return;
                    }
                }
            }
        }
    }

    // 2. Build-time embedded static web assets.
    if (const EmbeddedAsset *asset = FindEmbeddedAsset(cleanPath))
    {
        QueueHttpResponse(aConn, 200, "OK", asset->mContentType, asset->mData, asset->mSize);
        return;
    }

    static constexpr char kNotFound[] = "404 Not Found\n";

    QueueHttpResponse(aConn, 404, "Not Found", "text/plain; charset=utf-8",
                      reinterpret_cast<const uint8_t *>(kNotFound), sizeof(kNotFound) - 1);
}

void WsServer::AcceptIncoming(int aListenFd, bool aIsUnix)
{
    while (true)
    {
        int clientFd = ::accept(aListenFd, nullptr, nullptr);

        if (clientFd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        if (!SetNonBlocking(clientFd))
        {
            ::close(clientFd);
            continue;
        }

#ifdef SO_NOSIGPIPE
        int one = 1;

        (void)::setsockopt(clientFd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        if (!aIsUnix)
        {
            int nodelay = 1;

            (void)::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        }

        ClientId   id = mNextClientId++;
        Connection conn;

        conn.mId    = id;
        conn.mFd    = clientFd;
        conn.mState = aIsUnix ? ConnState::kUnixJsonRpc : ConnState::kHttpHandshake;

        mClients.emplace(id, std::move(conn));
        if (aIsUnix && mOnConnect)
        {
            mOnConnect(id);
        }
    }
}

void WsServer::ProcessHttp(Connection &aConn)
{
    static constexpr char kHeaderEnd[] = "\r\n\r\n";

    if (aConn.mRxBuf.size() > kMaxHttpHeaderBytes)
    {
        static constexpr char kTooBig[] = "431 Request Header Fields Too Large\n";

        QueueHttpResponse(aConn, 431, "Request Header Fields Too Large", "text/plain; charset=utf-8",
                          reinterpret_cast<const uint8_t *>(kTooBig), sizeof(kTooBig) - 1);
        return;
    }

    std::string text(reinterpret_cast<const char *>(aConn.mRxBuf.data()), aConn.mRxBuf.size());
    size_t      hdrEnd = text.find(kHeaderEnd);

    if (hdrEnd == std::string::npos)
    {
        return;
    }

    std::string reqBlock = text.substr(0, hdrEnd);

    aConn.mRxBuf.erase(aConn.mRxBuf.begin(), aConn.mRxBuf.begin() + static_cast<std::ptrdiff_t>(hdrEnd + 4));

    // Parse request line: METHOD TARGET HTTP/1.1
    size_t      firstCrLf = reqBlock.find("\r\n");
    std::string reqLine   = (firstCrLf == std::string::npos) ? reqBlock : reqBlock.substr(0, firstCrLf);
    size_t      sp1       = reqLine.find(' ');
    size_t      sp2       = (sp1 != std::string::npos) ? reqLine.find(' ', sp1 + 1) : std::string::npos;

    if (sp1 == std::string::npos || sp2 == std::string::npos)
    {
        static constexpr char kBad[] = "400 Bad Request\n";

        QueueHttpResponse(aConn, 400, "Bad Request", "text/plain; charset=utf-8",
                          reinterpret_cast<const uint8_t *>(kBad), sizeof(kBad) - 1);
        return;
    }

    std::string method = reqLine.substr(0, sp1);
    std::string target = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);

    if (method != "GET")
    {
        static constexpr char kMethod[] = "405 Method Not Allowed\n";

        QueueHttpResponse(aConn, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                          reinterpret_cast<const uint8_t *>(kMethod), sizeof(kMethod) - 1);
        return;
    }

    // Parse headers (lowercased keys).
    std::map<std::string, std::string> headers;

    if (firstCrLf != std::string::npos)
    {
        size_t pos = firstCrLf + 2;

        while (pos < reqBlock.size())
        {
            size_t next = reqBlock.find("\r\n", pos);
            size_t len  = (next == std::string::npos) ? (reqBlock.size() - pos) : (next - pos);
            size_t col  = reqBlock.find(':', pos);

            if (col != std::string::npos && col < pos + len)
            {
                std::string key = ToLowerAscii(TrimWhitespace(reqBlock.substr(pos, col - pos)));
                std::string val = TrimWhitespace(reqBlock.substr(col + 1, (pos + len) - (col + 1)));

                headers[key] = val;
            }
            if (next == std::string::npos)
            {
                break;
            }
            pos = next + 2;
        }
    }

    std::string cleanTarget = target;
    size_t      qpos        = cleanTarget.find('?');

    if (qpos != std::string::npos)
    {
        cleanTarget.resize(qpos);
    }

    if (cleanTarget == "/ws")
    {
        std::string upgrade = ToLowerAscii(headers["upgrade"]);
        std::string connHdr = ToLowerAscii(headers["connection"]);
        std::string wsVer   = headers["sec-websocket-version"];
        std::string wsKey   = headers["sec-websocket-key"];
        std::string origin  = headers["origin"];

        if (!IsOriginAllowed(origin))
        {
            static constexpr char kForb[] = "403 Forbidden Origin\n";

            QueueHttpResponse(aConn, 403, "Forbidden", "text/plain; charset=utf-8",
                              reinterpret_cast<const uint8_t *>(kForb), sizeof(kForb) - 1);
            return;
        }

        if (upgrade != "websocket" || connHdr.find("upgrade") == std::string::npos || wsVer != "13" || wsKey.empty())
        {
            static constexpr char kBadWs[] = "400 Invalid WebSocket Upgrade\n";

            QueueHttpResponse(aConn, 400, "Bad Request", "text/plain; charset=utf-8",
                              reinterpret_cast<const uint8_t *>(kBadWs), sizeof(kBadWs) - 1);
            return;
        }

        std::string accept = ComputeWebSocketAccept(wsKey);
        std::string resp   = "HTTP/1.1 101 Switching Protocols\r\n"
                             "Upgrade: websocket\r\n"
                             "Connection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: " +
                           accept + "\r\n\r\n";

        aConn.mTxBuf.insert(aConn.mTxBuf.end(), resp.begin(), resp.end());
        aConn.mState = ConnState::kWebSocket;

        if (mOnConnect)
        {
            mOnConnect(aConn.mId);
        }

        // Process any WebSocket frames already buffered behind the upgrade header.
        if (!aConn.mRxBuf.empty())
        {
            ProcessWebSocket(aConn);
        }
        return;
    }

    ServeStaticAsset(aConn, target);
}

void WsServer::ProcessWebSocket(Connection &aConn)
{
    while (aConn.mState == ConnState::kWebSocket && aConn.mRxBuf.size() >= 2)
    {
        uint8_t  b0        = aConn.mRxBuf[0];
        uint8_t  b1        = aConn.mRxBuf[1];
        bool     fin       = (b0 & 0x80u) != 0;
        uint8_t  opcode    = b0 & 0x0Fu;
        bool     masked    = (b1 & 0x80u) != 0;
        uint64_t payloadLn = b1 & 0x7Fu;
        size_t   headerLen = 2;

        // RFC 6455 Section 5.1: client-to-server frames MUST be masked.
        if (!masked)
        {
            aConn.mState = ConnState::kClosing;
            return;
        }

        if (payloadLn == 126u)
        {
            if (aConn.mRxBuf.size() < 4)
            {
                return;
            }
            payloadLn = (static_cast<uint64_t>(aConn.mRxBuf[2]) << 8u) | aConn.mRxBuf[3];
            headerLen = 4;
        }
        else if (payloadLn == 127u)
        {
            if (aConn.mRxBuf.size() < 10)
            {
                return;
            }
            payloadLn = 0;
            for (size_t i = 0; i < 8u; ++i)
            {
                payloadLn = (payloadLn << 8u) | aConn.mRxBuf[2 + i];
            }
            headerLen = 10;
        }

        if (payloadLn > kMaxWsFrameBytes)
        {
            QueueWsFrame(aConn, 0x08u, nullptr, 0);
            aConn.mCloseAfterTx = true;
            return;
        }

        size_t totalFrame = headerLen + 4u + static_cast<size_t>(payloadLn);

        if (aConn.mRxBuf.size() < totalFrame)
        {
            return;
        }

        uint8_t maskKey[4];

        std::memcpy(maskKey, aConn.mRxBuf.data() + headerLen, 4);

        std::string payload(static_cast<size_t>(payloadLn), '\0');

        for (size_t i = 0; i < static_cast<size_t>(payloadLn); ++i)
        {
            payload[i] = static_cast<char>(aConn.mRxBuf[headerLen + 4u + i] ^ maskKey[i & 3u]);
        }

        aConn.mRxBuf.erase(aConn.mRxBuf.begin(), aConn.mRxBuf.begin() + static_cast<std::ptrdiff_t>(totalFrame));

        if (opcode == 0x08u) // Close
        {
            QueueWsFrame(aConn, 0x08u, nullptr, 0);
            aConn.mCloseAfterTx = true;
            return;
        }
        if (opcode == 0x09u) // Ping -> Pong
        {
            QueueWsFrame(aConn, 0x0Au, reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
            continue;
        }
        if (opcode == 0x0Au) // Pong
        {
            continue;
        }

        if (opcode == 0x01u || opcode == 0x02u)
        {
            aConn.mWsFragOpcode = opcode;
            aConn.mWsFragment   = std::move(payload);
        }
        else if (opcode == 0x00u)
        {
            if (aConn.mWsFragment.size() + payload.size() > kMaxWsFrameBytes)
            {
                aConn.mState = ConnState::kClosing;
                return;
            }
            aConn.mWsFragment.append(payload);
        }
        else
        {
            aConn.mState = ConnState::kClosing;
            return;
        }

        if (fin && aConn.mWsFragOpcode == 0x01u)
        {
            std::string message;

            message.swap(aConn.mWsFragment);
            aConn.mWsFragOpcode = 0;
            if (mOnMessage)
            {
                mOnMessage(aConn.mId, message);
            }
        }
    }
}

void WsServer::ProcessUnixJsonRpc(Connection &aConn)
{
    while (aConn.mState == ConnState::kUnixJsonRpc)
    {
        auto it = std::find(aConn.mRxBuf.begin(), aConn.mRxBuf.end(), static_cast<uint8_t>('\n'));

        if (it == aConn.mRxBuf.end())
        {
            if (aConn.mRxBuf.size() > kMaxWsFrameBytes)
            {
                aConn.mState = ConnState::kClosing;
            }
            return;
        }

        std::string line(aConn.mRxBuf.begin(), it);

        aConn.mRxBuf.erase(aConn.mRxBuf.begin(), it + 1);
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (!line.empty() && mOnMessage)
        {
            mOnMessage(aConn.mId, line);
        }
    }
}

void WsServer::HandleReadable(Connection &aConn)
{
    uint8_t buf[4096];

    while (true)
    {
        ssize_t n = ::recv(aConn.mFd, buf, sizeof(buf), 0);

        if (n > 0)
        {
            if (aConn.mRxBuf.size() + static_cast<size_t>(n) > kMaxWsFrameBytes + kMaxHttpHeaderBytes)
            {
                aConn.mState = ConnState::kClosing;
                return;
            }
            aConn.mRxBuf.insert(aConn.mRxBuf.end(), buf, buf + n);
            continue;
        }

        if (n == 0)
        {
            aConn.mState = ConnState::kClosing;
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

        aConn.mState = ConnState::kClosing;
        return;
    }

    if (aConn.mState == ConnState::kHttpHandshake)
    {
        ProcessHttp(aConn);
    }
    else if (aConn.mState == ConnState::kWebSocket)
    {
        ProcessWebSocket(aConn);
    }
    else if (aConn.mState == ConnState::kUnixJsonRpc)
    {
        ProcessUnixJsonRpc(aConn);
    }
}

void WsServer::HandleWritable(Connection &aConn)
{
    while (!aConn.mTxBuf.empty())
    {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t n = ::send(aConn.mFd, aConn.mTxBuf.data(), aConn.mTxBuf.size(), flags);

        if (n > 0)
        {
            aConn.mTxBuf.erase(aConn.mTxBuf.begin(), aConn.mTxBuf.begin() + n);
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
        aConn.mState = ConnState::kClosing;
        return;
    }

    if (aConn.mTxBuf.empty() && aConn.mCloseAfterTx)
    {
        aConn.mState = ConnState::kClosing;
    }
}

void WsServer::CloseConnection(ClientId aClientId)
{
    auto it = mClients.find(aClientId);

    if (it == mClients.end())
    {
        return;
    }

    bool notifyDisconnect = (it->second.mState == ConnState::kWebSocket ||
                             it->second.mState == ConnState::kUnixJsonRpc || it->second.mState == ConnState::kClosing);

    if (it->second.mFd >= 0)
    {
        ::close(it->second.mFd);
        it->second.mFd = -1;
    }
    mClients.erase(it);

    if (notifyDisconnect && mOnDisconnect)
    {
        mOnDisconnect(aClientId);
    }
}

void WsServer::PollOnce(int aTimeoutMs, int aExtraReadFd, bool *aExtraFdReady)
{
    if (aExtraFdReady != nullptr)
    {
        *aExtraFdReady = false;
    }

    std::vector<struct pollfd> pfds;
    std::vector<ClientId>      connIds;

    pfds.reserve(mClients.size() + 3);
    connIds.reserve(mClients.size());

    size_t httpIdx  = static_cast<size_t>(-1);
    size_t unixIdx  = static_cast<size_t>(-1);
    size_t extraIdx = static_cast<size_t>(-1);

    if (mHttpListenFd >= 0)
    {
        httpIdx = pfds.size();
        pfds.push_back({mHttpListenFd, POLLIN, 0});
    }
    if (mUnixListenFd >= 0)
    {
        unixIdx = pfds.size();
        pfds.push_back({mUnixListenFd, POLLIN, 0});
    }
    if (aExtraReadFd >= 0)
    {
        extraIdx = pfds.size();
        pfds.push_back({aExtraReadFd, POLLIN, 0});
    }

    size_t clientsStart = pfds.size();

    for (const auto &pair : mClients)
    {
        short ev = POLLIN;

        if (!pair.second.mTxBuf.empty())
        {
            ev = static_cast<short>(ev | POLLOUT);
        }
        pfds.push_back({pair.second.mFd, ev, 0});
        connIds.push_back(pair.first);
    }

    if (pfds.empty())
    {
        return;
    }

    int pr = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), aTimeoutMs);

    if (pr <= 0)
    {
        return;
    }

    if (httpIdx != static_cast<size_t>(-1) && (pfds[httpIdx].revents & POLLIN) != 0)
    {
        AcceptIncoming(mHttpListenFd, false);
    }
    if (unixIdx != static_cast<size_t>(-1) && (pfds[unixIdx].revents & POLLIN) != 0)
    {
        AcceptIncoming(mUnixListenFd, true);
    }
    if (extraIdx != static_cast<size_t>(-1) && (pfds[extraIdx].revents & (POLLIN | POLLHUP)) != 0)
    {
        if (aExtraFdReady != nullptr)
        {
            *aExtraFdReady = true;
        }
    }

    std::vector<ClientId> toClose;

    for (size_t i = 0; i < connIds.size(); ++i)
    {
        short rev = pfds[clientsStart + i].revents;

        if (rev == 0)
        {
            continue;
        }

        auto it = mClients.find(connIds[i]);

        if (it == mClients.end())
        {
            continue;
        }

        if ((rev & (POLLERR | POLLNVAL)) != 0)
        {
            toClose.push_back(connIds[i]);
            continue;
        }
        if ((rev & (POLLIN | POLLHUP)) != 0)
        {
            HandleReadable(it->second);
        }
        if (it->second.mState != ConnState::kClosing && !it->second.mTxBuf.empty())
        {
            HandleWritable(it->second);
        }
        if (it->second.mState == ConnState::kClosing)
        {
            toClose.push_back(connIds[i]);
        }
    }

    for (ClientId id : toClose)
    {
        CloseConnection(id);
    }
}

} // namespace Server
} // namespace otns
