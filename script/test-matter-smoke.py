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
"""End-to-end Matter-over-Thread smoke test running against OTNS v3.

Topology:
  - Node 1 (in-process OpenThread 1.4 Leader) at (0, 0)
  - Node 2 (real otbr-agent over Spinel/HDLC ot-rcp-rfsim) at (40, 0)
  - Node 3 (real connectedhomeip all-devices-app linked to ot-rfsim v3) at (80, 0)
  - Real chip-tool controller commissioning Node 3 through Node 2 and toggling
    the Matter On/Off cluster (on -> read TRUE -> off -> read FALSE)

Also records WPAN-TAP PCAP (matter_smoke.pcap) and Web UI JSONL replay
(matter_smoke.jsonl) inside --output-dir.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
for candidate in (REPO_ROOT / "python", Path("/opt/otns/python")):
    if candidate.is_dir() and str(candidate) not in sys.path:
        sys.path.insert(0, str(candidate))

from otns import OtnsSession  # noqa: E402

MATTER_NODE_ID = 1
MATTER_ENDPOINT = 1
MATTER_PASSCODE = 20202021
MATTER_DISCRIMINATOR = 3840
MATTER_PORT = 5540


def first_non_empty_line(text: str, *, ignore_echo: str = "") -> str:
    echo = ignore_echo.strip()
    for line in text.splitlines():
        s = line.strip()
        if not s or s == "Done" or s.startswith(">"):
            continue
        if echo and s == echo:
            echo = ""
            continue
        return s
    return ""


def parse_ipv6_addresses(cli_output: str) -> list[str]:
    addrs: list[str] = []
    for line in cli_output.splitlines():
        s = line.strip()
        if ":" in s and " " not in s and s != "Done" and not s.startswith(">"):
            addrs.append(s)
    return addrs


def pick_routable_address(ipaddr_output: str, mleid_output: str) -> str:
    """Prefer a non-link-local, non-RLOC OMR/global IPv6 address; fall back to ML-EID."""
    mleid = first_non_empty_line(mleid_output, ignore_echo="ipaddr mleid")
    for addr in parse_ipv6_addresses(ipaddr_output):
        low = addr.lower()
        if low.startswith("fe80:") or ":0:ff:fe00:" in low:
            continue
        if addr != mleid:
            return addr
    return mleid


def parse_onoff_attribute(chip_tool_output: str) -> bool | None:
    """Extract boolean OnOff attribute value from chip-tool 'onoff read on-off' logs."""
    # Handles both 'OnOff: TRUE' / 'OnOff: FALSE' and 'on-off: true' / '1' / '0'.
    m = re.search(r"OnOff:\s*(TRUE|FALSE|true|false|1|0)\b", chip_tool_output)
    if not m:
        return None
    val = m.group(1).lower()
    return val in ("true", "1")


def run_cmd(cmd: list[str], timeout: float = 45.0, check: bool = True) -> subprocess.CompletedProcess[str]:
    print(f"+ {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False)
    if check and proc.returncode != 0:
        raise RuntimeError(
            f"Command failed (rc={proc.returncode}): {' '.join(cmd)}\n"
            f"--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}"
        )
    return proc


def default_binary_paths() -> dict[str, str]:
    """Resolve default native binary paths (prefer local build-release and ~/.cache/otns-matter/bin)."""
    matter_dir = Path(os.environ.get("OTNS_MATTER_DIR", str(Path.home() / ".cache" / "otns-matter")))
    matter_bin = matter_dir / "bin"

    otns_default = REPO_ROOT / "build-release" / "otns"
    if not otns_default.is_file():
        otns_default = Path("/usr/local/bin/otns")

    rcp_default = REPO_ROOT / "build-release" / "ot-rfsim" / "ot-rcp-rfsim"
    if not rcp_default.is_file():
        rcp_default = Path("/usr/local/bin/ot-rcp-rfsim")

    return {
        "otns": os.environ.get("OTNS_BIN", str(otns_default)),
        "rcp": os.environ.get("OTNS_RCP_BIN", str(rcp_default)),
        "matter_app": os.environ.get("MATTER_APP_BIN", str(matter_bin / "all-devices-app")),
        "chip_tool": os.environ.get("CHIP_TOOL_BIN", str(matter_bin / "chip-tool")),
        "otbr_agent": os.environ.get("OTBR_AGENT_BIN", str(matter_bin / "otbr-agent")),
        "ot_ctl": os.environ.get("OT_CTL_BIN", str(matter_bin / "ot-ctl")),
    }


def ensure_loopback_up(priv_prefix: list[str]) -> None:
    """Ensure loopback 'lo' is UP (needed when invoked inside 'unshare --net')."""
    res = subprocess.run(["ip", "-o", "link", "show", "lo"], capture_output=True, text=True, check=False)
    if res.returncode == 0 and "UP" not in res.stdout:
        run_cmd([*priv_prefix, "ip", "link", "set", "lo", "up"])


def ensure_writable_run(priv_prefix: list[str]) -> None:
    """Mount a private tmpfs on /run when running inside rootless 'unshare -Urmn'."""
    if not priv_prefix and os.geteuid() == 0 and not os.access("/run", os.W_OK):
        run_cmd(["mount", "-t", "tmpfs", "tmpfs", "/run"])


def ensure_backbone_iface(name: str, priv_prefix: list[str]) -> tuple[str, bool]:
    """Ensure an UP backbone interface exists for otbr-agent -B; returns (iface, created_dummy)."""
    res = subprocess.run(["ip", "-o", "link", "show", name], capture_output=True, text=True, check=False)
    if res.returncode == 0:
        if "UP" not in res.stdout:
            run_cmd([*priv_prefix, "ip", "link", "set", name, "up"])
        return name, False

    run_cmd([*priv_prefix, "ip", "link", "add", name, "type", "dummy"])
    run_cmd([*priv_prefix, "ip", "link", "set", name, "up"])
    return name, True


def cleanup_backbone_iface(name: str, priv_prefix: list[str]) -> None:
    subprocess.run([*priv_prefix, "ip", "link", "delete", name], capture_output=True, text=True, check=False)


def build_otbr_cmd(
    otbr_bin: Path,
    thread_if: str,
    backbone_if: str,
    radio_url: str,
    *,
    data_path: Path | None = None,
    sudo: bool = False,
) -> list[str]:
    prefix = ["sudo"] if sudo else []
    cmd = [*prefix, str(otbr_bin), "-I", thread_if, "-B", backbone_if, "-d", "6", "-s"]
    if data_path is not None:
        cmd.extend(["--data-path", str(data_path)])
    cmd.append(radio_url)
    return cmd


def wait_for_ot_ctl(ot_ctl_bin: str, iface: str, priv_prefix: list[str], timeout: float = 15.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        res = subprocess.run(
            [*priv_prefix, ot_ctl_bin, "-I", iface, "state"], capture_output=True, text=True, check=False
        )
        if res.returncode == 0 and "Done" in res.stdout:
            return
        time.sleep(0.25)
    raise TimeoutError(f"otbr-agent on interface {iface} did not become ready within {timeout}s")


def build_chip_tool_cmd(chip_tool_bin: Path, storage_dir: Path, *args: str) -> list[str]:
    """Build a chip-tool invocation with --storage-directory placed after subcommand args."""
    return [str(chip_tool_bin), *args, "--storage-directory", str(storage_dir)]


def run_ot_ctl(ot_ctl_bin: str, iface: str, priv_prefix: list[str], *args: str) -> str:
    proc = run_cmd([*priv_prefix, ot_ctl_bin, "-I", iface, *args], timeout=15.0, check=True)
    return proc.stdout


def build_radio_url(rcp_bin: Path, rfsim_sock: Path, node_id: int, label: str = "otbr") -> str:
    return (
        f"spinel+hdlc+forkpty://{rcp_bin}"
        f"?forkpty-arg=--socket&forkpty-arg={rfsim_sock}"
        f"&forkpty-arg=--node-id&forkpty-arg={node_id}"
        f"&forkpty-arg=--label&forkpty-arg={label}"
    )


def build_parser() -> argparse.ArgumentParser:
    defaults = default_binary_paths()
    p = argparse.ArgumentParser(description="OTNS v3 + upstream connectedhomeip Matter-over-Thread smoke test")
    p.add_argument("--otns-bin", default=defaults["otns"])
    p.add_argument("--rcp-bin", default=defaults["rcp"])
    p.add_argument("--matter-app", default=defaults["matter_app"])
    p.add_argument("--chip-tool", default=defaults["chip_tool"])
    p.add_argument("--otbr-agent", default=defaults["otbr_agent"])
    p.add_argument("--ot-ctl", default=defaults["ot_ctl"])
    p.add_argument("--output-dir", default="/tmp/otns_matter_smoke")
    p.add_argument(
        "--backbone-if",
        default="otnsbr0",
        help="Backbone interface for otbr-agent -B (default: isolated dummy 'otnsbr0' to avoid host LAN traffic)",
    )
    p.add_argument("--thread-if", default="wpan0")
    p.add_argument("--device-type", default="on-off-light", help="Device type passed to all-devices-app --device")
    p.add_argument("--seed", type=int, default=2026)
    p.add_argument(
        "--sudo-otbr",
        action="store_true",
        help="Run ip link, otbr-agent, and ot-ctl via sudo while keeping otns/Matter apps unprivileged",
    )
    p.add_argument("--check-only", action="store_true", help="Verify required binaries exist and exit without running")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    required = {
        "otns": Path(args.otns_bin),
        "ot-rcp-rfsim": Path(args.rcp_bin),
        "matter-app": Path(args.matter_app),
        "chip-tool": Path(args.chip_tool),
        "otbr-agent": Path(args.otbr_agent),
        "ot-ctl": Path(args.ot_ctl),
    }
    missing = [f"{name}={path}" for name, path in required.items() if not path.is_file()]
    if missing:
        print("Missing required binaries:\n  " + "\n  ".join(missing), file=sys.stderr)
        return 2

    if args.check_only:
        print("All required OTNS v3 + Matter binaries present.")
        return 0

    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    priv_prefix = ["sudo"] if args.sudo_otbr else []
    ensure_writable_run(priv_prefix)
    ensure_loopback_up(priv_prefix)
    backbone_if, created_dummy = ensure_backbone_iface(args.backbone_if, priv_prefix)

    with tempfile.TemporaryDirectory(prefix="otns_matter_state_") as state_dir_str:
        state_dir = Path(state_dir_str)
        kvs_path = state_dir / "matter_light.kvs"
        storage_dir = state_dir / "chip_tool_storage"
        storage_dir.mkdir()
        otbr_settings = state_dir / "otbr"
        otbr_settings.mkdir(parents=True, exist_ok=True)

        otbr_proc: subprocess.Popen[str] | None = None
        otbr_log_fp = (out_dir / "otbr-agent.log").open("w", encoding="utf-8")

        try:
            sess = OtnsSession(
                required["otns"],
                seed=args.seed,
                rf_model="Ideal",
                speed=0.0,
                output_dir=out_dir,
                enable_rfsim=True,
                allow_spawn=[required["matter-app"].parent],
            )
            with sess as cli:
                assert sess.rfsim_socket_path is not None

                cli.start_pcap("matter_smoke.pcap", fmt="wpan-tap")
                cli.start_replay("matter_smoke.jsonl")

                # 1. Start in-process OpenThread 1.4 Leader (Node 1).
                leader_id = cli.add_router(x=0.0, y=0.0, radio_range=160.0, label="leader")
                cli.go(seconds=10.0)
                dataset_hex = first_non_empty_line(cli.cli(leader_id, "dataset active -x"))
                if not dataset_hex:
                    raise RuntimeError("Failed to read Active Operational Dataset from Leader")
                print(f"[OTNS] Leader #{leader_id} formed Thread 1.4 network", flush=True)

                # Switch simulator to real-time pacing for wall-clock otbr-agent + chip-tool.
                cli.set_speed(1.0)
                cli.resume()

                # 2. Launch real otbr-agent bridged over Spinel/HDLC ot-rcp-rfsim (Node 2).
                radio_url = build_radio_url(required["ot-rcp-rfsim"], sess.rfsim_socket_path, node_id=2, label="otbr")
                otbr_cmd = build_otbr_cmd(
                    required["otbr-agent"],
                    args.thread_if,
                    backbone_if,
                    radio_url,
                    data_path=otbr_settings,
                    sudo=args.sudo_otbr,
                )
                print(f"+ {' '.join(otbr_cmd)}", flush=True)
                otbr_proc = subprocess.Popen(otbr_cmd, stdout=otbr_log_fp, stderr=subprocess.STDOUT, text=True)

                wait_for_ot_ctl(str(required["ot-ctl"]), args.thread_if, priv_prefix, timeout=20.0)
                cli.move_node(2, 40.0, 0.0)
                run_ot_ctl(str(required["ot-ctl"]), args.thread_if, priv_prefix, "dataset", "set", "active", dataset_hex)
                run_ot_ctl(str(required["ot-ctl"]), args.thread_if, priv_prefix, "ifconfig", "up")
                run_ot_ctl(str(required["ot-ctl"]), args.thread_if, priv_prefix, "thread", "start")

                deadline = time.monotonic() + 20.0
                otbr_state = ""
                while time.monotonic() < deadline:
                    otbr_state = first_non_empty_line(
                        run_ot_ctl(str(required["ot-ctl"]), args.thread_if, priv_prefix, "state")
                    )
                    if otbr_state in ("child", "router", "leader"):
                        break
                    time.sleep(0.5)
                if otbr_state not in ("child", "router", "leader"):
                    raise RuntimeError(f"otbr-agent failed to attach to Thread network (state={otbr_state!r})")
                run_ot_ctl(str(required["ot-ctl"]), args.thread_if, priv_prefix, "srp", "server", "enable")
                print(f"[OTBR] Attached to OTNS v3 mesh as {otbr_state}", flush=True)

                # 3. Spawn real upstream connectedhomeip Matter device linked directly against ot-rfsim v3.
                matter_extra_args = [
                    "--device",
                    args.device_type,
                    "--discriminator",
                    str(MATTER_DISCRIMINATOR),
                    "--passcode",
                    str(MATTER_PASSCODE),
                    "--secured-device-port",
                    str(MATTER_PORT),
                    "--KVS",
                    str(kvs_path),
                ]
                light_id = cli.spawn_node(
                    required["matter-app"],
                    node_type="med",
                    x=80.0,
                    y=0.0,
                    radio_range=160.0,
                    arg_style="matter",
                    auto_start=False,
                    args=matter_extra_args,
                    label="matter-light",
                    timeout_ms=15000,
                )
                print(f"[Matter] Spawned {required['matter-app'].name} as Node #{light_id}", flush=True)

                # Configure Thread credentials on the Matter node via ot-rfsim v3 CLI UART bridge.
                cli.cli(light_id, f"dataset set active {dataset_hex}")
                cli.cli(light_id, "ifconfig up")
                cli.cli(light_id, "thread start")

                deadline = time.monotonic() + 25.0
                light_state = ""
                while time.monotonic() < deadline:
                    light_state = first_non_empty_line(cli.cli(light_id, "state"), ignore_echo="state")
                    if light_state in ("child", "router", "leader"):
                        break
                    time.sleep(0.5)
                if light_state not in ("child", "router", "leader"):
                    raise RuntimeError(f"Matter device failed to attach to Thread network (state={light_state!r})")

                # Wait briefly for OMR/SLAAC address assignment and SRP registration.
                time.sleep(3.0)
                light_ip = pick_routable_address(cli.cli(light_id, "ipaddr"), cli.cli(light_id, "ipaddr mleid"))
                if not light_ip:
                    raise RuntimeError("Failed to obtain IPv6 address from Matter device")
                print(f"[Matter] Device #{light_id} attached ({light_state}) ip={light_ip}", flush=True)

                # 4. Commission and control the Matter light via real chip-tool over wpan0 -> ot-rcp-rfsim -> OTNS v3!
                run_cmd(
                    build_chip_tool_cmd(
                        required["chip-tool"],
                        storage_dir,
                        "pairing",
                        "already-discovered",
                        str(MATTER_NODE_ID),
                        str(MATTER_PASSCODE),
                        light_ip,
                        str(MATTER_PORT),
                        "--bypass-attestation-verifier",
                        "true",
                    ),
                    timeout=90.0,
                )
                print("[Matter] Commissioning succeeded over simulated Thread 1.4 RF!", flush=True)

                # Turn light ON and read back OnOff attribute.
                run_cmd(
                    build_chip_tool_cmd(
                        required["chip-tool"],
                        storage_dir,
                        "onoff",
                        "on",
                        str(MATTER_NODE_ID),
                        str(MATTER_ENDPOINT),
                    ),
                    timeout=30.0,
                )
                read_on = run_cmd(
                    build_chip_tool_cmd(
                        required["chip-tool"],
                        storage_dir,
                        "onoff",
                        "read",
                        "on-off",
                        str(MATTER_NODE_ID),
                        str(MATTER_ENDPOINT),
                    ),
                    timeout=30.0,
                )
                on_val = parse_onoff_attribute(read_on.stdout + "\n" + read_on.stderr)
                if on_val is not True:
                    raise AssertionError(f"Expected OnOff=TRUE after 'onoff on', got {on_val!r}\n{read_on.stdout}")

                # Turn light OFF and read back OnOff attribute.
                run_cmd(
                    build_chip_tool_cmd(
                        required["chip-tool"],
                        storage_dir,
                        "onoff",
                        "off",
                        str(MATTER_NODE_ID),
                        str(MATTER_ENDPOINT),
                    ),
                    timeout=30.0,
                )
                read_off = run_cmd(
                    build_chip_tool_cmd(
                        required["chip-tool"],
                        storage_dir,
                        "onoff",
                        "read",
                        "on-off",
                        str(MATTER_NODE_ID),
                        str(MATTER_ENDPOINT),
                    ),
                    timeout=30.0,
                )
                off_val = parse_onoff_attribute(read_off.stdout + "\n" + read_off.stderr)
                if off_val is not False:
                    raise AssertionError(f"Expected OnOff=FALSE after 'onoff off', got {off_val!r}\n{read_off.stdout}")

                print("[Matter] OnOff cluster toggle verified (TRUE -> FALSE)!", flush=True)

                # 5. Export KPIs, PCAP, and Web UI JSONL replay.
                kpi = cli.kpi()
                cli.save_kpi("matter_smoke_kpi.json")
                frames = cli.stop_pcap()
                events = cli.stop_replay()
                if frames <= 0 or kpi.get("total", {}).get("tx_frames", 0) <= 0:
                    raise AssertionError(f"Expected simulated RF traffic, got frames={frames}, kpi={kpi}")

                print(
                    f"[PASS] Matter-over-Thread smoke test complete: "
                    f"pcap_frames={frames}, replay_events={events}, out={out_dir}",
                    flush=True,
                )
                return 0
        finally:
            if otbr_proc is not None:
                try:
                    otbr_proc.terminate()
                except PermissionError:
                    subprocess.run(["sudo", "kill", "-TERM", str(otbr_proc.pid)], check=False)
                try:
                    otbr_proc.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    try:
                        otbr_proc.kill()
                    except PermissionError:
                        subprocess.run(["sudo", "kill", "-KILL", str(otbr_proc.pid)], check=False)
                    otbr_proc.wait()
            otbr_log_fp.close()
            if created_dummy:
                cleanup_backbone_iface(backbone_if, priv_prefix)


if __name__ == "__main__":
    sys.exit(main())
