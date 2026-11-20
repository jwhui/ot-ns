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
"""Zero-dependency Python 3 client and daemon launcher for OTNS v3."""

from __future__ import annotations

import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import tempfile
from pathlib import Path
from typing import Any

from .models import NodeInfo, SimInfo, SimStats

_WS_MAGIC = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class OtnsRpcError(RuntimeError):
    """Error returned by an OTNS v3 JSON-RPC call."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class _Transport:
    def send_text(self, text: str) -> None:
        raise NotImplementedError

    def recv_text(self) -> str:
        raise NotImplementedError

    def close(self) -> None:
        raise NotImplementedError


class _WsTransport(_Transport):
    """Minimal RFC 6455 WebSocket text-frame client using Python stdlib only."""

    def __init__(self, host: str, port: int, timeout: float = 15.0) -> None:
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._buf = bytearray()

        key = base64.b64encode(os.urandom(16)).decode("ascii")
        expected_accept = base64.b64encode(
            hashlib.sha1(key.encode("ascii") + _WS_MAGIC).digest()
        ).decode("ascii")

        req = (
            f"GET /ws HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self._sock.sendall(req.encode("ascii"))

        while b"\r\n\r\n" not in self._buf:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise ConnectionError("EOF during WebSocket handshake")
            self._buf.extend(chunk)

        hdr_end = self._buf.index(b"\r\n\r\n")
        header_text = self._buf[:hdr_end].decode("ascii", errors="replace")
        del self._buf[: hdr_end + 4]

        lines = header_text.split("\r\n")
        if not lines or "101" not in lines[0]:
            raise ConnectionError(f"WebSocket upgrade rejected: {lines[0] if lines else ''}")

        headers = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        if headers.get("sec-websocket-accept") != expected_accept:
            raise ConnectionError("Invalid Sec-WebSocket-Accept header")

    def _read_exact(self, n: int) -> bytes:
        while len(self._buf) < n:
            chunk = self._sock.recv(max(4096, n - len(self._buf)))
            if not chunk:
                raise ConnectionError("WebSocket closed unexpectedly")
            self._buf.extend(chunk)
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    def send_text(self, text: str) -> None:
        payload = text.encode("utf-8")
        mask = os.urandom(4)
        header = bytearray([0x81])
        length = len(payload)
        if length <= 125:
            header.append(0x80 | length)
        elif length <= 65535:
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", length))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", length))
        header.extend(mask)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self._sock.sendall(bytes(header) + masked)

    def recv_text(self) -> str:
        while True:
            b0, b1 = self._read_exact(2)
            opcode = b0 & 0x0F
            masked = (b1 & 0x80) != 0
            length = b1 & 0x7F
            if length == 126:
                (length,) = struct.unpack("!H", self._read_exact(2))
            elif length == 127:
                (length,) = struct.unpack("!Q", self._read_exact(8))
            mask = self._read_exact(4) if masked else b""
            data = self._read_exact(length)
            if masked:
                data = bytes(b ^ mask[i & 3] for i, b in enumerate(data))

            if opcode == 0x01:
                return data.decode("utf-8")
            if opcode == 0x08:
                raise ConnectionError("WebSocket peer sent Close")
            if opcode == 0x09:
                # Ping -> Pong
                pong_mask = os.urandom(4)
                hdr = bytearray([0x8A, 0x80 | len(data)])
                hdr.extend(pong_mask)
                masked_pong = bytes(b ^ pong_mask[i & 3] for i, b in enumerate(data))
                self._sock.sendall(bytes(hdr) + masked_pong)

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass


class _UnixJsonTransport(_Transport):
    """Newline-delimited JSON-RPC transport over AF_UNIX stream socket."""

    def __init__(self, socket_path: str, timeout: float = 15.0) -> None:
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.settimeout(timeout)
        self._sock.connect(socket_path)
        self._buf = bytearray()

    def send_text(self, text: str) -> None:
        self._sock.sendall((text + "\n").encode("utf-8"))

    def recv_text(self) -> str:
        while b"\n" not in self._buf:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise ConnectionError("Unix JSON-RPC socket closed")
            self._buf.extend(chunk)
        idx = self._buf.index(b"\n")
        line = self._buf[:idx].decode("utf-8")
        del self._buf[: idx + 1]
        return line

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass


class OtnsClient:
    """Typed JSON-RPC client for OTNS v3 over WebSocket or Unix domain socket."""

    def __init__(self, transport: _Transport) -> None:
        self._transport = transport
        self._next_id = 1
        self._events: list[dict[str, Any]] = []

    @classmethod
    def connect_ws(cls, host: str = "127.0.0.1", port: int = 8999, timeout: float = 15.0) -> OtnsClient:
        return cls(_WsTransport(host, port, timeout=timeout))

    @classmethod
    def connect_unix(cls, socket_path: str | Path, timeout: float = 15.0) -> OtnsClient:
        return cls(_UnixJsonTransport(str(socket_path), timeout=timeout))

    def close(self) -> None:
        self._transport.close()

    def __enter__(self) -> OtnsClient:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def drain_events(self) -> list[dict[str, Any]]:
        out = list(self._events)
        self._events.clear()
        return out

    def call(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        req_id = self._next_id
        self._next_id += 1
        frame = {
            "v": 1,
            "type": "rpc.req",
            "id": req_id,
            "method": method,
            "params": params or {},
        }
        self._transport.send_text(json.dumps(frame, separators=(",", ":")))

        while True:
            msg = json.loads(self._transport.recv_text())
            if msg.get("type") == "rpc.res" and msg.get("id") == req_id:
                if "error" in msg:
                    err = msg["error"]
                    raise OtnsRpcError(str(err.get("code", "ERROR")), str(err.get("message", "")))
                result = msg.get("result")
                return result if isinstance(result, dict) else {"value": result}
            self._events.append(msg)

    def info(self) -> SimInfo:
        return SimInfo.from_dict(self.call("sim.info"))

    def go(
        self,
        seconds: float | None = None,
        duration_us: int | None = None,
        speed: float | None = None,
    ) -> int:
        params: dict[str, Any] = {}
        if duration_us is not None:
            params["duration_us"] = int(duration_us)
        elif seconds is not None:
            params["duration_sec"] = float(seconds)
        if speed is not None:
            params["speed"] = float(speed)
        res = self.call("sim.go", params)
        return int(res.get("ts_us", 0))

    def pause(self) -> None:
        self.call("sim.pause")

    def resume(self) -> None:
        self.call("sim.resume")

    def set_speed(self, speed: float) -> float:
        res = self.call("sim.set_speed", {"speed": float(speed)})
        return float(res["speed"])

    def reset(self, seed: int | None = None) -> None:
        params = {"seed": int(seed)} if seed is not None else {}
        self.call("sim.reset", params)

    def add_node(
        self,
        node_type: str = "router",
        x: float = 0.0,
        y: float = 0.0,
        z: float = 0.0,
        radio_range: float | None = None,
        auto_start: bool = True,
    ) -> int:
        params: dict[str, Any] = {
            "type": node_type,
            "x": float(x),
            "y": float(y),
            "z": float(z),
            "auto_start": bool(auto_start),
        }
        if radio_range is not None:
            params["radio_range"] = float(radio_range)
        res = self.call("node.add", params)
        return int(res["node_id"])

    def add_router(self, x: float = 0.0, y: float = 0.0, z: float = 0.0, **kwargs: Any) -> int:
        return self.add_node("router", x=x, y=y, z=z, **kwargs)

    def add_fed(self, x: float = 0.0, y: float = 0.0, z: float = 0.0, **kwargs: Any) -> int:
        return self.add_node("fed", x=x, y=y, z=z, **kwargs)

    def add_med(self, x: float = 0.0, y: float = 0.0, z: float = 0.0, **kwargs: Any) -> int:
        return self.add_node("med", x=x, y=y, z=z, **kwargs)

    def add_sed(self, x: float = 0.0, y: float = 0.0, z: float = 0.0, **kwargs: Any) -> int:
        return self.add_node("sed", x=x, y=y, z=z, **kwargs)

    def remove_node(self, node_id: int) -> None:
        self.call("node.remove", {"node_id": int(node_id)})

    def move_node(self, node_id: int, x: float, y: float, z: float = 0.0) -> None:
        self.call("node.move", {"node_id": int(node_id), "x": float(x), "y": float(y), "z": float(z)})

    def set_enabled(self, node_id: int, enabled: bool) -> None:
        self.call("node.set_enabled", {"node_id": int(node_id), "enabled": bool(enabled)})

    def cli(self, node_id: int, command: str) -> str:
        res = self.call("node.cli", {"node_id": int(node_id), "command": command})
        return str(res.get("output", ""))

    def take_cli_output(self, node_id: int) -> str:
        res = self.call("node.take_cli_output", {"node_id": int(node_id)})
        return str(res.get("output", ""))

    def nodes(self) -> list[NodeInfo]:
        snap = self.call("node.list")
        return [NodeInfo.from_dict(n) for n in snap.get("nodes", [])]

    def stats(self) -> SimStats:
        snap = self.call("node.list")
        return SimStats.from_dict(snap.get("stats", {}))

    def kpi(self) -> dict[str, Any]:
        return self.call("kpi.get")

    def save_kpi(self, path: str) -> None:
        self.call("kpi.save", {"path": path})

    def get_rf_params(self) -> dict[str, Any]:
        return self.call("rf.get_params")

    def set_rf_params(self, model: str | None = None, **overrides: Any) -> dict[str, Any]:
        params = dict(overrides)
        if model is not None:
            params["model"] = model
        return self.call("rf.set_params", params)

    def start_pcap(self, path: str, fmt: str = "wpan-tap") -> None:
        self.call("pcap.start", {"path": path, "format": fmt})

    def stop_pcap(self) -> int:
        res = self.call("pcap.stop")
        return int(res.get("frames", 0))

    def start_replay(self, path: str) -> None:
        self.call("replay.start", {"path": path})

    def stop_replay(self) -> int:
        res = self.call("replay.stop")
        return int(res.get("events", 0))

    def export_topology(self) -> dict[str, Any]:
        return self.call("topology.export")

    def import_topology(self, topo: dict[str, Any]) -> int:
        res = self.call("topology.import", topo)
        return int(res.get("imported_nodes", 0))

    def save_topology(self, path: str) -> None:
        self.call("topology.save", {"path": path})

    def load_topology(self, path: str) -> int:
        res = self.call("topology.load", {"path": path})
        return int(res.get("loaded_nodes", 0))


class OtnsSession:
    """Context manager that launches a headless otns daemon and connects an OtnsClient."""

    def __init__(
        self,
        otns_bin: str | Path,
        seed: int = 1,
        rf_model: str = "MutualInterference",
        speed: float = 0.0,
        output_dir: str | Path | None = None,
        use_unix_rpc: bool = False,
    ) -> None:
        self._otns_bin = str(otns_bin)
        self._seed = seed
        self._rf_model = rf_model
        self._speed = speed
        self._use_unix_rpc = use_unix_rpc
        self._temp_dir: tempfile.TemporaryDirectory[str] | None = None
        if output_dir is None:
            self._temp_dir = tempfile.TemporaryDirectory(prefix="otns_v3_py_")
            self.output_dir = Path(self._temp_dir.name)
        else:
            self.output_dir = Path(output_dir)
            self.output_dir.mkdir(parents=True, exist_ok=True)
        self._proc: subprocess.Popen[str] | None = None
        self.http_port: int = 0
        self.rpc_socket_path: Path | None = None
        self.client: OtnsClient | None = None

    def __enter__(self) -> OtnsClient:
        cmd = [
            self._otns_bin,
            "--headless",
            "--port",
            "0",
            "--bind",
            "127.0.0.1",
            "--seed",
            str(self._seed),
            "--rf-model",
            self._rf_model,
            "--speed",
            str(self._speed),
            "--output-dir",
            str(self.output_dir),
        ]
        if self._use_unix_rpc:
            self.rpc_socket_path = self.output_dir / "rpc.sock"
            cmd.extend(["--rpc-socket", str(self.rpc_socket_path)])

        self._proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        assert self._proc.stdout is not None
        while True:
            line = self._proc.stdout.readline()
            if not line:
                err = self._proc.stderr.read() if self._proc.stderr else ""
                raise RuntimeError(f"otns exited prematurely: {err}")
            if line.startswith("OTNS_HTTP_PORT="):
                self.http_port = int(line.split("=", 1)[1].strip())
                break

        if self._use_unix_rpc and self.rpc_socket_path is not None:
            self.client = OtnsClient.connect_unix(self.rpc_socket_path)
        else:
            self.client = OtnsClient.connect_ws("127.0.0.1", self.http_port)
        return self.client

    def __exit__(self, *_exc: object) -> None:
        if self.client is not None:
            self.client.close()
            self.client = None
        if self._proc is not None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait()
            if self._proc.stdout is not None:
                self._proc.stdout.close()
            if self._proc.stderr is not None:
                self._proc.stderr.close()
            self._proc = None
        if self._temp_dir is not None:
            self._temp_dir.cleanup()
            self._temp_dir = None
