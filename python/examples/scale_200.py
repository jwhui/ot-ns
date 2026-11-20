#!/usr/bin/env python3
#
#  Copyright (c) 2026, The OTNS Authors.
#  All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions are met:
#  1. Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.
#  2. Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the distribution.
#  3. Neither the name of the copyright holder nor the
#     names of its contributors may be used to endorse or promote products
#     derived from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
#  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
#  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
#  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
#  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
#  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
#  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
#  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
#  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
#  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
#  POSSIBILITY OF SUCH DAMAGE.
#
"""200-node converged Thread 1.4 scale benchmark via the OTNS v3 Python SDK."""

from __future__ import annotations

import math
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from otns import OtnsSession  # noqa: E402


def default_otns_bin() -> Path:
    env = os.environ.get("OTNS_BIN")
    if env:
        return Path(env)
    return Path(__file__).resolve().parents[2] / "build-release" / "otns"


def main() -> None:
    num_nodes = 200
    cols = math.ceil(math.sqrt(num_nodes))
    spacing = 35.0
    sim_seconds = 60.0

    with OtnsSession(default_otns_bin(), seed=4242, rf_model="Midisc", speed=0.0) as sim:
        t0 = time.monotonic()
        for i in range(num_nodes):
            row, col = divmod(i, cols)
            ntype = "router" if (row % 3 == 0 and col % 3 == 0) else "med"
            sim.add_node(ntype, x=col * spacing, y=row * spacing, radio_range=160.0)

        sim.go(seconds=sim_seconds)
        wall = time.monotonic() - t0
        stats = sim.stats()

        print(
            f"200-node benchmark: sim={sim_seconds:.0f}s wall={wall:.2f}s "
            f"({sim_seconds / max(wall, 1e-6):.1f}x) "
            f"leaders={stats.leaders} routers={stats.routers} "
            f"children={stats.children} detached={stats.detached} partitions={stats.partitions}"
        )
        assert stats.total == num_nodes
        assert stats.leaders == 1 and stats.detached == 0 and stats.partitions == 1, stats


if __name__ == "__main__":
    main()
