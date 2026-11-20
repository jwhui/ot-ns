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

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/kpi_tracker.hpp"
#include "core/net_fabric.hpp"
#include "core/path_security.hpp"
#include "core/pcap_writer.hpp"
#include "core/simulation.hpp"
#include "core/topology.hpp"
#include "core/wpan_dissector.hpp"
#include "doctest/doctest.h"

using namespace otns::Core;
using namespace otns::Rf;

namespace {

struct TempDir
{
    explicit TempDir(const char *aTag)
    {
        std::error_code ec;

        mPath = std::filesystem::temp_directory_path(ec) /
                ("otns_v3_test_" + std::string(aTag) + "_" + std::to_string(static_cast<long>(::getpid())));
        std::filesystem::remove_all(mPath, ec);
        std::filesystem::create_directories(mPath, ec);
    }

    ~TempDir(void)
    {
        std::error_code ec;

        std::filesystem::remove_all(mPath, ec);
    }

    std::filesystem::path mPath;
};

uint32_t ReadU32Le(const uint8_t *aPtr)
{
    return static_cast<uint32_t>(aPtr[0]) | (static_cast<uint32_t>(aPtr[1]) << 8u) |
           (static_cast<uint32_t>(aPtr[2]) << 16u) | (static_cast<uint32_t>(aPtr[3]) << 24u);
}

class MockNetNode final : public SimNode
{
public:
    explicit MockNetNode(NodeId aId)
        : mId(aId)
    {
    }

    NodeId       GetId(void) const override { return mId; }
    NodeKind     GetKind(void) const override { return NodeKind::kExternal; }
    Position3D   GetPosition(void) const override { return mPos; }
    void         SetPosition(Position3D aPos) override { mPos = aPos; }
    bool         IsEnabled(void) const override { return true; }
    void         SetEnabled(bool aEnabled) override { (void)aEnabled; }
    bool         HasPendingTasklets(void) const override { return false; }
    void         ProcessTasklets(void) override {}
    std::string  ExecuteCli(const std::string &aCommand) override { return aCommand; }
    std::string  TakeCliOutput(void) override { return ""; }
    NodeSnapshot GetSnapshot(void) const override
    {
        NodeSnapshot s;

        s.mId = mId;
        return s;
    }
    bool OnRadioRxDone(const RawFrame &aRxFrame, RawFrame &aAckOut) override
    {
        (void)aRxFrame;
        (void)aAckOut;
        return false;
    }

    void OnTrelPacket(const NetPacket &aPacket) override { mTrelRx.push_back(aPacket); }
    void OnInfraIfPacket(const NetPacket &aPacket) override { mInfraRx.push_back(aPacket); }
    void OnHostUdpPacket(const NetPacket &aPacket) override { mUdpRx.push_back(aPacket); }

    NodeId                 mId;
    Position3D             mPos;
    std::vector<NetPacket> mTrelRx;
    std::vector<NetPacket> mInfraRx;
    std::vector<NetPacket> mUdpRx;
};

} // namespace

TEST_CASE("ResolveContainedPath blocks traversal, absolute escapes, and symlink escapes")
{
    TempDir               tmp("pathsec");
    std::filesystem::path baseDir    = tmp.mPath / "out";
    std::filesystem::path outsideDir = tmp.mPath / "outside";
    std::error_code       ec;

    std::filesystem::create_directories(baseDir, ec);
    std::filesystem::create_directories(outsideDir, ec);
    std::filesystem::create_directory_symlink(outsideDir, baseDir / "escape_link", ec);

    std::filesystem::path resolved;

    CHECK(ResolveContainedPath(baseDir, "capture.pcap", resolved));
    CHECK(ResolveContainedPath(baseDir, "sub/dir/kpi.json", resolved));

    CHECK_FALSE(ResolveContainedPath(baseDir, "", resolved));
    CHECK_FALSE(ResolveContainedPath(baseDir, "../outside/evil.pcap", resolved));
    CHECK_FALSE(ResolveContainedPath(baseDir, "sub/../../outside/evil.pcap", resolved));
    CHECK_FALSE(ResolveContainedPath(baseDir, (outsideDir / "evil.pcap").string(), resolved));
    CHECK_FALSE(ResolveContainedPath(baseDir, "escape_link/evil.pcap", resolved));

    std::string withNul("good.pcap\0evil", 14);

    CHECK_FALSE(ResolveContainedPath(baseDir, withNul, resolved));
}

TEST_CASE("WpanDissector parses 802.15.4 frames safely and rejects truncated inputs")
{
    // Imm-Ack frame (FC=0x0002, Seq=0x42, FCS=0x1234).
    const uint8_t ackPsdu[] = {0x02, 0x00, 0x42, 0x34, 0x12};
    MacFrameInfo  ackInfo   = DissectWpanFrame(ackPsdu, sizeof(ackPsdu), 15);

    CHECK(ackInfo.mValid);
    CHECK(ackInfo.mFrameType == MacFrameType::kAck);
    CHECK(ackInfo.mSeq == 0x42);
    CHECK(ackInfo.ToSummaryString().find("ACK,FC:0x0002,Seq:66") != std::string::npos);

    // 2006 Data frame with PAN ID compression, short dst=0xFFFF, short src=0x1234, ackReq=true.
    // FC = Data(1) | AckReq(0x20) | PanIdComp(0x40) | DstShort(0x0800) | Ver1(0x1000) | SrcShort(0x8000) = 0x9861.
    const uint8_t dataPsdu[] = {
        0x61, 0x98, // FC
        0x07,       // Seq
        0xCE, 0xFA, // Dst PAN ID = 0xFACE
        0xFF, 0xFF, // Dst Short = 0xFFFF (broadcast)
        0x34, 0x12, // Src Short = 0x1234
        0xAA, 0xBB, // Payload
        0x00, 0x00  // FCS
    };
    MacFrameInfo dataInfo = DissectWpanFrame(dataPsdu, sizeof(dataPsdu), 11);

    CHECK(dataInfo.mValid);
    CHECK(dataInfo.mFrameType == MacFrameType::kData);
    CHECK(dataInfo.mAckRequested);
    CHECK(dataInfo.mHasDstPanId);
    CHECK_FALSE(dataInfo.mHasSrcPanId);
    CHECK(dataInfo.mDstPanId == 0xFACE);
    CHECK(dataInfo.mDstAddrShort == 0xFFFF);
    CHECK(dataInfo.mSrcAddrShort == 0x1234);
    CHECK(dataInfo.IsBroadcast());

    // Truncated frames must fail cleanly without out-of-bounds reads.
    CHECK_FALSE(DissectWpanFrame(nullptr, 10, 11).mValid);
    CHECK_FALSE(DissectWpanFrame(dataPsdu, 3, 11).mValid);
    CHECK_FALSE(DissectWpanFrame(dataPsdu, 7, 11).mValid);
}

TEST_CASE("PCAP/WPAN-TAP capture, KPI JSON export, and JSON topology round-trip")
{
    TempDir          tmp("obs");
    SimulationConfig cfg;

    cfg.mSeed     = 3030;
    cfg.mRfPreset = RadioModelPreset::kMutualInterference;

    Simulation sim(cfg);

    CHECK(sim.StartPcapContained(tmp.mPath, "captures/net.pcap", PcapFormat::kWpanTap));
    CHECK_FALSE(sim.StartPcapContained(tmp.mPath, "../escape.pcap", PcapFormat::kWpanTap));

    (void)sim.AddNode({NodeType::kRouter, {0.0, 0.0, 0.0}, 160.0, true});
    sim.AdvanceSec(10.0);
    (void)sim.AddNode({NodeType::kMed, {40.0, 0.0, 0.0}, 160.0, true});
    sim.AdvanceSec(12.0);
    sim.StopPcap();

    // Verify WPAN-TAP PCAP header (magic + DLT 283) and non-zero captured frames.
    std::filesystem::path pcapPath = tmp.mPath / "captures/net.pcap";
    std::FILE            *fp       = std::fopen(pcapPath.c_str(), "rb");

    CHECK(fp != nullptr);
    if (fp != nullptr)
    {
        uint8_t hdr[24];
        size_t  n = std::fread(hdr, 1, sizeof(hdr), fp);

        std::fclose(fp);
        CHECK(n == 24);
        CHECK(ReadU32Le(hdr + 0) == 0xa1b2c3d4u);
        CHECK(ReadU32Le(hdr + 20) == 283u);
    }
    CHECK(sim.GetPcapWriter().GetFrameCount() > 0);

    // Verify KPI tracker JSON and contained file save.
    nlohmann::json kpiJson = sim.GetKpiTracker().ToJson(sim.GetNowUs());

    CHECK(kpiJson["version"] == 1);
    CHECK(kpiJson["total"]["tx_frames"].get<uint64_t>() > 0);
    CHECK(kpiJson["total"]["rx_ok"].get<uint64_t>() > 0);
    CHECK(sim.GetKpiTracker().SaveJsonContained(tmp.mPath, "kpi/report.json", sim.GetNowUs()));
    CHECK_FALSE(sim.GetKpiTracker().SaveJsonContained(tmp.mPath, "../bad.json", sim.GetNowUs()));

    // Verify JSON topology export -> save -> load -> rebuild simulation.
    TopologySpec exported = sim.ExportTopology();

    CHECK(exported.mNodes.size() == 2);
    CHECK(SaveTopologyFileContained(tmp.mPath, "topo/sample.json", exported));

    TopologySpec loaded;
    std::string  err;

    CHECK(LoadTopologyFileContained(tmp.mPath, "topo/sample.json", loaded, err));
    CHECK(err.empty());
    CHECK(loaded.mSeed == cfg.mSeed);
    CHECK(loaded.mNodes.size() == 2);

    Simulation sim2;

    CHECK(sim2.LoadTopology(loaded));
    sim2.AdvanceSec(18.0);

    SimulationStats st2 = sim2.GetStats();

    CHECK(st2.mTotal == 2);
    CHECK(st2.mLeaders == 1);
    CHECK(st2.mPartitions == 1);
    CHECK(st2.mDetached == 0);
}

TEST_CASE("Virtual NetFabric routes TREL, Infra-If, and Host UDP packets and pacing respects speed")
{
    Simulation   sim;
    auto         n1  = std::make_unique<MockNetNode>(1);
    auto         n2  = std::make_unique<MockNetNode>(2);
    MockNetNode *p1  = n1.get();
    MockNetNode *p2  = n2.get();
    NodeConfig   cfg = {NodeType::kRouter, {0.0, 0.0, 0.0}, 160.0, false};

    CHECK(sim.RegisterExternalNode(std::move(n1), cfg) == 1);
    CHECK(sim.RegisterExternalNode(std::move(n2), cfg) == 2);

    int peerCallbacks = 0;

    sim.GetNetFabric().RegisterTrelPeer(1, 5001, {'a', 'b'},
                                        [&](NodeId, const TrelPeerRecord &, bool) { peerCallbacks += 1; });
    sim.GetNetFabric().RegisterTrelPeer(2, 5002, {'c', 'd'},
                                        [&](NodeId, const TrelPeerRecord &, bool) { peerCallbacks += 1; });
    CHECK(peerCallbacks == 2); // Mutual discovery callbacks

    NetPacket trelPkt;

    trelPkt.mSrcNodeId = 1;
    trelPkt.mSrcIp6    = NetFabric::MakeNodeBackboneIp6(1);
    trelPkt.mDstIp6    = NetFabric::MakeNodeBackboneIp6(2);
    trelPkt.mSrcPort   = 5001;
    trelPkt.mDstPort   = 5002;
    trelPkt.mPayload   = {0xDE, 0xAD, 0xBE, 0xEF};
    sim.SendTrelPacket(trelPkt);

    sim.GetNetFabric().JoinInfraIf(1);
    sim.GetNetFabric().JoinInfraIf(2);

    NetPacket infraPkt;

    infraPkt.mSrcNodeId = 1;
    infraPkt.mSrcIp6    = NetFabric::MakeNodeLinkLocalIp6(1);
    infraPkt.mDstIp6    = {0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02}; // ff02::2 all-routers
    infraPkt.mPayload   = {0x85, 0x00};                                              // ICMPv6 RS
    sim.SendInfraIfPacket(infraPkt);

    sim.GetNetFabric().BindHostUdp(2, 61631);

    NetPacket udpPkt;

    udpPkt.mSrcNodeId = 1;
    udpPkt.mSrcIp6    = NetFabric::MakeNodeBackboneIp6(1);
    udpPkt.mDstIp6    = NetFabric::MakeNodeBackboneIp6(2);
    udpPkt.mSrcPort   = 12345;
    udpPkt.mDstPort   = 61631;
    udpPkt.mPayload   = {'h', 'i'};
    sim.SendHostUdpPacket(udpPkt);

    sim.AdvanceUs(1000);

    CHECK(p1->mTrelRx.empty());
    CHECK(p2->mTrelRx.size() == 1);
    CHECK(p2->mInfraRx.size() == 1);
    CHECK(p2->mUdpRx.size() == 1);

    // Real-time pacing sanity check: 20 ms at speed=1.0 should take >= 15 ms wall time.
    auto t0 = std::chrono::steady_clock::now();

    sim.AdvancePacedUs(20000, 1.0);

    auto wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    CHECK(wallMs >= 15);
}
