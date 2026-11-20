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

#include <sstream>
#include <string>
#include <vector>

#include "core/scheduler.hpp"
#include "core/simulation.hpp"
#include "doctest/doctest.h"

using namespace otns::Core;
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

} // namespace

TEST_CASE("Scheduler deterministic ordering and cancellation")
{
    Scheduler        sched;
    std::vector<int> fired;

    sched.ScheduleAt(200, [&](void) { fired.push_back(3); });

    EventId cancelled = sched.ScheduleAt(150, [&](void) { fired.push_back(99); });

    sched.ScheduleAt(100, [&](void) { fired.push_back(1); });
    sched.ScheduleAt(100, [&](void) { fired.push_back(2); });
    sched.Cancel(cancelled);

    while (sched.DispatchNextIfBeforeOrAt(500))
    {
    }

    CHECK(fired == std::vector<int>{1, 2, 3});
    CHECK(sched.GetNowUs() == 200);
}

TEST_CASE("In-process multi-node Thread network formation, CLI, and IPv6 ping")
{
    SimulationConfig cfg;

    cfg.mSeed     = 2026;
    cfg.mRfPreset = RadioModelPreset::kMutualInterference;

    Simulation sim(cfg);
    NodeId     n1 = sim.AddNode({NodeType::kRouter, {0.0, 0.0, 0.0}, 160.0, true});

    sim.AdvanceSec(10.0); // Allow leader to form before attaching router, MED, and SED

    NodeId n2 = sim.AddNode({NodeType::kRouter, {50.0, 0.0, 0.0}, 160.0, true});
    NodeId n3 = sim.AddNode({NodeType::kMed, {100.0, 0.0, 0.0}, 160.0, true});
    NodeId n4 = sim.AddNode({NodeType::kSed, {25.0, 40.0, 0.0}, 160.0, true});

    (void)sim.ExecuteCli(n4, "pollperiod 1000");

    sim.AdvanceSec(20.0);

    SimulationStats st = sim.GetStats();

    CHECK(st.mTotal == 4);
    CHECK(st.mLeaders == 1);
    CHECK(st.mPartitions == 1);
    CHECK(st.mDetached == 0);
    CHECK(st.mDisabled == 0);
    CHECK(st.mTxFrames > 0);
    CHECK(st.mRxOk > 0);

    // Query node 2's ML-EID via OpenThread CLI and ping it from node 1.
    std::string mleidOut = sim.ExecuteCli(n2, "ipaddr mleid");
    std::string n2Mleid  = FirstNonEmptyLine(mleidOut);

    CHECK_FALSE(n2Mleid.empty());
    if (!n2Mleid.empty())
    {
        (void)sim.ExecuteCli(n1, "ping " + n2Mleid);
        sim.AdvanceSec(2.0);

        std::string pingOut = sim.TakeCliOutput(n1);

        CHECK(pingOut.find("1 packets transmitted, 1 packets received") != std::string::npos);
    }

    // Verify node 3 (MED) and node 4 (SED) state via CLI and clean removal of node 4.
    CHECK(sim.ExecuteCli(n3, "state").find("child") != std::string::npos);
    CHECK(sim.ExecuteCli(n4, "state").find("child") != std::string::npos);
    CHECK(sim.RemoveNode(n4));
    sim.AdvanceSec(2.0);
    CHECK(sim.GetStats().mTotal == 3);
}

TEST_CASE("Deterministic reproducibility across identical seeded simulations")
{
    auto runOnce = [](void) {
        SimulationConfig cfg;

        cfg.mSeed     = 7777;
        cfg.mRfPreset = RadioModelPreset::kMutualInterference;

        Simulation sim(cfg);

        for (int i = 0; i < 6; ++i)
        {
            sim.AddNode({NodeType::kRouter, {static_cast<double>(i) * 40.0, 0.0, 0.0}, 160.0, true});
        }
        sim.AdvanceSec(20.0);

        return std::make_pair(sim.GetStats(), sim.GetAllNodeSnapshots());
    };

    auto res1 = runOnce();
    auto res2 = runOnce();

    CHECK(res1.first.mLeaders == 1);
    CHECK(res1.first.mPartitions == 1);
    CHECK(res1.first.mLeaders == res2.first.mLeaders);
    CHECK(res1.first.mRouters == res2.first.mRouters);
    CHECK(res1.first.mChildren == res2.first.mChildren);
    CHECK(res1.first.mTxFrames == res2.first.mTxFrames);
    CHECK(res1.first.mRxOk == res2.first.mRxOk);
    CHECK(res1.second.size() == res2.second.size());
    if (res1.second.size() == res2.second.size())
    {
        for (size_t i = 0; i < res1.second.size(); ++i)
        {
            CHECK(res1.second[i].mRole == res2.second[i].mRole);
            CHECK(res1.second[i].mRloc16 == res2.second[i].mRloc16);
            CHECK(res1.second[i].mPartitionId == res2.second[i].mPartitionId);
        }
    }
}

TEST_CASE("Simulation::Reset restores bit-for-bit identical determinism after active traffic")
{
    auto populateAndRun = [](Simulation &aSim) {
        aSim.SetRfPreset(RadioModelPreset::kMidisc);
        for (int i = 0; i < 8; ++i)
        {
            NodeType type = (i % 2 == 0) ? NodeType::kRouter : NodeType::kMed;

            aSim.AddNode(
                {type, {static_cast<double>(i % 4) * 35.0, static_cast<double>(i / 4) * 35.0, 0.0}, 160.0, true});
        }
        aSim.AdvanceSec(25.0);
        return aSim.GetStats();
    };

    SimulationStats freshStats;
    {
        SimulationConfig freshCfg;

        freshCfg.mSeed     = 4242;
        freshCfg.mRfPreset = RadioModelPreset::kMidisc;

        Simulation freshSim(freshCfg);

        freshStats = populateAndRun(freshSim);
    }

    SimulationStats resetStats;
    {
        SimulationConfig dirtyCfg;

        dirtyCfg.mSeed     = 2026;
        dirtyCfg.mRfPreset = RadioModelPreset::kMutualInterference;

        Simulation dirtySim(dirtyCfg);

        for (int i = 0; i < 10; ++i)
        {
            dirtySim.AddNode({NodeType::kRouter, {static_cast<double>(i) * 20.0, 0.0, 0.0}, 150.0, true});
        }
        dirtySim.AdvanceSec(28.0);
        (void)dirtySim.ExecuteCli(1, "ping ff02::1");
        dirtySim.AdvanceUs(1000); // Stop mid-flight with active alarms & in-flight radio events

        dirtySim.Reset(4242);
        resetStats = populateAndRun(dirtySim);
    }

    CHECK(freshStats.mLeaders == 1);
    CHECK(freshStats.mPartitions == 1);
    CHECK(freshStats.mDetached == 0);
    CHECK(resetStats.mLeaders == freshStats.mLeaders);
    CHECK(resetStats.mRouters == freshStats.mRouters);
    CHECK(resetStats.mChildren == freshStats.mChildren);
    CHECK(resetStats.mDetached == freshStats.mDetached);
    CHECK(resetStats.mPartitions == freshStats.mPartitions);
    CHECK(resetStats.mTxFrames == freshStats.mTxFrames);
    CHECK(resetStats.mRxOk == freshStats.mRxOk);
    CHECK(resetStats.mRxFcsErr == freshStats.mRxFcsErr);
}
