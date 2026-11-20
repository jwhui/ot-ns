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

#include <algorithm>
#include <cmath>
#include <vector>

#include "doctest/doctest.h"
#include "rf/ber_model.hpp"
#include "rf/pathloss_fading.hpp"
#include "rf/prng.hpp"
#include "rf/radio_model.hpp"
#include "rf/spatial_index.hpp"

using namespace otns::Rf;

TEST_CASE("Prng determinism and bounds")
{
    Prng a(42);
    Prng b(42);
    Prng c(43);
    bool diffFound = false;

    for (int i = 0; i < 100; ++i)
    {
        uint64_t va = a.NextU64();
        uint64_t vb = b.NextU64();
        uint64_t vc = c.NextU64();

        CHECK(va == vb);
        if (va != vc)
        {
            diffFound = true;
        }
    }
    CHECK(diffFound);

    for (int i = 0; i < 200; ++i)
    {
        double u = a.NextUnitDouble();
        double e = a.NextExponential();

        CHECK(u >= 0.0);
        CHECK(u < 1.0);
        CHECK(std::isfinite(a.NextGaussian()));
        CHECK(e >= 0.0);
        CHECK(std::isfinite(e));
    }
}

TEST_CASE("SpatialIndex matches brute-force scan across 500 nodes")
{
    SpatialIndex                               idx(100.0);
    Prng                                       rng(12345);
    std::vector<std::pair<NodeId, Position3D>> pts;

    for (NodeId id = 1; id <= 500; ++id)
    {
        Position3D p{rng.NextUnitDouble() * 2000.0 - 1000.0, rng.NextUnitDouble() * 2000.0 - 1000.0,
                     rng.NextUnitDouble() * 100.0};

        pts.emplace_back(id, p);
        idx.Upsert(id, p);
    }
    CHECK(idx.GetSize() == 500);

    // Move a node and remove another.
    pts[0].second = {500.0, 500.0, 10.0};
    idx.Upsert(1, pts[0].second);
    idx.Remove(250);
    pts.erase(std::remove_if(pts.begin(), pts.end(), [](const auto &aKv) { return aKv.first == 250; }), pts.end());
    CHECK(idx.GetSize() == 499);

    Position3D          center{120.0, -80.0, 25.0};
    double              radius = 240.0;
    std::vector<NodeId> expected;

    for (const auto &entry : pts)
    {
        if (center.DistanceTo(entry.second) <= radius)
        {
            expected.push_back(entry.first);
        }
    }
    std::sort(expected.begin(), expected.end());

    auto actual = idx.QueryRadius(center, radius);

    CHECK(actual == expected);
}

TEST_CASE("BER model behavior matches IEEE 802.15.4 Annex E.4.1.8")
{
    CHECK(ComputePacketSuccessRate(6.0, 50) == doctest::Approx(1.0));
    CHECK(ComputePacketSuccessRate(10.0, 127) == doctest::Approx(1.0));
    CHECK(ComputePacketSuccessRate(-10.1, 5) == doctest::Approx(0.0));

    double pLow  = ComputePacketSuccessRate(-4.0, 50);
    double pMid  = ComputePacketSuccessRate(0.0, 50);
    double pHigh = ComputePacketSuccessRate(4.0, 50);

    CHECK(pLow < pMid);
    CHECK(pMid < pHigh);

    // Longer frames must have lower or equal success rate at the same SIR.
    double pShort = ComputePacketSuccessRate(-1.0, 5);
    double pLong  = ComputePacketSuccessRate(-1.0, 120);

    CHECK(pShort > pLong);
}

TEST_CASE("Pathloss and symmetric fading models")
{
    auto        params = RadioModelParams::FromPreset(RadioModelPreset::kMutualInterference);
    DbValue     rNear  = ComputeIndoorRssi3gpp(20.0, 0.0, params);
    DbValue     rFar   = ComputeIndoorRssi3gpp(200.0, 0.0, params);
    FadingModel fading(777);
    Position3D  a{100.0, 200.0, 0.0};
    Position3D  b{350.0, 400.0, 0.0};
    DbValue     fAb = fading.ComputeFadingDb(a, b, params);
    DbValue     fBa = fading.ComputeFadingDb(b, a, params);

    CHECK(rNear > rFar);
    CHECK(fAb == doctest::Approx(fBa));
}

TEST_CASE("RadioModel Ideal disc reachability and RSSI")
{
    RadioModel     rm(RadioModelPreset::kIdeal, 1);
    RadioNodeState n1;

    n1.mId         = 1;
    n1.mPos        = {0.0, 0.0, 0.0};
    n1.mRadioRange = 100.0;
    n1.mChannel    = 11;
    n1.mState      = RadioState::kRx;

    RadioNodeState n2 = n1;

    n2.mId  = 2;
    n2.mPos = {80.0, 0.0, 0.0};

    RadioNodeState n3 = n1;

    n3.mId  = 3;
    n3.mPos = {150.0, 0.0, 0.0};

    rm.AddNode(n1);
    rm.AddNode(n2);
    rm.AddNode(n3);

    CHECK(rm.IsReachable(1, 2));
    CHECK_FALSE(rm.IsReachable(1, 3));
    CHECK(rm.GetTxRssi(1, 2) == doctest::Approx(-60.0));

    auto txStart = rm.BeginTx(1, 11, 0.0, 40, 1500);

    CHECK_FALSE(txStart.mAborted);
    CHECK(txStart.mReceivers.size() == 1);
    if (txStart.mReceivers.size() == 1)
    {
        CHECK(txStart.mReceivers[0].mNodeId == 2);
    }

    auto txEnd = rm.EndTx(1, 11, 40);

    CHECK(txEnd.mDeliveries.size() == 1);
    if (txEnd.mDeliveries.size() == 1)
    {
        CHECK(txEnd.mDeliveries[0].mNodeId == 2);
        CHECK(txEnd.mDeliveries[0].mStatus == RxResultStatus::kOk);
    }
}

TEST_CASE("RadioModel MutualInterference collisions, CCA energy scan, and determinism")
{
    auto runScenario = [](uint64_t aSeed) {
        RadioModelParams params = RadioModelParams::FromPreset(RadioModelPreset::kMutualInterference);

        params.mShadowFadingSigmaDb  = 0.0;
        params.mTimeFadingSigmaMaxDb = 0.0;
        params.mMeanTimeFadingChange = 0.0;

        RadioModel rm(RadioModelPreset::kMutualInterference, aSeed);

        rm.SetParams(params);

        RadioNodeState tx1;

        tx1.mId      = 1;
        tx1.mPos     = {0.0, 0.0, 0.0};
        tx1.mChannel = 15;
        tx1.mState   = RadioState::kRx;

        RadioNodeState rx;

        rx.mId      = 2;
        rx.mPos     = {50.0, 0.0, 0.0};
        rx.mChannel = 15;
        rx.mState   = RadioState::kRx;

        RadioNodeState interferer;

        interferer.mId      = 3;
        interferer.mPos     = {60.0, 0.0, 0.0}; // Closer to rx than tx1 -> strong interference
        interferer.mChannel = 15;
        interferer.mState   = RadioState::kRx;

        RadioNodeState scanner;

        scanner.mId      = 4;
        scanner.mPos     = {25.0, 0.0, 0.0};
        scanner.mChannel = 15;
        scanner.mState   = RadioState::kRx;

        rm.AddNode(tx1);
        rm.AddNode(rx);
        rm.AddNode(interferer);
        rm.AddNode(scanner);

        CHECK(rm.BeginEnergyScan(4, 15));

        int8_t idleScan = rm.EndEnergyScan(4, 15);

        CHECK(idleScan == -95);
        CHECK(rm.BeginEnergyScan(4, 15));

        auto s1 = rm.BeginTx(1, 15, 0.0, 80, 3000);
        auto s3 = rm.BeginTx(3, 15, 0.0, 80, 3000);

        CHECK_FALSE(s1.mAborted);
        CHECK_FALSE(s3.mAborted);

        int8_t busyScan = rm.EndEnergyScan(4, 15);

        CHECK(busyScan > idleScan + 20);

        auto e1 = rm.EndTx(1, 15, 80);
        auto e3 = rm.EndTx(3, 15, 80);

        return std::make_pair(e1, e3);
    };

    auto resA = runScenario(999);
    auto resB = runScenario(999);

    CHECK(resA.first.mDeliveries.size() == resB.first.mDeliveries.size());
    if (resA.first.mDeliveries.size() == resB.first.mDeliveries.size())
    {
        for (size_t i = 0; i < resA.first.mDeliveries.size(); ++i)
        {
            CHECK(resA.first.mDeliveries[i].mNodeId == resB.first.mDeliveries[i].mNodeId);
            CHECK(resA.first.mDeliveries[i].mStatus == resB.first.mDeliveries[i].mStatus);
            CHECK(resA.first.mDeliveries[i].mRssiDbm == resB.first.mDeliveries[i].mRssiDbm);
        }
    }

    // Node 2 (rx) receives tx1 with interferer 3 closer -> SIR < 0 dB -> FCS error.
    auto itRx = std::find_if(resA.first.mDeliveries.begin(), resA.first.mDeliveries.end(),
                             [](const RxOutcome &aOut) { return aOut.mNodeId == 2; });

    CHECK(itRx != resA.first.mDeliveries.end());
    if (itRx != resA.first.mDeliveries.end())
    {
        CHECK(itRx->mSirDb < 0.0);
        CHECK(itRx->mStatus == RxResultStatus::kFcsError);
    }
}
