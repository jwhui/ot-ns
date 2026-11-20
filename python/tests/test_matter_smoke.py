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
"""Unit tests for script/test-matter-smoke.py helper functions and CLI checks."""

from __future__ import annotations

import importlib.util
import io
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / "script" / "test-matter-smoke.py"


def load_smoke_module():
    spec = importlib.util.spec_from_file_location("test_matter_smoke", SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestMatterSmokeHelpers(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.smoke = load_smoke_module()

    def test_first_non_empty_line_and_echo_stripping(self) -> None:
        self.assertEqual(self.smoke.first_non_empty_line("\r\nchild\r\nDone\r\n> "), "child")
        self.assertEqual(
            self.smoke.first_non_empty_line("state\r\nchild\r\nDone\r\n> ", ignore_echo="state"),
            "child",
        )
        self.assertEqual(self.smoke.first_non_empty_line("> \nDone\n"), "")

    def test_pick_routable_address_prefers_omr_over_mleid_and_skips_rloc_and_link_local(self) -> None:
        ipaddr_out = "\n".join(
            [
                "fdde:ad00:beef:0:0:ff:fe00:fc00",
                "fd11:22:0:1:abcd:1234:5678:9abc",
                "fdde:ad00:beef:0:1111:2222:3333:4444",
                "fe80:0:0:0:100:0:0:3",
                "Done",
            ]
        )
        mleid_out = "ipaddr mleid\r\nfdde:ad00:beef:0:1111:2222:3333:4444\r\nDone\r\n"
        self.assertEqual(
            self.smoke.pick_routable_address(ipaddr_out, mleid_out),
            "fd11:22:0:1:abcd:1234:5678:9abc",
        )

        # Fallback to ML-EID when only RLOC, ML-EID, and link-local exist.
        no_omr = "\n".join(
            [
                "fdde:ad00:beef:0:0:ff:fe00:1400",
                "fdde:ad00:beef:0:1111:2222:3333:4444",
                "fe80::1",
                "Done",
            ]
        )
        self.assertEqual(
            self.smoke.pick_routable_address(no_omr, mleid_out),
            "fdde:ad00:beef:0:1111:2222:3333:4444",
        )

    def test_parse_onoff_attribute(self) -> None:
        self.assertIs(self.smoke.parse_onoff_attribute("[1700.0] [TOO]   OnOff: TRUE\n"), True)
        self.assertIs(self.smoke.parse_onoff_attribute("[1700.0] [TOO]   OnOff: FALSE\n"), False)
        self.assertIs(self.smoke.parse_onoff_attribute("OnOff: 1"), True)
        self.assertIs(self.smoke.parse_onoff_attribute("OnOff: 0"), False)
        self.assertIsNone(self.smoke.parse_onoff_attribute("CHIP:TOO: Command failed"))

    def test_build_radio_url(self) -> None:
        url = self.smoke.build_radio_url(Path("/usr/local/bin/ot-rcp-rfsim"), Path("/tmp/otns.sock"), 2, "otbr")
        self.assertEqual(
            url,
            "spinel+hdlc+forkpty:///usr/local/bin/ot-rcp-rfsim"
            "?forkpty-arg=--socket&forkpty-arg=/tmp/otns.sock"
            "&forkpty-arg=--node-id&forkpty-arg=2"
            "&forkpty-arg=--label&forkpty-arg=otbr",
        )

    def test_build_chip_tool_cmd_places_storage_directory_last(self) -> None:
        cmd = self.smoke.build_chip_tool_cmd(
            Path("/opt/matter/bin/chip-tool"),
            Path("/tmp/chip_store"),
            "pairing",
            "already-discovered",
            "1",
            "20202021",
            "fd11:22::1",
            "5540",
        )
        self.assertEqual(cmd[0], "/opt/matter/bin/chip-tool")
        self.assertEqual(cmd[1:3], ["pairing", "already-discovered"])
        self.assertEqual(cmd[-2:], ["--storage-directory", "/tmp/chip_store"])

    def test_build_otbr_cmd_and_default_backbone(self) -> None:
        cmd = self.smoke.build_otbr_cmd(
            Path("/bin/otbr-agent"),
            "wpan0",
            "otnsbr0",
            "spinel+hdlc+forkpty:///bin/rcp",
            data_path=Path("/tmp/otbr_data"),
        )
        self.assertEqual(cmd[0], "/bin/otbr-agent")
        self.assertIn("otnsbr0", cmd)
        self.assertIn("-s", cmd)
        self.assertIn("--data-path", cmd)
        self.assertIn("/tmp/otbr_data", cmd)

        sudo_cmd = self.smoke.build_otbr_cmd(
            Path("/bin/otbr-agent"), "wpan0", "otnsbr0", "spinel+hdlc+forkpty:///bin/rcp", sudo=True
        )
        self.assertEqual(sudo_cmd[0], "sudo")
        self.assertEqual(sudo_cmd[1], "/bin/otbr-agent")

        parsed = self.smoke.build_parser().parse_args([])
        self.assertEqual(parsed.backbone_if, "otnsbr0")
        self.assertFalse(parsed.sudo_otbr)

    def test_check_only_missing_and_present(self) -> None:
        with tempfile.TemporaryDirectory(prefix="otns_matter_test_") as td:
            d = Path(td)
            stderr = io.StringIO()
            with redirect_stderr(stderr):
                rc = self.smoke.main(
                    [
                        "--check-only",
                        "--otns-bin",
                        str(d / "otns"),
                        "--rcp-bin",
                        str(d / "ot-rcp-rfsim"),
                        "--matter-app",
                        str(d / "all-devices-app"),
                        "--chip-tool",
                        str(d / "chip-tool"),
                        "--otbr-agent",
                        str(d / "otbr-agent"),
                        "--ot-ctl",
                        str(d / "ot-ctl"),
                    ]
                )
            self.assertEqual(rc, 2)
            self.assertIn("Missing required binaries", stderr.getvalue())

            names = ["otns", "ot-rcp-rfsim", "all-devices-app", "chip-tool", "otbr-agent", "ot-ctl"]
            for name in names:
                (d / name).write_text("#!/bin/sh\n", encoding="utf-8")

            stdout = io.StringIO()
            with redirect_stdout(stdout):
                rc2 = self.smoke.main(
                    [
                        "--check-only",
                        "--otns-bin",
                        str(d / "otns"),
                        "--rcp-bin",
                        str(d / "ot-rcp-rfsim"),
                        "--matter-app",
                        str(d / "all-devices-app"),
                        "--chip-tool",
                        str(d / "chip-tool"),
                        "--otbr-agent",
                        str(d / "otbr-agent"),
                        "--ot-ctl",
                        str(d / "ot-ctl"),
                    ]
                )
            self.assertEqual(rc2, 0)
            self.assertIn("present", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
