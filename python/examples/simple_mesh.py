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
"""Simple 7-node Thread 1.4 mesh formation example using the OTNS v3 Python SDK."""

from __future__ import annotations

import math
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from otns import OtnsSession  # noqa: E402


def default_otns_bin() -> Path:
    env = os.environ.get("OTNS_BIN")
    if env:
        return Path(env)
    return Path(__file__).resolve().parents[2] / "build-release" / "otns"


def main() -> None:
    with OtnsSession(default_otns_bin(), seed=2026, rf_model="MutualInterference", speed=0.0) as sim:
        center = sim.add_router(x=0.0, y=0.0, radio_range=150.0)
        for i in range(6):
            ang = (2.0 * math.pi * i) / 6.0
            x = round(math.cos(ang) * 80.0, 1)
            y = round(math.sin(ang) * 80.0, 1)
            if i % 2 == 0:
                sim.add_router(x=x, y=y, radio_range=150.0)
            else:
                sim.add_med(x=x, y=y, radio_range=150.0)

        sim.go(seconds=25.0)
        stats = sim.stats()
        print(
            f"Formed mesh: total={stats.total} leaders={stats.leaders} "
            f"routers={stats.routers} children={stats.children} "
            f"detached={stats.detached} partitions={stats.partitions}"
        )
        assert stats.leaders == 1, f"Expected 1 leader, got {stats.leaders}"
        assert stats.detached == 0, f"Expected 0 detached, got {stats.detached}"
        assert stats.partitions == 1, f"Expected 1 partition, got {stats.partitions}"

        print(f"Node #{center} state: {sim.cli(center, 'state').splitlines()[0]}")


if __name__ == "__main__":
    main()
