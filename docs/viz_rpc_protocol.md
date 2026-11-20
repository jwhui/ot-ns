# Shared Visualization and Control Protocol (OTNS v3 + Nexus)

## 1. Overview

OTNS v3 replaces v2's `gRPC`, `grpcwebproxy`, `grpc-web`, and stdin/stdout text-scraping stack with a single embedded HTTP/1.1 and WebSocket (RFC 6455) JSON endpoint.

The protocol is designed to work identically with:

- OTNS v3 (C++17 hybrid simulator, live Web UI, Python SDK, CLI, and `.jsonl` replay).
- `openthread/tests/nexus` (via a lightweight WebSocket observer in a follow-up `openthread` PR).

The machine-readable contract lives in [protocol/schema/viz_rpc_v1.schema.json](../protocol/schema/viz_rpc_v1.schema.json).

---

## 2. Transport and Security

- **Default bind address**: `127.0.0.1:8999` (never `0.0.0.0` unless explicitly configured with `--bind`).
- **HTTP endpoints**:
  - `GET /`, `GET /assets/*`: Embedded static Vite and TypeScript bundle (or `--web-dir` during UI development). Responses include `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`, and `Content-Security-Policy: default-src 'self'; connect-src 'self' ws://127.0.0.1:* ws://localhost:* ws://[::1]:*; style-src 'self' 'unsafe-inline'; object-src 'none'; base-uri 'none'`.
  - `GET /healthz`: Lightweight health check returning `200 OK` with `{"status":"ok"}`.
- **WebSocket endpoint (`GET /ws`)**: Upgrades to RFC 6455 JSON streaming. Native clients without an `Origin` header are permitted. Browser `Origin` headers must be loopback (`127.0.0.1`, `localhost`, or `[::1]`), or match `http(s)://<Host>` when bound to a non-loopback address.
- **Unix domain socket**: Optional `--rpc-socket <path>` endpoint for local Python SDK automation without TCP ports.

---

## 3. Message Envelope

Every WebSocket text frame and every line in a `.jsonl` replay file is a JSON object with protocol version `v: 1`.

### Client → Server: RPC Request

```json
{
  "v": 1,
  "type": "rpc.req",
  "id": 42,
  "method": "node.add",
  "params": { "type": "router", "x": 100.0, "y": 200.0, "z": 0.0 }
}
```

### Server → Client: RPC Response

```json
{
  "v": 1,
  "type": "rpc.res",
  "id": 42,
  "result": { "node_id": 1 }
}
```

Or on failure:

```json
{
  "v": 1,
  "type": "rpc.res",
  "id": 42,
  "error": { "code": "INVALID_PARAMS", "message": "Unknown node 'type'" }
}
```

When a request fails JSON parsing before `id` can be extracted, the server responds with `"id": null` and error code `"PARSE_ERROR"`.

### Server → Client: Visualization Stream Events

Designed for 200 to 1,000+ node scale:

1. `event.snapshot`: Full simulator state sent immediately upon WebSocket connection or simulation reset (`engine`, `speed`, `paused`, `rf_model` preset name, `nodes`, `links`, and `stats`).
2. `event.batch_diff`: Rate-coalesced updates (capped at 30 to 60 Hz by default) carrying `upsert_nodes`, `remove_nodes`, `upsert_links`, and `remove_links` since the previous frame.
3. `event.packet`: Packet animation events (`src`, `dst`, `channel`, `duration_us`, `rssi_dbm`, `frame_len`, `protocol`, and `summary`). Automatically gated off when simulation speed is `0.0` (unpaced max speed) or exceeds the animation threshold (default `5.0x`), unless explicitly filtered to watched nodes.
4. `event.stats`: Periodic summary counts (`total`, `leaders`, `routers`, `children`, `detached`, `disabled`, and `partitions`; `sed` and `failed` are optional in the schema and not emitted by OTNS v3).
5. `event.log`: *Reserved in schema (not yet emitted by OTNS v3).* Intended for streaming node log lines (`node_id`, `level`, `message`).
6. `event.heartbeat`: Periodic simulation time and speed synchronization (`ts_us`, `speed`, and `paused`).

---

## 4. Core RPC Methods (v1)

| Method | Parameters | Notes |
| :--- | :--- | :--- |
| `sim.info` | `{}` | Returns `engine` (`"otns-v3"` or `"nexus"`), protocol version, `ts_us`, `speed`, `paused`, `node_count`, `spawn_enabled`, and optional `rfsim_endpoint`. |
| `sim.go` | `{ "duration_us"?: u64, "duration_sec"?: f64, "speed"?: f64 }` | Advances simulation by `duration_us` or `duration_sec` (or resumes continuous execution when omitted). |
| `sim.pause` / `sim.resume` | `{}` | Pauses or resumes continuous execution. |
| `sim.set_speed` | `{ "speed": f64 }` | Sets simulation speed (`0` = max/unpaced, `1.0` = real time, `>1` = faster than real time). |
| `sim.reset` | `{ "seed"?: u64 }` | Resets all nodes and the virtual clock. |
| `node.add` | `{ "type"?: "router"\|"fed"\|"med"\|"sed"\|"br", "x"?: f64, "y"?: f64, "z"?: f64, "radio_range"?: f64, "auto_start"?: bool, "label"?: string }` | Creates an in-process OpenThread node (`"role"` is also accepted as a fallback alias for `"type"`; `"br"` currently aliases `"router"`). |
| `node.spawn` | `{ "executable": string, "arg_style"?: "otns"\|"matter"\|"raw", "args"?: [string], "type"?: string, "x"?: f64, "y"?: f64, "z"?: f64, "radio_range"?: f64, "auto_start"?: bool, "label"?: string, "timeout_ms"?: u32 }` | Spawns an allowlisted external `ot-rfsim` v3 process via `fork`/`execv` (no shell). Disabled unless `otns` is launched with `--allow-spawn` and `--rfsim-socket` or `--rfsim-tcp`. |
| `node.remove` | `{ "node_id": u32 }` | Removes the node cleanly. |
| `node.move` | `{ "node_id": u32, "x": f64, "y": f64, "z"?: f64 }` | Updates node 3D position and spatial index. |
| `node.set_enabled` | `{ "node_id": u32, "enabled": bool }` | Enables or disables a node (simulating power off or failure). |
| `node.list` | `{}` | Returns a full snapshot object (`nodes`, `links`, `stats`, and simulation metadata). |
| `node.cli` | `{ "node_id": u32, "command": string }` | Executes an OpenThread CLI command and returns `{ "node_id", "output": string }`. |
| `node.take_cli_output` | `{ "node_id": u32 }` | Drains and returns buffered asynchronous CLI output as `{ "node_id", "output": string }`. |
| `rf.get_params` | `{}` | Returns all `otns_rf` radio model parameters. |
| `rf.set_params` | `{ "model"?: string, "noise_floor_dbm"?: f64, "shadow_fading_sigma_db"?: f64, "time_fading_sigma_max"?: f64, "meter_per_unit"?: f64, "is_disc_limit"?: bool }` | Updates the RF preset and/or supported tuning fields and returns the active parameter set. |
| `viz.subscribe` | `{ "max_hz"?: u32, "packets"?: bool, "watch_nodes"?: [u32] }` | Configures per-client stream rate and packet filters. |
| `kpi.get` | `{}` | Returns structured per-node and aggregate KPI counters. |
| `kpi.save` | `{ "path": string }` | Writes KPI JSON inside `--output-dir`. |
| `pcap.start` | `{ "path": string, "format"?: "wpan-tap"\|"ieee802154" }` | Starts PCAP capture inside `--output-dir` (default format: `"wpan-tap"`). |
| `pcap.stop` | `{}` | Stops PCAP capture and returns recorded frame count. |
| `replay.start` | `{ "path": string }` | Starts JSONL replay recording inside `--output-dir`. |
| `replay.stop` | `{}` | Stops JSONL replay recording and returns recorded event count. |
| `topology.export` / `topology.import` | `{}` / JSON topology object | Exports or imports topology state in memory (no filesystem I/O). |
| `topology.save` / `topology.load` | `{ "path": string }` | Saves or loads a JSON topology file contained within `--output-dir`. |

### Error Codes

| Code | Meaning |
| :--- | :--- |
| `PARSE_ERROR` | Malformed JSON frame (`"id": null`). |
| `INVALID_REQUEST` | Missing or invalid envelope fields (`v`, `type`, `id`, or `method`). |
| `INVALID_PARAMS` | Missing, mistyped, or out-of-range method parameter. |
| `METHOD_NOT_FOUND` | Unknown RPC method name. |
| `NOT_FOUND` | Referenced `node_id` does not exist. |
| `FORBIDDEN_PATH` | Requested file path escapes `--output-dir` or fails to open. |
| `RFSIM_DISABLED` | `node.spawn` called without `--rfsim-socket` or `--rfsim-tcp`. |
| `SPAWN_DISABLED` | `node.spawn` called without `--allow-spawn`. |
| `FORBIDDEN_EXECUTABLE` | Executable path is outside `--allow-spawn` entries. |
| `SPAWN_FAILED` | External process failed to start or did not connect within `timeout_ms`. |

---

## 5. JSONL Replay Format

Replay files (`*.jsonl`) store one server-emitted event JSON object per line, beginning with `event.snapshot` and followed by `event.batch_diff`, `event.packet`, `event.stats`, and `event.heartbeat` records ordered by `ts_us`. Load a `.jsonl` file directly in the Web UI or stream it with `otns --replay <file>`.
