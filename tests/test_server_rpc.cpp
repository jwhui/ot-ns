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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cli/repl.hpp"
#include "core/simulation.hpp"
#include "doctest/doctest.h"
#include "server/replay.hpp"
#include "server/rpc_handler.hpp"
#include "server/sha1_base64.hpp"
#include "server/viz_publisher.hpp"
#include "server/ws_server.hpp"

using namespace otns::Core;
using namespace otns::Server;
using namespace otns::Cli;

namespace {

int ConnectLoopback(uint16_t aPort)
{
    int                fd = ::socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;

    if (fd < 0)
    {
        return -1;
    }

    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(aPort);
    (void)::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        return -1;
    }

    return fd;
}

std::string ReadHttpResponse(int aFd)
{
    std::string out;
    char        buf[2048];

    while (true)
    {
        ssize_t n = ::recv(aFd, buf, sizeof(buf), 0);

        if (n <= 0)
        {
            break;
        }
        out.append(buf, static_cast<size_t>(n));
    }

    return out;
}

std::vector<uint8_t> MakeMaskedWsTextFrame(const std::string &aText)
{
    std::vector<uint8_t> frame;
    const uint8_t        mask[4] = {0x37, 0xFA, 0x21, 0x3D};

    frame.push_back(0x81u);
    if (aText.size() <= 125u)
    {
        frame.push_back(static_cast<uint8_t>(0x80u | aText.size()));
    }
    else
    {
        frame.push_back(0x80u | 126u);
        frame.push_back(static_cast<uint8_t>((aText.size() >> 8u) & 0xFFu));
        frame.push_back(static_cast<uint8_t>(aText.size() & 0xFFu));
    }
    frame.insert(frame.end(), mask, mask + 4);
    for (size_t i = 0; i < aText.size(); ++i)
    {
        frame.push_back(static_cast<uint8_t>(static_cast<uint8_t>(aText[i]) ^ mask[i & 3u]));
    }

    return frame;
}

bool RecvWsTextFrame(int aFd, std::vector<uint8_t> &aBuf, std::string &aOut)
{
    while (true)
    {
        if (aBuf.size() >= 2)
        {
            size_t len    = aBuf[1] & 0x7Fu;
            size_t hdrLen = 2;

            if (len == 126u)
            {
                if (aBuf.size() >= 4)
                {
                    len    = (static_cast<size_t>(aBuf[2]) << 8u) | aBuf[3];
                    hdrLen = 4;
                }
                else
                {
                    len = static_cast<size_t>(-1);
                }
            }
            if (len != static_cast<size_t>(-1) && aBuf.size() >= hdrLen + len)
            {
                aOut.assign(reinterpret_cast<const char *>(aBuf.data() + hdrLen), len);
                aBuf.erase(aBuf.begin(), aBuf.begin() + static_cast<std::ptrdiff_t>(hdrLen + len));
                return true;
            }
        }

        uint8_t tmp[4096];
        ssize_t n = ::recv(aFd, tmp, sizeof(tmp), 0);

        if (n <= 0)
        {
            return false;
        }
        aBuf.insert(aBuf.end(), tmp, tmp + n);
    }
}

struct TempOutDir
{
    TempOutDir(void)
    {
        std::error_code ec;

        mPath =
            std::filesystem::temp_directory_path(ec) / ("otns_v3_srv_" + std::to_string(static_cast<long>(::getpid())));
        std::filesystem::remove_all(mPath, ec);
        std::filesystem::create_directories(mPath, ec);
    }

    ~TempOutDir(void)
    {
        std::error_code ec;

        std::filesystem::remove_all(mPath, ec);
    }

    std::filesystem::path mPath;
};

} // namespace

TEST_CASE("RFC 6455 Sec-WebSocket-Accept SHA-1 + Base64 test vector")
{
    // Exact RFC 6455 Section 1.3 example vector.
    CHECK(ComputeWebSocketAccept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("WsServer enforces HTTP security headers, traversal rejection, and WebSocket Origin checks")
{
    WsServer server;

    CHECK(server.ListenHttp(0, "127.0.0.1"));

    uint16_t    port = server.GetHttpPort();
    std::string healthResp;
    std::string indexResp;
    std::string travResp;
    std::string badOriginResp;

    std::thread clientThread([&](void) {
        auto doReq = [port](const std::string &aReq) -> std::string {
            int fd = ConnectLoopback(port);

            if (fd < 0)
            {
                return "";
            }
            (void)::send(fd, aReq.data(), aReq.size(), 0);
            std::string r = ReadHttpResponse(fd);

            ::close(fd);
            return r;
        };

        healthResp    = doReq("GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        indexResp     = doReq("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        travResp      = doReq("GET /../../etc/passwd HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        badOriginResp = doReq("GET /ws HTTP/1.1\r\n"
                              "Host: 127.0.0.1\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                              "Sec-WebSocket-Version: 13\r\n"
                              "Origin: https://evil.example.com\r\n\r\n");
    });

    for (int i = 0; i < 60; ++i)
    {
        server.PollOnce(10);
    }
    clientThread.join();

    CHECK(healthResp.find("200 OK") != std::string::npos);
    CHECK(healthResp.find("X-Content-Type-Options: nosniff") != std::string::npos);
    CHECK(healthResp.find("X-Frame-Options: DENY") != std::string::npos);
    CHECK(healthResp.find("Content-Security-Policy:") != std::string::npos);
    CHECK(indexResp.find("200 OK") != std::string::npos);
    CHECK(travResp.find("400 Bad Request") != std::string::npos);
    CHECK(badOriginResp.find("403 Forbidden") != std::string::npos);
}

TEST_CASE("WebSocket JSON-RPC, VizPublisher coalesced diffs, JSONL replay, and CLI REPL")
{
    TempOutDir    outDir;
    Simulation    sim;
    WsServer      server;
    JsonlRecorder recorder;

    CHECK(server.ListenHttp(0, "127.0.0.1"));

    VizPublisher viz(sim, server, &recorder);
    RpcHandler   rpc(sim, server, viz, recorder, outDir.mPath);

    sim.SetPacketObserver([&viz](const PacketObservation &aObs) { viz.OnPacket(aObs); });
    server.SetOnConnect([&viz, &rpc](ClientId aId) { viz.OnClientConnect(aId, rpc.IsPaused()); });
    server.SetOnDisconnect([&viz](ClientId aId) { viz.OnClientDisconnect(aId); });
    server.SetOnMessage([&rpc](ClientId aId, const std::string &aMsg) { rpc.HandleMessage(aId, aMsg); });

    uint16_t port      = server.GetHttpPort();
    bool     clientOk  = false;
    bool     gotSnap   = false;
    bool     gotAddRes = false;
    bool     gotForb   = false;
    bool     gotDiff   = false;

    std::thread wsClient([&](void) {
        int fd = ConnectLoopback(port);

        if (fd < 0)
        {
            return;
        }

        std::string upgrade = "GET /ws HTTP/1.1\r\n"
                              "Host: 127.0.0.1\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              "Origin: http://127.0.0.1:" +
                              std::to_string(port) +
                              "\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                              "Sec-WebSocket-Version: 13\r\n\r\n";

        (void)::send(fd, upgrade.data(), upgrade.size(), 0);

        std::vector<uint8_t> rx;
        std::string          hdr;

        while (hdr.find("\r\n\r\n") == std::string::npos)
        {
            uint8_t tmp[1024];
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);

            if (n <= 0)
            {
                ::close(fd);
                return;
            }
            rx.insert(rx.end(), tmp, tmp + n);
            hdr.assign(reinterpret_cast<const char *>(rx.data()), rx.size());
        }

        size_t hdrEnd = hdr.find("\r\n\r\n");

        if (hdr.find("101 Switching Protocols") == std::string::npos)
        {
            ::close(fd);
            return;
        }
        rx.erase(rx.begin(), rx.begin() + static_cast<std::ptrdiff_t>(hdrEnd + 4));

        // 1. Expect initial event.snapshot on connect.
        std::string msg;

        if (RecvWsTextFrame(fd, rx, msg))
        {
            auto j = nlohmann::json::parse(msg, nullptr, false);

            gotSnap = (!j.is_discarded() && j["type"] == "event.snapshot");
        }

        auto sendRpc = [&](const std::string &aJson) {
            auto f = MakeMaskedWsTextFrame(aJson);

            (void)::send(fd, f.data(), f.size(), 0);
        };

        // 2. Start replay recording, add a node, advance 10s, and attempt path escape.
        sendRpc(R"({"v":1,"type":"rpc.req","id":1,"method":"replay.start","params":{"path":"rec.jsonl"}})");
        sendRpc(R"({"v":1,"type":"rpc.req","id":2,"method":"node.add","params":{"type":"router","x":10,"y":20}})");
        sendRpc(R"({"v":1,"type":"rpc.req","id":3,"method":"sim.go","params":{"duration_us":10000000,"speed":0}})");
        sendRpc(R"({"v":1,"type":"rpc.req","id":4,"method":"kpi.save","params":{"path":"../evil.json"}})");
        sendRpc(R"({"v":1,"type":"rpc.req","id":5,"method":"replay.stop","params":{}})");

        bool sawDone = false;

        while (!sawDone && RecvWsTextFrame(fd, rx, msg))
        {
            auto j = nlohmann::json::parse(msg, nullptr, false);

            if (j.is_discarded())
            {
                continue;
            }
            if (j["type"] == "event.batch_diff")
            {
                gotDiff = true;
            }
            if (j["type"] == "rpc.res")
            {
                if (j["id"] == 2 && j.contains("result") && j["result"]["node_id"] == 1)
                {
                    gotAddRes = true;
                }
                if (j["id"] == 4 && j.contains("error") && j["error"]["code"] == "FORBIDDEN_PATH")
                {
                    gotForb = true;
                }
                if (j["id"] == 5)
                {
                    sawDone = true;
                }
            }
        }

        clientOk = sawDone;
        ::close(fd);
    });

    for (int i = 0; i < 120 && !clientOk; ++i)
    {
        server.PollOnce(15);
    }
    wsClient.join();

    CHECK(clientOk);
    CHECK(gotSnap);
    CHECK(gotAddRes);
    CHECK(gotDiff);
    CHECK(gotForb);

    // Verify recorded JSONL file reloads cleanly via JsonlReplayer.
    JsonlReplayer replayer;
    std::string   err;

    CHECK(replayer.LoadContained(outDir.mPath, "rec.jsonl", err));
    CHECK(err.empty());
    CHECK(replayer.GetEventCount() >= 2);
    CHECK_FALSE(replayer.PopDueEvents(10000000ULL).empty());

    // Verify interactive CLI REPL.
    Repl               repl(sim, viz, rpc, recorder, outDir.mPath);
    std::ostringstream oss;

    CHECK(repl.ExecuteLine("stats", oss));
    CHECK(oss.str().find("leaders=1") != std::string::npos);

    oss.str("");
    CHECK(repl.ExecuteLine("node 1 state", oss));
    CHECK(oss.str().find("leader") != std::string::npos);
    CHECK_FALSE(repl.ExecuteLine("quit", oss));
}
