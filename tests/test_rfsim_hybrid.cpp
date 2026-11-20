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
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/net_fabric.hpp"
#include "core/simulation.hpp"
#include "doctest/doctest.h"
#include "nodes/rfsim_server.hpp"
#include "protocol/rfsim_v3_codec.hpp"

using namespace otns::Core;
using namespace otns::Nodes;
using namespace otns::Rf;

namespace {

std::string FirstNonEmptyLine(const std::string &aText)
{
    std::istringstream iss(aText);
    std::string        line;

    while (std::getline(iss, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        {
            line.pop_back();
        }
        if (!line.empty() && line != "Done")
        {
            return line;
        }
    }

    return "";
}

void RunHybridFormationAndPingTest(bool aUseTcp)
{
    SimulationConfig cfg;

    cfg.mSeed     = aUseTcp ? 9101 : 9100;
    cfg.mRfPreset = RadioModelPreset::kMutualInterference;

    Simulation  sim(cfg);
    RfSimServer server(sim);

    if (aUseTcp)
    {
        CHECK(server.ListenTcp(0, "127.0.0.1"));
        CHECK(server.GetTcpPort() > 0);
    }
    else
    {
        std::string sockPath = "/tmp/otns_v3_hybrid_" + std::to_string(static_cast<long>(::getpid())) + ".sock";

        CHECK(server.ListenUnix(sockPath));
    }

    // 1. In-process OpenThread Leader forms the Thread 1.4 network first.
    NodeId leaderId = sim.AddNode({NodeType::kRouter, {0.0, 0.0, 0.0}, 160.0, true});

    sim.AdvanceSec(10.0);

    auto leaderSnap = sim.GetNodeSnapshot(leaderId);

    CHECK(leaderSnap.has_value());
    if (leaderSnap.has_value())
    {
        CHECK(leaderSnap->mRole == DeviceRole::kLeader);
    }

    // 2. Spawn a real external OS process (ot-cli-ftd-rfsim) connecting over ot-rfsim v3 socket.
    NodeId extId = server.SpawnExternalNode(OTNS_RFSIM_CLI_BIN, {NodeType::kMed, {45.0, 0.0, 0.0}, 160.0, true});

    CHECK(extId != kInvalidNodeId);
    if (extId == kInvalidNodeId)
    {
        return;
    }

    // 3. Advance hybrid simulation so the external process attaches to the in-process Leader over RF.
    sim.AdvanceSec(15.0);

    SimulationStats st = sim.GetStats();

    CHECK(st.mTotal == 2);
    CHECK(st.mLeaders == 1);
    CHECK(st.mChildren == 1);
    CHECK(st.mPartitions == 1);
    CHECK(st.mDetached == 0);

    auto extSnap = sim.GetNodeSnapshot(extId);

    CHECK(extSnap.has_value());
    if (extSnap.has_value())
    {
        CHECK(extSnap->mKind == NodeKind::kExternal);
        CHECK(extSnap->mRole == DeviceRole::kChild);
        CHECK_FALSE(extSnap->mExtAddr.empty());
    }

    // 4. Query ML-EID from the external node via ot-rfsim v3 CLI and ping it from the in-process Leader!
    std::string extMleid = FirstNonEmptyLine(sim.ExecuteCli(extId, "ipaddr mleid"));

    CHECK_FALSE(extMleid.empty());
    if (!extMleid.empty())
    {
        (void)sim.ExecuteCli(leaderId, "ping " + extMleid);
        sim.AdvanceSec(2.0);

        std::string pingOut = sim.TakeCliOutput(leaderId);

        CHECK(pingOut.find("1 packets transmitted, 1 packets received") != std::string::npos);
    }

    // 5. Reverse direction: ping the in-process Leader from the external OS process!
    std::string leaderMleid = FirstNonEmptyLine(sim.ExecuteCli(leaderId, "ipaddr mleid"));

    CHECK_FALSE(leaderMleid.empty());
    if (!leaderMleid.empty())
    {
        (void)sim.ExecuteCli(extId, "ping " + leaderMleid);
        sim.AdvanceSec(2.0);

        std::string extPingOut = sim.TakeCliOutput(extId);

        CHECK(extPingOut.find("1 packets transmitted, 1 packets received") != std::string::npos);
    }

    // Cleanly disconnect and reap the child process.
    CHECK(sim.RemoveNode(extId));
    CHECK(sim.GetStats().mTotal == 1);
}

} // namespace

TEST_CASE("Hybrid in-process + external ot-rfsim v3 process over Unix domain socket")
{
    RunHybridFormationAndPingTest(false);
}

TEST_CASE("Hybrid in-process + external ot-rfsim v3 process over TCP loopback") { RunHybridFormationAndPingTest(true); }

TEST_CASE("RfSimServer rejects malformed/oversized handshake frames cleanly")
{
    Simulation  sim;
    RfSimServer server(sim);

    CHECK(server.ListenTcp(0, "127.0.0.1"));

    uint16_t    port = server.GetTcpPort();
    std::thread badPeer([port](void) {
        int                fd = ::socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;

        if (fd < 0)
        {
            return;
        }
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        (void)::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0)
        {
            // Send an oversized payload_len header (0xFFFFFFFF) to test server bounds rejection.
            uint8_t badHdr[16] = {0xFF, 0xFF, 0xFF, 0xFF};

            (void)::send(fd, badHdr, sizeof(badHdr), 0);
        }
        ::close(fd);
    });

    NodeId id = server.AcceptExternalNode({NodeType::kRouter, {}, 160.0, false}, 2000);

    badPeer.join();
    CHECK(id == kInvalidNodeId);
    CHECK(sim.GetStats().mTotal == 0);
}

TEST_CASE("Matter --thread-arg and raw placeholder spawn modes with node labels")
{
    SimulationConfig cfg;

    cfg.mSeed     = 9200;
    cfg.mRfPreset = RadioModelPreset::kIdeal;

    Simulation  sim(cfg);
    RfSimServer server(sim);
    std::string sockPath = "/tmp/otns_v3_matter_spawn_" + std::to_string(static_cast<long>(::getpid())) + ".sock";

    CHECK(server.ListenUnix(sockPath));

    NodeId leaderId = sim.AddNode({NodeType::kRouter, {0.0, 0.0, 0.0}, 160.0, true, "leader-br"});

    sim.AdvanceSec(10.0);
    auto leaderSnap = sim.GetNodeSnapshot(leaderId);

    CHECK(leaderSnap.has_value());
    if (leaderSnap.has_value())
    {
        CHECK(leaderSnap->mRole == DeviceRole::kLeader);
        CHECK(leaderSnap->mLabel == "leader-br");
    }

    // 1. Spawn using Matter --thread-arg=<id> --thread-arg=<socket> --thread-arg=<seed> convention.
    NodeId matterId =
        server.SpawnExternalNode(OTNS_RFSIM_CLI_BIN, {NodeType::kMed, {40.0, 0.0, 0.0}, 160.0, true, "matter-light"},
                                 {}, 5000, SpawnArgStyle::kMatter);

    CHECK(matterId != kInvalidNodeId);

    // 2. Spawn using raw placeholder substitution ({socket}, {node_id}).
    NodeId rawId =
        server.SpawnExternalNode(OTNS_RFSIM_CLI_BIN, {NodeType::kMed, {-40.0, 0.0, 0.0}, 160.0, true, "matter-switch"},
                                 {"--socket", "{socket}", "--node-id", "{node_id}"}, 5000, SpawnArgStyle::kRaw);

    CHECK(rawId != kInvalidNodeId);

    sim.AdvanceSec(15.0);

    auto matterSnap = sim.GetNodeSnapshot(matterId);
    auto rawSnap    = sim.GetNodeSnapshot(rawId);

    CHECK(matterSnap.has_value());
    CHECK(rawSnap.has_value());
    if (matterSnap.has_value() && rawSnap.has_value())
    {
        CHECK(matterSnap->mKind == NodeKind::kExternal);
        CHECK(matterSnap->mLabel == "matter-light");
        CHECK(matterSnap->mRole == DeviceRole::kChild);

        CHECK(rawSnap->mKind == NodeKind::kExternal);
        CHECK(rawSnap->mLabel == "matter-switch");
        CHECK(rawSnap->mRole == DeviceRole::kChild);
    }

    CHECK(sim.GetStats().mPartitions == 1);
    CHECK(sim.GetStats().mDetached == 0);

    std::string matterMleid = FirstNonEmptyLine(sim.ExecuteCli(matterId, "ipaddr mleid"));

    CHECK_FALSE(matterMleid.empty());
    if (!matterMleid.empty())
    {
        (void)sim.ExecuteCli(leaderId, "ping " + matterMleid);
        sim.AdvanceSec(2.0);
        CHECK(sim.TakeCliOutput(leaderId).find("1 packets transmitted, 1 packets received") != std::string::npos);
    }

    CHECK(sim.RemoveNode(matterId));
    CHECK(sim.RemoveNode(rawId));
}

TEST_CASE("ot-rfsim v3 virtual backbone: TREL peer discovery/packets, Infra-If AIL, and Host UDP")
{
    using namespace otns::Protocol;

    auto sendFrameFd = [](int aFd, const Frame &aFrame) -> bool {
        std::vector<uint8_t> wire;

        if (!EncodeFrame(aFrame, wire))
        {
            return false;
        }

        size_t sent = 0;

        while (sent < wire.size())
        {
            ssize_t n = ::send(aFd, wire.data() + sent, wire.size() - sent, 0);

            if (n <= 0)
            {
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    };

    auto recvFrameFd = [](int aFd, std::vector<uint8_t> &aBuf, Frame &aOut, int aTimeoutMs) -> bool {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(aTimeoutMs);

        while (true)
        {
            if (!aBuf.empty())
            {
                DecodeResult res = DecodeFrame(aBuf.data(), aBuf.size());

                if (res.mStatus == DecodeStatus::kOk)
                {
                    aBuf.erase(aBuf.begin(), aBuf.begin() + static_cast<std::ptrdiff_t>(res.mBytesConsumed));
                    aOut = std::move(res.mFrame);
                    return true;
                }
                if (res.mStatus != DecodeStatus::kNeedMoreData)
                {
                    return false;
                }
            }

            auto now = std::chrono::steady_clock::now();

            if (now >= deadline)
            {
                return false;
            }

            struct pollfd pfd = {aFd, POLLIN, 0};

            if (::poll(&pfd, 1, 50) > 0)
            {
                uint8_t tmp[2048];
                ssize_t n = ::recv(aFd, tmp, sizeof(tmp), 0);

                if (n <= 0)
                {
                    return false;
                }
                aBuf.insert(aBuf.end(), tmp, tmp + n);
            }
        }
    };

    auto connectPeer = [](uint16_t aPort) -> int {
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
    };

    Simulation  sim;
    RfSimServer server(sim);

    CHECK(server.ListenTcp(0, "127.0.0.1"));
    uint16_t port = server.GetTcpPort();

    std::atomic<bool> peer1SawTrelPeer{false};
    std::atomic<bool> peer1SawTrelRemoved{false};
    std::atomic<bool> peer2SawTrelPeer{false};
    std::atomic<bool> peer2SawTrelPacket{false};
    std::atomic<bool> peer2SawInfraIf{false};
    std::atomic<bool> peer2SawHostUdp{false};
    std::atomic<bool> stopPeers{false};

    auto runPeerLoop = [&](int aFd, uint32_t aNodeId, const char *aLabel, uint16_t aTrelPort, bool aIsSender) {
        std::vector<uint8_t> rxBuf;
        Frame                hello;

        hello.mType = MsgType::kHello;
        hello.AddBytes(TlvType::kMagic, kProtocolMagic.data(), kProtocolMagic.size());
        hello.AddVersion(kProtocolMajor, kProtocolMinor);
        hello.AddU32(TlvType::kNodeId, aNodeId);
        hello.AddText(TlvType::kText, aLabel);
        if (!sendFrameFd(aFd, hello))
        {
            return;
        }

        Frame ack;

        if (!recvFrameFd(aFd, rxBuf, ack, 2000) || ack.mType != MsgType::kHelloAck)
        {
            return;
        }

        // Register TREL service and join Infra-If AIL during startup before initial StepDone.
        Frame        trelReg;
        NetAddrValue trelAddr;
        const char  *txt = aIsSender ? "xa=1111" : "xa=2222";

        trelAddr.mSrcPort = aTrelPort;
        trelReg.mType     = MsgType::kTrelPeer;
        trelReg.AddNetAddr(trelAddr);
        trelReg.AddBytes(TlvType::kPayload, reinterpret_cast<const uint8_t *>(txt), std::strlen(txt));
        (void)sendFrameFd(aFd, trelReg);

        Frame infraJoin;

        infraJoin.mType = MsgType::kInfraIfState;
        (void)sendFrameFd(aFd, infraJoin);

        if (!aIsSender)
        {
            // Bind Matter default UDP port 5540 on peer 2.
            Frame        udpBind;
            NetAddrValue bindAddr;

            bindAddr.mSrcPort = 5540;
            udpBind.mType     = MsgType::kHostSocket;
            udpBind.AddNetAddr(bindAddr);
            (void)sendFrameFd(aFd, udpBind);
        }

        Frame done;

        done.mType = MsgType::kStepDone;
        (void)sendFrameFd(aFd, done);

        // Event loop: respond to simulator frames with StepDone.
        while (!stopPeers.load())
        {
            Frame in;

            if (!recvFrameFd(aFd, rxBuf, in, 200))
            {
                continue;
            }
            if (in.mType == MsgType::kShutdown)
            {
                break;
            }
            if (in.mType == MsgType::kTrelPeer)
            {
                auto addr = in.GetNetAddr();

                if ((in.mFlags & kFlagCancel) != 0)
                {
                    if (aIsSender)
                    {
                        peer1SawTrelRemoved.store(true);
                    }
                }
                else if (addr.has_value())
                {
                    if (aIsSender)
                    {
                        peer1SawTrelPeer.store(true);

                        // Send TREL packet, Infra-If multicast RA, and Host UDP packet to peer 2!
                        Frame        trelPkt;
                        NetAddrValue dst;
                        const char  *trelBody = "trel-hello";

                        dst.mSrcPort  = aTrelPort;
                        dst.mDstPort  = addr->mSrcPort;
                        dst.mDstIp6   = addr->mSrcIp6;
                        trelPkt.mType = MsgType::kTrelPacket;
                        trelPkt.AddNetAddr(dst);
                        trelPkt.AddBytes(TlvType::kPayload, reinterpret_cast<const uint8_t *>(trelBody),
                                         std::strlen(trelBody));
                        (void)sendFrameFd(aFd, trelPkt);

                        Frame        raPkt;
                        NetAddrValue raAddr;
                        const char  *raBody = "icmp6-ra";

                        raAddr.mDstIp6[0]  = 0xFF;
                        raAddr.mDstIp6[1]  = 0x02;
                        raAddr.mDstIp6[15] = 0x01; // ff02::1
                        raPkt.mType        = MsgType::kInfraIfPacket;
                        raPkt.AddNetAddr(raAddr);
                        raPkt.AddBytes(TlvType::kPayload, reinterpret_cast<const uint8_t *>(raBody),
                                       std::strlen(raBody));
                        (void)sendFrameFd(aFd, raPkt);

                        Frame        udpPkt;
                        NetAddrValue udpAddr;
                        const char  *udpBody = "matter-mrp";

                        udpAddr.mSrcPort = 5541;
                        udpAddr.mDstPort = 5540;
                        udpAddr.mDstIp6  = addr->mSrcIp6;
                        udpPkt.mType     = MsgType::kHostSocket;
                        udpPkt.AddNetAddr(udpAddr);
                        udpPkt.AddBytes(TlvType::kPayload, reinterpret_cast<const uint8_t *>(udpBody),
                                        std::strlen(udpBody));
                        (void)sendFrameFd(aFd, udpPkt);
                    }
                    else
                    {
                        peer2SawTrelPeer.store(true);
                    }
                }
            }
            else if (in.mType == MsgType::kTrelPacket)
            {
                const Tlv *p = in.FindTlv(TlvType::kPayload);
                auto       a = in.GetNetAddr();

                if (p != nullptr && a.has_value() && std::string(p->mValue.begin(), p->mValue.end()) == "trel-hello" &&
                    a->mSrcIp6 == NetFabric::MakeNodeBackboneIp6(1))
                {
                    peer2SawTrelPacket.store(true);
                }
            }
            else if (in.mType == MsgType::kInfraIfPacket)
            {
                const Tlv *p = in.FindTlv(TlvType::kPayload);
                auto       a = in.GetNetAddr();

                if (p != nullptr && a.has_value() && std::string(p->mValue.begin(), p->mValue.end()) == "icmp6-ra" &&
                    a->mSrcIp6 == NetFabric::MakeNodeLinkLocalIp6(1))
                {
                    peer2SawInfraIf.store(true);
                }
            }
            else if (in.mType == MsgType::kHostSocket)
            {
                const Tlv *p = in.FindTlv(TlvType::kPayload);
                auto       a = in.GetNetAddr();

                if (p != nullptr && a.has_value() && std::string(p->mValue.begin(), p->mValue.end()) == "matter-mrp" &&
                    a->mDstPort == 5540)
                {
                    peer2SawHostUdp.store(true);
                }
            }

            Frame stepDone;

            stepDone.mType = MsgType::kStepDone;
            (void)sendFrameFd(aFd, stepDone);
        }
    };

    int fd1 = connectPeer(port);
    int fd2 = connectPeer(port);

    CHECK(fd1 >= 0);
    CHECK(fd2 >= 0);

    std::thread t1([&](void) { runPeerLoop(fd1, 1, "matter-br", 49153, true); });
    std::thread t2([&](void) { runPeerLoop(fd2, 2, "matter-dev", 49154, false); });

    NodeId id1 = server.AcceptExternalNode({NodeType::kRouter, {}, 160.0, false}, 2000);
    NodeId id2 = server.AcceptExternalNode({NodeType::kRouter, {}, 160.0, false}, 2000);

    CHECK(id1 == 1);
    CHECK(id2 == 2);

    // Labels should auto-populate from Hello text TLV.
    auto snap1 = sim.GetNodeSnapshot(id1);
    auto snap2 = sim.GetNodeSnapshot(id2);

    CHECK(snap1.has_value());
    CHECK(snap2.has_value());
    if (snap1.has_value() && snap2.has_value())
    {
        CHECK(snap1->mLabel == "matter-br");
        CHECK(snap2->mLabel == "matter-dev");
    }

    // Advance virtual time to deliver scheduled TREL peer updates and backbone packets.
    sim.AdvanceUs(10000);

    CHECK(peer1SawTrelPeer.load());
    CHECK(peer2SawTrelPeer.load());
    CHECK(peer2SawTrelPacket.load());
    CHECK(peer2SawInfraIf.load());
    CHECK(peer2SawHostUdp.load());

    // Removing peer 2 should notify peer 1 of TREL peer removal!
    CHECK(sim.RemoveNode(id2));
    sim.AdvanceUs(1000);
    CHECK(peer1SawTrelRemoved.load());

    stopPeers.store(true);
    CHECK(sim.RemoveNode(id1));
    ::close(fd1);
    ::close(fd2);
    t1.join();
    t2.join();
}

TEST_CASE("Matter library-mode app (otSysInit + cli_uart otAppCliInit + select loop) end-to-end")
{
    SimulationConfig cfg;

    cfg.mSeed     = 9300;
    cfg.mRfPreset = RadioModelPreset::kMutualInterference;

    Simulation  sim(cfg);
    RfSimServer server(sim);
    std::string sockPath = "/tmp/otns_v3_matter_lib_" + std::to_string(static_cast<long>(::getpid())) + ".sock";

    CHECK(server.ListenUnix(sockPath));

    NodeId leaderId = sim.AddNode({NodeType::kRouter, {0.0, 0.0, 0.0}, 160.0, true, "leader"});

    sim.AdvanceSec(10.0);

    std::string datasetHex = FirstNonEmptyLine(sim.ExecuteCli(leaderId, "dataset active -x"));

    CHECK_FALSE(datasetHex.empty());

    // Spawn mock_matter_node using SpawnArgStyle::kMatter (identical args to connectedhomeip all-devices-app).
    // Note auto_start=false so we also test driving cli_uart commands manually just like test-matter-smoke.py.
    std::vector<std::string> matterFlags = {
        "--device",   "on-off-light", "--discriminator",       "3840",
        "--passcode", "20202021",     "--secured-device-port", "5540",
    };

    NodeId matterId =
        server.SpawnExternalNode(OTNS_MOCK_MATTER_BIN, {NodeType::kMed, {40.0, 0.0, 0.0}, 160.0, false, "matter-light"},
                                 matterFlags, 5000, SpawnArgStyle::kMatter);

    CHECK(matterId != kInvalidNodeId);
    if (matterId == kInvalidNodeId)
    {
        return;
    }

    // Verify cli_uart echo is cleanly stripped and commands work without otRfSimCompleteStartup()!
    CHECK(FirstNonEmptyLine(sim.ExecuteCli(matterId, "state")) == "disabled");

    CHECK(sim.ExecuteCli(matterId, "dataset set active " + datasetHex).find("Done") != std::string::npos);
    CHECK(sim.ExecuteCli(matterId, "mode rn").find("Done") != std::string::npos);
    CHECK(sim.ExecuteCli(matterId, "ifconfig up").find("Done") != std::string::npos);
    CHECK(sim.ExecuteCli(matterId, "thread start").find("Done") != std::string::npos);

    sim.AdvanceSec(15.0);

    CHECK(FirstNonEmptyLine(sim.ExecuteCli(matterId, "state")) == "child");

    auto matterSnap = sim.GetNodeSnapshot(matterId);

    CHECK(matterSnap.has_value());
    if (matterSnap.has_value())
    {
        CHECK(matterSnap->mKind == NodeKind::kExternal);
        CHECK(matterSnap->mRole == DeviceRole::kChild);
        CHECK(matterSnap->mLabel == "matter-light");
    }

    CHECK(sim.GetStats().mTotal == 2);
    CHECK(sim.GetStats().mLeaders == 1);
    CHECK(sim.GetStats().mChildren == 1);
    CHECK(sim.GetStats().mPartitions == 1);

    // Bidirectional IPv6 ping over simulated 802.15.4 RF between Leader and Matter library-mode node!
    std::string matterMleid = FirstNonEmptyLine(sim.ExecuteCli(matterId, "ipaddr mleid"));
    std::string leaderMleid = FirstNonEmptyLine(sim.ExecuteCli(leaderId, "ipaddr mleid"));

    CHECK_FALSE(matterMleid.empty());
    CHECK_FALSE(leaderMleid.empty());

    if (!matterMleid.empty() && !leaderMleid.empty())
    {
        (void)sim.ExecuteCli(leaderId, "ping " + matterMleid);
        sim.AdvanceSec(2.0);
        CHECK(sim.TakeCliOutput(leaderId).find("1 packets transmitted, 1 packets received") != std::string::npos);

        (void)sim.ExecuteCli(matterId, "ping " + leaderMleid);
        sim.AdvanceSec(2.0);
        CHECK(sim.TakeCliOutput(matterId).find("1 packets transmitted, 1 packets received") != std::string::npos);
    }

    CHECK(sim.RemoveNode(matterId));
    CHECK(sim.GetStats().mTotal == 1);
}
