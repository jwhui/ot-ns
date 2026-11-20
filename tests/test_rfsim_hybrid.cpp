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
#include <sstream>
#include <string>
#include <thread>

#include "core/simulation.hpp"
#include "doctest/doctest.h"
#include "nodes/rfsim_server.hpp"

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
