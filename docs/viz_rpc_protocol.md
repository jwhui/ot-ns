# Shared Visualization & Control Protocol (OTNS v3 + Nexus)

## 1. Overview

OTNS v3 replaces v2's `gRPC` + `grpcwebproxy` + `grpc-web` + stdin/stdout text-scraping stack with a **single embedded HTTP/1.1 + WebSocket (RFC 6455) JSON endpoint**.

The protocol is explicitly designed to work identically with:

- **OTNS v3** (C++17 hybrid simulator, live Web UI, Python SDK, CLI, and `.jsonl` replay).
- **`openthread/tests/nexus`** (via a lightweight WebSocket observer in a follow-up `openthread` PR).

The authoritative machine-readable contract lives in [protocol/schema/viz_rpc_v1.schema.json](../protocol/schema/viz_rpc_v1.schema.json).

---

## 2. Transport & Security

- **Default bind address**: `127.0.0.1:8999` (never `0.0.0.0` unless explicitly configured).
- **Endpoints**:
  - `GET /`, `GET /assets/*`: Embedded static Vite/TypeScript bundle (or `--web-dir` during UI development) served with `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, and a strict `Content-Security-Policy` (`default-src 'self'; connect-src 'self' ws://127.0.0.1:* ws://localhost:*; object-src 'none'`).
  - `GET /ws`: WebSocket upgrade endpoint. Rejects non-loopback `Origin` headers when bound to loopback.
  - Optional Unix domain socket JSON-RPC endpoint (`--rpc-socket`) for local Python SDK automation without TCP ports.

---

## 3. Message Envelope

Every WebSocket text frame (and every line in a `.jsonl` replay file) is a JSON object with protocol version `v: 1`:

### Client → Server: RPC Request

```json
{
  "v": 1,
  "type": "rpc.req",
  "id": 42,
  "method": "node.add",
  "params": { "role": "router", "x": 100.0, "y": 200.0, "z": 0.0 }
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
  "error": { "code": "INVALID_PARAMS", "message": "unknown role" }
}
```

### Server → Client: Visualization Stream Events

Designed for **200–1,000+ node scale**:

1. **`event.snapshot`**: Full simulator state sent immediately upon WebSocket connection or simulation reset (simulation metadata, RF model parameters, all nodes, active links, current stats).
2. **`event.batch_diff`**: Rate-coalesced updates (capped at 30–60 Hz by default) carrying only nodes added/removed/updated and links added/removed since the previous frame.
3. **`event.packet`**: Individual packet animation events (`src_id`, `dst_id`, `channel`, `duration_us`, `rssi_dbm`, `protocol`, `summary`) — automatically gated off by the server when simulation speed exceeds the configured animation threshold (default `5.0x`) unless explicitly filtered to watched nodes.
4. **`event.stats`**: Periodic summary counts (`total`, `leaders`, `routers`, `children`, `sed`, `detached`, `disabled`, `failed`, `partitions`).
5. **`event.log`**: Node log lines for explicitly watched nodes.
6. **`event.heartbeat`**: Periodic simulation time & speed synchronization (`ts_us`, `speed`, `paused`).

---

## 4. Core RPC Methods (v1)

| Method | Parameters | Notes |
| :--- | :--- | :--- |
| `sim.info` | `{}` | Returns engine name (`"otns-v3"` or `"nexus"`), protocol version, capabilities, `ts_us`, `speed`, `paused`. |
| `sim.go` | `{ "duration_us": u64, "speed"?: f64 }` | Advances simulation by `duration_us` (or runs continuously when omitted). |
| `sim.pause` / `sim.resume` | `{}` | Pauses/resumes continuous execution. |
| `sim.set_speed` | `{ "speed": f64 }` | Sets simulation speed (`0` = paused, `1.0` = real-time, `>1` = fast, `max`). |
| `sim.reset` | `{ "seed"?: u64 }` | Resets all nodes and virtual clock. |
| `node.add` | `{ "type": "router"\|"fed"\|"med"\|"sed"\|"br"\|"external", "x": f64, "y": f64, "z"?: f64, "radio_range"?: f64 }` | Creates an in-process node (or reserves config for an external node). |
| `node.remove` | `{ "node_id": u32 }` | Removes node cleanly. |
| `node.move` | `{ "node_id": u32, "x": f64, "y": f64, "z"?: f64 }` | Updates node 3D position and spatial index. |
| `node.set_enabled` | `{ "node_id": u32, "enabled": bool }` | Enables/disables node (simulates power off / failure). |
| `node.cli` | `{ "node_id": u32, "command": string }` | Executes an OpenThread CLI command on the node and returns output lines. |
| `rf.get_params` / `rf.set_params` | `{ "model"?: string, ... }` | Inspects/updates `libotns_rf` model parameters. |
| `viz.subscribe` | `{ "max_hz"?: u32, "packets"?: bool, "watch_nodes"?: [u32] }` | Configures client stream rate and filters. |
| `kpi.get` | `{}` | Returns structured KPI counters. |
| `topology.export` / `topology.import` | JSON topology object | Saves/restores topology state (file writes restricted to `--output-dir`). |

---

## 5. JSONL Replay Format

Replay files (`*.jsonl`) simply store one server-emitted event JSON object per line starting with an `event.snapshot`, followed by `event.batch_diff`, `event.packet`, `event.stats`, and `event.heartbeat` records ordered by `ts_us`. The Web UI can load a `.jsonl` file directly in-browser or stream it from `otns --replay <file>`.
