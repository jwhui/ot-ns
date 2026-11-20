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
import select
import subprocess
import sys
import tempfile
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

    def test_hybrid_external_rfsim_node(self) -> None:
        otns_bin = find_otns_bin()
        rfsim_bin = otns_bin.parent / "ot-rfsim" / "ot-cli-ftd-rfsim"
        if not rfsim_bin.is_file():
            self.skipTest(f"External ot-rfsim binary not found at {rfsim_bin}")

        sess = OtnsSession(otns_bin, seed=101, rf_model="Ideal", speed=0.0, enable_rfsim=True)
        with sess as cli:
            assert sess.rfsim_socket_path is not None
            n1 = cli.add_router(x=0.0, y=0.0, radio_range=160.0)
            cli.go(seconds=10.0)
            dataset_hex = first_non_empty_line(cli.cli(n1, "dataset active -x"))
            self.assertTrue(dataset_hex)

            ext = subprocess.Popen(
                [str(rfsim_bin), "--socket", str(sess.rfsim_socket_path), "--node-id", "2"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                deadline = time.monotonic() + 5.0
                while time.monotonic() < deadline:
                    if any(n.node_id == 2 and n.kind == "external" for n in cli.nodes()):
                        break
                    time.sleep(0.05)

                nodes = {n.node_id: n for n in cli.nodes()}
                self.assertIn(2, nodes)
                self.assertEqual(nodes[2].kind, "external")

                cli.move_node(2, 40.0, 0.0)
                cli.cli(2, f"dataset set active {dataset_hex}")
                cli.cli(2, "ifconfig up")
                cli.cli(2, "thread start")
                cli.go(seconds=20.0)

                st = cli.stats()
                self.assertEqual(st.total, 2)
                self.assertEqual(st.leaders, 1)
                self.assertEqual(st.partitions, 1)
                self.assertEqual(st.detached, 0)

                n2_mleid = first_non_empty_line(cli.cli(2, "ipaddr mleid"))
                cli.cli(1, f"ping {n2_mleid}")
                cli.go(seconds=2.0)
                self.assertIn("1 packets transmitted, 1 packets received", cli.take_cli_output(1))
            finally:
                ext.terminate()
                ext.wait(timeout=5.0)

    def test_real_posix_host_over_ot_rcp_rfsim(self) -> None:
        otns_bin = find_otns_bin()
        rcp_bin = otns_bin.parent / "ot-rfsim" / "ot-rcp-rfsim"
        posix_cli_env = os.environ.get("OT_POSIX_CLI_BIN")
        if not posix_cli_env or not Path(posix_cli_env).is_file():
            self.skipTest("Set OT_POSIX_CLI_BIN to a built OpenThread POSIX ot-cli binary to run RCP host test")
        if not rcp_bin.is_file():
            self.skipTest(f"External ot-rcp-rfsim binary not found at {rcp_bin}")

        def read_until(fd: int, buf: list[str], predicate, timeout: float = 5.0) -> str:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                cur = buf[0]
                if predicate(cur):
                    buf[0] = ""
                    return cur
                r, _, _ = select.select([fd], [], [], 0.05)
                if r:
                    chunk = os.read(fd, 4096)
                    if not chunk:
                        break
                    buf[0] += chunk.decode("utf-8", errors="replace")
            cur = buf[0]
            buf[0] = ""
            return cur

        def run_posix_cmd(proc: subprocess.Popen[bytes], buf: list[str], cmd: str) -> list[str]:
            assert proc.stdin is not None and proc.stdout is not None
            proc.stdin.write((cmd + "\n").encode("utf-8"))
            proc.stdin.flush()
            raw = read_until(
                proc.stdout.fileno(),
                buf,
                lambda s: "Done\r\n" in s or "Done\n" in s or "Error " in s,
                timeout=5.0,
            )
            lines: list[str] = []
            for ln in raw.splitlines():
                s = ln.strip()
                while s.startswith(">"):
                    s = s[1:].strip()
                if s and s != "Done":
                    lines.append(s)
            return lines

        with tempfile.TemporaryDirectory(prefix="otns_rcp_host_") as td:
            sess = OtnsSession(otns_bin, seed=101, rf_model="Ideal", speed=0.0, enable_rfsim=True, output_dir=td)
            with sess as cli:
                assert sess.rfsim_socket_path is not None
                n1 = cli.add_router(x=0.0, y=0.0, radio_range=160.0)
                cli.go(seconds=10.0)
                dataset_hex = first_non_empty_line(cli.cli(n1, "dataset active -x"))
                n1_mleid = first_non_empty_line(cli.cli(n1, "ipaddr mleid"))
                self.assertTrue(dataset_hex)
                self.assertIn(":", n1_mleid)

                cli.set_speed(1.0)
                cli.resume()

                radio_url = (
                    f"spinel+hdlc+forkpty://{rcp_bin}"
                    f"?forkpty-arg=--socket&forkpty-arg={sess.rfsim_socket_path}"
                    f"&forkpty-arg=--node-id&forkpty-arg=2"
                )
                data_dir = Path(td) / "posix_data"
                data_dir.mkdir()
                posix = subprocess.Popen(
                    [posix_cli_env, "--data-path", str(data_dir), radio_url],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    bufsize=0,
                )
                try:
                    deadline = time.monotonic() + 8.0
                    while time.monotonic() < deadline:
                        if any(n.node_id == 2 and n.kind == "external" for n in cli.nodes()):
                            break
                        time.sleep(0.05)

                    nodes = {n.node_id: n for n in cli.nodes()}
                    self.assertIn(2, nodes)
                    self.assertEqual(nodes[2].kind, "external")
                    self.assertEqual(nodes[2].label, "rcp")
                    cli.move_node(2, 40.0, 0.0)

                    buf = [""]
                    rcp_ver = run_posix_cmd(posix, buf, "rcp version")
                    self.assertTrue(any("OPENTHREAD/" in l for l in rcp_ver), rcp_ver)

                    run_posix_cmd(posix, buf, f"dataset set active {dataset_hex}")
                    run_posix_cmd(posix, buf, "ifconfig up")
                    run_posix_cmd(posix, buf, "thread start")

                    state = ""
                    deadline = time.monotonic() + 15.0
                    while time.monotonic() < deadline:
                        vals = run_posix_cmd(posix, buf, "state")
                        state = vals[0] if vals else ""
                        if state in ("child", "router"):
                            break
                        time.sleep(0.4)
                    self.assertIn(state, ("child", "router"))

                    mleid2 = [l for l in run_posix_cmd(posix, buf, "ipaddr mleid") if ":" in l][0]

                    assert posix.stdin is not None and posix.stdout is not None
                    posix.stdin.write(f"ping {n1_mleid}\n".encode("utf-8"))
                    posix.stdin.flush()
                    ping_raw = read_until(posix.stdout.fileno(), buf, lambda s: "packets received" in s, timeout=5.0)
                    self.assertIn("1 packets transmitted, 1 packets received", ping_raw)

                    cli.cli(n1, f"ping {mleid2}")
                    time.sleep(1.5)
                    self.assertIn("1 packets transmitted, 1 packets received", cli.take_cli_output(n1))
                finally:
                    posix.terminate()
                    try:
                        posix.wait(timeout=5.0)
                    except subprocess.TimeoutExpired:
                        posix.kill()
                        posix.wait()
                    if posix.stdin is not None:
                        posix.stdin.close()
                    if posix.stdout is not None:
                        posix.stdout.close()

    def test_spawn_node_rpc_allowlist_and_matter_arg_style(self) -> None:
        otns_bin = find_otns_bin()
        rfsim_bin = otns_bin.parent / "ot-rfsim" / "ot-cli-ftd-rfsim"
        if not rfsim_bin.is_file():
            self.skipTest(f"External ot-rfsim binary not found at {rfsim_bin}")

        # 1. Without allow_spawn, node.spawn must be rejected with SPAWN_DISABLED.
        with OtnsSession(otns_bin, seed=77, rf_model="Ideal", speed=0.0, enable_rfsim=True) as cli:
            self.assertFalse(cli.info().spawn_enabled)
            with self.assertRaises(OtnsRpcError) as ctx:
                cli.spawn_node(rfsim_bin)
            self.assertEqual(ctx.exception.code, "SPAWN_DISABLED")

        # 2. With allow_spawn scoped to ot-rfsim/, /bin/sh is denied while ot-cli-ftd-rfsim spawns cleanly.
        with OtnsSession(
            otns_bin,
            seed=78,
            rf_model="Ideal",
            speed=0.0,
            allow_spawn=[rfsim_bin.parent],
        ) as cli:
            info = cli.info()
            self.assertTrue(info.spawn_enabled)
            self.assertTrue(info.rfsim_endpoint)

            with self.assertRaises(OtnsRpcError) as ctx:
                cli.spawn_node("/bin/sh")
            self.assertEqual(ctx.exception.code, "FORBIDDEN_EXECUTABLE")

            leader = cli.add_router(x=0.0, y=0.0, radio_range=160.0, label="br")
            cli.go(seconds=10.0)
            dataset_hex = first_non_empty_line(cli.cli(leader, "dataset active -x"))
            self.assertTrue(dataset_hex)

            # Spawn one node in native otns style and one in Matter --thread-args style.
            n_otns = cli.spawn_node(
                rfsim_bin,
                node_type="med",
                x=40.0,
                y=0.0,
                arg_style="otns",
                label="matter-switch",
            )
            n_matter = cli.spawn_node(
                rfsim_bin,
                node_type="med",
                x=-40.0,
                y=0.0,
                arg_style="matter",
                auto_start=False,
                label="matter-light",
            )

            cli.cli(n_matter, f"dataset set active {dataset_hex}")
            cli.cli(n_matter, "mode rn")
            cli.cli(n_matter, "ifconfig up")
            cli.cli(n_matter, "thread start")
            cli.go(seconds=18.0)

            nodes = {n.node_id: n for n in cli.nodes()}
            self.assertEqual(nodes[leader].label, "br")
            self.assertEqual(nodes[n_otns].kind, "external")
            self.assertEqual(nodes[n_otns].label, "matter-switch")
            self.assertEqual(nodes[n_matter].kind, "external")
            self.assertEqual(nodes[n_matter].label, "matter-light")

            st = cli.stats()
            self.assertEqual(st.total, 3)
            self.assertEqual(st.leaders, 1)
            self.assertEqual(st.partitions, 1)
            self.assertEqual(st.detached, 0)

            light_mleid = first_non_empty_line(cli.cli(n_matter, "ipaddr mleid"))
            cli.cli(leader, f"ping {light_mleid}")
            cli.go(seconds=2.0)
            self.assertIn("1 packets transmitted, 1 packets received", cli.take_cli_output(leader))

            cli.remove_node(n_matter)
            cli.remove_node(n_otns)
            self.assertEqual(cli.stats().total, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
