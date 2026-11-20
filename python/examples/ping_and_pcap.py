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
"""Multi-hop IPv6 ping with WPAN-TAP PCAP, KPI JSON, and JSONL replay capture."""

from __future__ import annotations

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


def first_value_line(cli_output: str) -> str:
    for line in cli_output.splitlines():
        s = line.strip()
        if s and s != "Done":
            return s
    return ""


def main() -> None:
    session = OtnsSession(default_otns_bin(), seed=7777, rf_model="Midisc", speed=0.0)
    with session as sim:
        sim.start_pcap("multihop.pcap", fmt="wpan-tap")
        sim.start_replay("multihop.jsonl")

        # 4 routers in a 3-hop chain (spacing 70, radio_range 110 => strictly multi-hop)
        nodes = [sim.add_router(x=0.0, y=0.0, radio_range=110.0)]
        sim.go(seconds=10.0)
        for i in range(1, 4):
            nodes.append(sim.add_router(x=i * 70.0, y=0.0, radio_range=110.0))
            sim.go(seconds=12.0)
        sim.go(seconds=10.0)

        stats = sim.stats()
        assert stats.leaders == 1 and stats.partitions == 1 and stats.detached == 0, stats

        dst_mleid = first_value_line(sim.cli(nodes[-1], "ipaddr mleid"))
        assert dst_mleid, "Missing destination ML-EID"

        sim.cli(nodes[0], f"ping {dst_mleid} 32 3")
        sim.go(seconds=5.0)
        ping_out = sim.take_cli_output(nodes[0])
        assert "3 packets transmitted, 3 packets received" in ping_out, ping_out

        frames = sim.stop_pcap()
        events = sim.stop_replay()
        sim.save_kpi("multihop_kpi.json")

        print(
            f"Multi-hop ping OK ({nodes[0]} -> {nodes[-1]} {dst_mleid}): "
            f"pcap_frames={frames} replay_events={events}"
        )


if __name__ == "__main__":
    main()
