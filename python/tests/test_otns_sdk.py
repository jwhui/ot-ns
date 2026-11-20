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
"""Integration and 200-node scale tests for the OTNS v3 Python SDK (pytest & unittest compatible)."""

from __future__ import annotations

import math
import os
import sys
import time
import unittest
import urllib.request
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT_DIR / "python"))

from otns import OtnsRpcError, OtnsSession  # noqa: E402


def find_otns_bin() -> Path:
    env_bin = os.environ.get("OTNS_BIN")
    if env_bin:
        return Path(env_bin)
    for candidate in (ROOT_DIR / "build-release" / "otns", ROOT_DIR / "build-asan" / "otns", ROOT_DIR / "build" / "otns"):
        if candidate.is_file():
            return candidate
    raise FileNotFoundError("Could not find built 'otns' binary; set OTNS_BIN")


def first_non_empty_line(text: str) -> str:
    for line in text.splitlines():
        s = line.strip()
        if s and s != "Done":
            return s
    return ""


class TestOtnsV3Sdk(unittest.TestCase):
    def test_ws_formation_ping_pcap_kpi_replay_and_path_security(self) -> None:
        otns_bin = find_otns_bin()
        with OtnsSession(otns_bin, seed=2026, rf_model="MutualInterference", speed=0.0) as cli:
            info = cli.info()
            self.assertEqual(info.engine, "otns-v3")
            self.assertEqual(info.version, 1)

            cli.start_pcap("capture.pcap", fmt="wpan-tap")
            cli.start_replay("session.jsonl")

            # Path traversal attempts via JSON-RPC must be rejected cleanly.
            with self.assertRaises(OtnsRpcError) as ctx:
                cli.start_pcap("../escape.pcap")
            self.assertEqual(ctx.exception.code, "FORBIDDEN_PATH")

            with self.assertRaises(OtnsRpcError) as ctx2:
                cli.save_kpi("../escape.json")
            self.assertEqual(ctx2.exception.code, "FORBIDDEN_PATH")

            n1 = cli.add_router(x=0.0, y=0.0)
            cli.go(seconds=10.0)
            n2 = cli.add_router(x=50.0, y=0.0)
            n3 = cli.add_med(x=90.0, y=20.0)
            cli.go(seconds=18.0)

            st = cli.stats()
            self.assertEqual(st.total, 3)
            self.assertEqual(st.leaders, 1)
            self.assertEqual(st.partitions, 1)
            self.assertEqual(st.detached, 0)

            # Ping node 2 from node 1 over simulated Thread 1.4 RF.
            n2_mleid = first_non_empty_line(cli.cli(n2, "ipaddr mleid"))
            self.assertTrue(n2_mleid)
            cli.cli(n1, f"ping {n2_mleid}")
            cli.go(seconds=2.0)
            ping_out = cli.take_cli_output(n1)
            self.assertIn("1 packets transmitted, 1 packets received", ping_out)

            # Verify topology round-trip & KPI export.
            topo = cli.export_topology()
            self.assertEqual(len(topo["nodes"]), 3)
            kpi = cli.kpi()
            self.assertGreater(kpi["total"]["tx_frames"], 0)
            cli.save_kpi("kpi.json")

            frames = cli.stop_pcap()
            events = cli.stop_replay()
            self.assertGreater(frames, 0)
            self.assertGreater(events, 0)

            # Verify node move and removal.
            cli.move_node(n3, 95.0, 25.0)
            cli.remove_node(n3)
            self.assertEqual(cli.stats().total, 2)

    def test_unix_rpc_socket_control(self) -> None:
        otns_bin = find_otns_bin()
        with OtnsSession(otns_bin, seed=5555, rf_model="Midisc", speed=0.0, use_unix_rpc=True) as cli:
            n1 = cli.add_router(x=0.0, y=0.0)
            cli.go(seconds=10.0)
            n2 = cli.add_med(x=40.0, y=0.0)
            cli.go(seconds=12.0)

            st = cli.stats()
            self.assertEqual(st.total, 2)
            self.assertEqual(st.leaders, 1)
            self.assertEqual(st.children, 1)
            self.assertEqual(st.partitions, 1)
            self.assertIn("leader", cli.cli(n1, "state"))
            self.assertIn("child", cli.cli(n2, "state"))

    def test_scale_200_nodes_via_python_sdk(self) -> None:
        otns_bin = find_otns_bin()
        num_nodes = 200
        cols = math.ceil(math.sqrt(num_nodes))
        spacing = 35.0

        t0 = time.monotonic()
        with OtnsSession(otns_bin, seed=4242, rf_model="Midisc", speed=0.0, use_unix_rpc=True) as cli:
            for i in range(num_nodes):
                row = i // cols
                col = i % cols
                # Even 2D router grid (spacing 3*35 = 105 < 160 radio range) with MEDs filling the mesh.
                ntype = "router" if (row % 3 == 0 and col % 3 == 0) else "med"
                cli.add_node(ntype, x=col * spacing, y=row * spacing, radio_range=160.0)

            cli.go(seconds=60.0)
            st = cli.stats()
            wall = time.monotonic() - t0
            print(
                f"\n[Python SDK 200-node scale] nodes={st.total} sim_s=60 wall_s={wall:.2f} "
                f"leaders={st.leaders} routers={st.routers} children={st.children} "
                f"detached={st.detached} partitions={st.partitions}"
            )
            self.assertEqual(st.total, num_nodes)
            self.assertEqual(st.leaders, 1)
            self.assertEqual(st.partitions, 1)
            self.assertEqual(st.detached, 0)

    def test_http_serves_web_ui_and_security_headers(self) -> None:
        otns_bin = find_otns_bin()
        sess = OtnsSession(otns_bin, seed=1, speed=0.0)
        with sess:
            url = f"http://127.0.0.1:{sess.http_port}/"
            with urllib.request.urlopen(url, timeout=5.0) as resp:
                body = resp.read().decode("utf-8")
                self.assertEqual(resp.status, 200)
                self.assertEqual(resp.headers.get("X-Content-Type-Options"), "nosniff")
                self.assertEqual(resp.headers.get("X-Frame-Options"), "DENY")
                self.assertIn("OTNS v3", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
