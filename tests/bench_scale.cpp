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

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "core/simulation.hpp"

int main(int aArgc, char *aArgv[])
{
    int    numNodes = (aArgc > 1) ? std::atoi(aArgv[1]) : 100;
    double simSecs  = (aArgc > 2) ? std::atof(aArgv[2]) : 30.0;

    if (numNodes < 2 || simSecs <= 0.0)
    {
        std::cerr << "Usage: bench_scale [num_nodes >= 2] [sim_secs > 0]\n";
        return 2;
    }

    otns::Core::SimulationConfig cfg;

    cfg.mSeed     = 4242;
    cfg.mRfPreset = otns::Rf::RadioModelPreset::kMidisc;

    auto                   t0 = std::chrono::steady_clock::now();
    otns::Core::Simulation sim(cfg);
    int                    cols    = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(numNodes))));
    double                 spacing = 40.0;

    // Place nodes on a grid; make roughly 1 in 4 router-eligible (up to Thread's 32-router cap) and the rest MEDs.
    for (int i = 0; i < numNodes; ++i)
    {
        int                  row  = i / cols;
        int                  col  = i % cols;
        otns::Core::NodeType type = (i % 4 == 0) ? otns::Core::NodeType::kRouter : otns::Core::NodeType::kMed;

        sim.AddNode({type, {col * spacing, row * spacing, 0.0}, 160.0, true});
    }

    sim.AdvanceSec(simSecs);

    auto   t1       = std::chrono::steady_clock::now();
    double wallSecs = std::chrono::duration<double>(t1 - t0).count();
    auto   st       = sim.GetStats();
    double speedup  = (wallSecs > 0.0) ? (simSecs / wallSecs) : 0.0;

    std::cout << "OTNS v3 Scale Benchmark: nodes=" << st.mTotal << " sim_s=" << simSecs << " wall_s=" << wallSecs
              << " speedup=" << speedup << "x leaders=" << st.mLeaders << " routers=" << st.mRouters
              << " children=" << st.mChildren << " detached=" << st.mDetached << " partitions=" << st.mPartitions
              << " tx=" << st.mTxFrames << " rx_ok=" << st.mRxOk << " rx_fcs_err=" << st.mRxFcsErr << "\n";

    if (st.mLeaders != 1 || st.mPartitions != 1 || st.mDetached != 0)
    {
        std::cerr << "FAIL: expected 1 leader, 1 partition, 0 detached\n";
        return 1;
    }

    return 0;
}
