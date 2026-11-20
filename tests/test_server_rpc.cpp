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
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cli/repl.hpp"
#include "core/path_security.hpp"
#include "core/simulation.hpp"
#include "doctest/doctest.h"
#include "nodes/rfsim_server.hpp"
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

TEST_CASE("WsServer enforces Origin check on 0.0.0.0 bind and owner-only Unix socket permissions")
{
    TempOutDir outDir;
    WsServer   server;
    auto       sockPath = outDir.mPath / "rpc.sock";

    CHECK(server.ListenHttp(0, "0.0.0.0"));
    CHECK(server.ListenRpcUnix(sockPath.string()));

    struct stat st;

    CHECK(::stat(sockPath.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    uint16_t    port = server.GetHttpPort();
    std::string resp;
    bool        done = false;

    std::thread client([&](void) {
        int fd = ConnectLoopback(port);

        if (fd >= 0)
        {
            std::string req = "GET /ws HTTP/1.1\r\n"
                              "Host: 127.0.0.1:" +
                              std::to_string(port) +
                              "\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              "Origin: https://evil.example\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                              "Sec-WebSocket-Version: 13\r\n\r\n";

            (void)::send(fd, req.data(), req.size(), 0);
            char    buf[1024];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);

            if (n > 0)
            {
                resp.assign(buf, static_cast<size_t>(n));
            }
            ::close(fd);
        }
        done = true;
    });

    for (int i = 0; i < 60 && !done; ++i)
    {
        server.PollOnce(10);
    }
    client.join();

    CHECK(resp.find("403 Forbidden") != std::string::npos);
}

TEST_CASE("Executable allowlist security and node label sanitization")
{
    TempOutDir            outDir;
    std::filesystem::path resolved;

    CHECK(SanitizeNodeLabel("matter-light:1") == "matter-light:1");
    CHECK(SanitizeNodeLabel("bad label; rm -rf /") == "badlabelrm-rf/");
    CHECK(SanitizeNodeLabel(std::string(64, 'a')).size() == 32);

    // Empty allowlist must always deny.
    CHECK_FALSE(ResolveAllowedExecutable({}, "/bin/sh", resolved));

    // Create a non-executable and an executable file inside outDir.
    auto nonExec = outDir.mPath / "data.txt";
    auto okExec  = outDir.mPath / "mock_node";

    {
        FILE *fp = std::fopen(nonExec.c_str(), "wb");

        CHECK(fp != nullptr);
        if (fp != nullptr)
        {
            std::fputs("hello\n", fp);
            std::fclose(fp);
        }
        (void)::chmod(nonExec.c_str(), 0644);
    }
    {
        FILE *fp = std::fopen(okExec.c_str(), "wb");

        CHECK(fp != nullptr);
        if (fp != nullptr)
        {
            std::fputs("#!/bin/sh\n", fp);
            std::fclose(fp);
        }
        (void)::chmod(okExec.c_str(), 0755);
    }

    std::vector<std::filesystem::path> allowDir = {outDir.mPath};

    CHECK_FALSE(ResolveAllowedExecutable(allowDir, nonExec.string(), resolved));
    CHECK_FALSE(ResolveAllowedExecutable(allowDir, "/bin/sh", resolved));
    CHECK_FALSE(ResolveAllowedExecutable(allowDir, (outDir.mPath / "../etc/passwd").string(), resolved));
    CHECK_FALSE(ResolveAllowedExecutable(allowDir, outDir.mPath.string(), resolved));

    CHECK(ResolveAllowedExecutable(allowDir, okExec.string(), resolved));
    CHECK(resolved == std::filesystem::canonical(okExec));

    // Exact file allowlist permits only that file.
    std::vector<std::filesystem::path> allowFile = {okExec};

    CHECK(ResolveAllowedExecutable(allowFile, okExec.string(), resolved));
    CHECK_FALSE(ResolveAllowedExecutable(allowFile, "/bin/sh", resolved));
}

#ifdef OTNS_RFSIM_CLI_BIN
TEST_CASE("node.spawn RPC and REPL enforce allowlist and launch labeled external nodes")
{
    TempOutDir               outDir;
    Simulation               sim;
    WsServer                 server;
    JsonlRecorder            recorder;
    otns::Nodes::RfSimServer rfsimServer(sim);
    auto                     rfsimSock = outDir.mPath / "rfsim.sock";
    auto                     rpcSock   = outDir.mPath / "rpc.sock";

    CHECK(server.ListenHttp(0, "127.0.0.1"));
    CHECK(server.ListenRpcUnix(rpcSock.string()));
    CHECK(rfsimServer.ListenUnix(rfsimSock.string()));

    VizPublisher viz(sim, server, &recorder);
    RpcHandler   rpc(sim, server, viz, recorder, outDir.mPath, &rfsimServer);
    Repl         repl(sim, viz, rpc, recorder, outDir.mPath);

    server.SetOnMessage([&](ClientId aId, const std::string &aMsg) { rpc.HandleMessage(aId, aMsg); });

    // 1. REPL rejects spawn before --allow-spawn is configured.
    std::ostringstream oss;

    CHECK(repl.ExecuteLine(std::string("spawn ") + OTNS_RFSIM_CLI_BIN, oss));
    CHECK(oss.str().find("Error:") != std::string::npos);

    // 2. Allowlist the ot-rfsim executable directory and verify REPL + RPC spawning & denial.
    std::filesystem::path cliBin(OTNS_RFSIM_CLI_BIN);

    rpc.AddAllowedSpawnPath(cliBin.parent_path());

    oss.str("");
    CHECK(repl.ExecuteLine("spawn /bin/sh", oss));
    CHECK(oss.str().find("Error:") != std::string::npos);

    oss.str("");
    CHECK(repl.ExecuteLine(std::string("spawn ") + OTNS_RFSIM_CLI_BIN + " matter matter-repl 10 20", oss));
    CHECK(oss.str().find("Done") != std::string::npos);

    auto replSnap = sim.GetNodeSnapshot(1);

    CHECK(replSnap.has_value());
    if (replSnap.has_value())
    {
        CHECK(replSnap->mKind == NodeKind::kExternal);
        CHECK(replSnap->mLabel == "matter-repl");
    }

    // 3. Verify JSON-RPC node.spawn rejection (/bin/sh) and success (matter-rpc) over Unix RPC socket.
    bool rpcOk      = false;
    bool sawForbid  = false;
    bool sawSpawned = false;

    std::thread rpcClient([&](void) {
        int                fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr;

        if (fd < 0)
        {
            return;
        }
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, rpcSock.c_str(), rpcSock.string().size() + 1);
        if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            ::close(fd);
            return;
        }

        auto sendLine = [&](const std::string &aLine) {
            std::string wire = aLine + "\n";

            (void)::send(fd, wire.data(), wire.size(), 0);
        };

        std::string buf;
        auto        recvLine = [&](std::string &aOut) -> bool {
            while (buf.find('\n') == std::string::npos)
            {
                char    tmp[1024];
                ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);

                if (n <= 0)
                {
                    return false;
                }
                buf.append(tmp, static_cast<size_t>(n));
            }
            size_t nl = buf.find('\n');

            aOut = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            return true;
        };

        sendLine(R"({"v":1,"type":"rpc.req","id":1,"method":"node.spawn","params":{"executable":"/bin/sh"}})");
        std::string line;

        if (recvLine(line))
        {
            auto j = nlohmann::json::parse(line, nullptr, false);

            if (j.contains("error") && j["error"]["code"] == "FORBIDDEN_EXECUTABLE")
            {
                sawForbid = true;
            }
        }

        nlohmann::json spawnReq = {
            {"v", 1},
            {"type", "rpc.req"},
            {"id", 2},
            {"method", "node.spawn"},
            {"params",
             {
                 {"executable", OTNS_RFSIM_CLI_BIN},
                 {"arg_style", "matter"},
                 {"label", "matter-rpc"},
                 {"x", 30.0},
                 {"y", 40.0},
             }},
        };

        sendLine(spawnReq.dump());
        if (recvLine(line))
        {
            auto j = nlohmann::json::parse(line, nullptr, false);

            if (j.contains("result") && j["result"]["kind"] == "external" && j["result"]["label"] == "matter-rpc")
            {
                sawSpawned = true;
            }
        }

        rpcOk = sawForbid && sawSpawned;
        ::close(fd);
    });

    for (int i = 0; i < 120 && !rpcOk; ++i)
    {
        server.PollOnce(25);
    }
    rpcClient.join();

    CHECK(sawForbid);
    CHECK(sawSpawned);
    CHECK(sim.GetStats().mTotal == 2);
}
#endif
