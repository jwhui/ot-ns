# `ot-rfsim` v3 Wire Protocol Specification

## 1. Goals

The `ot-rfsim` v3 protocol connects external OS processes (Matter Linux apps, real OTBR/RCPs, custom/legacy OpenThread binaries) to the OTNS v3 simulator over a **Unix domain stream socket** (`AF_UNIX`, `SOCK_STREAM`) or **TCP socket** (`AF_INET`/`AF_INET6` for Docker/remote nodes).

Compared to v2's fixed 19-byte packed structs and 2048-byte buffer cap, v3 provides:

- Explicit protocol magic & semantic versioning (`Hello` / `HelloAck` handshake).
- Length-prefixed framing with strict bounds checking (`kMaxPayloadBytes = 65536`).
- Extensible **TLV (Type-Length-Value)** payload fields so new radio/host attributes can be added without breaking older clients.
- Unified little-endian wire encoding independent of C compiler struct packing.

---

## 2. Frame Header (16 Bytes, Little-Endian)

Every message on the stream consists of a 16-byte header followed by `payload_len` bytes of TLV records:

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        payload_len (u32)                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          msg_type (u16)       |          flags (u16)          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                     timestamp_us (u64)                        +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   TLV payload (payload_len bytes)             |
...
```

- `payload_len`: Byte length of the TLV body (`0 <= payload_len <= 65536`). Decoders MUST reject frames exceeding `kMaxPayloadBytes` and close the connection.
- `msg_type`: Message type identifier (see Section 4).
- `flags`: Bitmask (`0x0001 = FLAG_RELATIVE_DELAY` indicating `timestamp_us` is a relative delay in µs rather than an absolute simulation timestamp; reserved bits MUST be `0`).
- `timestamp_us`: Simulation timestamp in microseconds (or relative delay when `FLAG_RELATIVE_DELAY` is set).

---

## 3. TLV Encoding

Each TLV inside the payload body is encoded in little-endian order:

```text
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           tlv_type (u16)      |         tlv_len (u16)         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    value (tlv_len bytes)                      |
...
```

Decoders MUST validate `offset + 4 + tlv_len <= payload_len` for every TLV and silently skip unknown optional TLV types (`tlv_type >= 0x8000` or forward-compatible additions) to allow minor-version evolution.

---

## 4. Message Types

| Group | `msg_type` | Direction | Description |
| :--- | :---: | :--- | :--- |
| **Lifecycle** | `0x0001` `Hello` | Node → Sim | Handshake (`magic="OTRF"`, `version_major=3`, `version_minor=0`, requested `node_id` or `0`, capability flags, node label). |
| | `0x0002` `HelloAck` | Sim → Node | Confirms assigned `node_id`, `seed`, initial simulation time, and accepted capabilities. |
| | `0x0003` `Shutdown` | Both | Graceful disconnect with optional reason code. |
| **Time** | `0x0010` `AlarmSet` | Node → Sim | Requests wakeup at `timestamp_us` (or cancels if `FLAG_CANCEL`). |
| | `0x0011` `AlarmFired` | Sim → Node | Advances node clock to `timestamp_us` and signals alarm expiry. |
| | `0x0012` `StepDone` | Node → Sim | Signals node finished processing current tick and is idle until next alarm/event. |
| **Radio** | `0x0020` `RadioState` | Node → Sim | Reports radio state (`disabled`/`sleep`/`rx`/`tx`), channel, Tx power, Rx sensitivity, sub-state. |
| | `0x0021` `TxStart` | Node → Sim | Begins frame transmission (channel, tx_power_dbm, duration_us, PSDU bytes, msg_id). |
| | `0x0022` `TxDone` | Sim → Node | Signals TX completion (msg_id, ot_error, optional Ack PSDU). |
| | `0x0023` `RxStart` | Sim → Node | Signals start of incoming frame reception (channel, rssi_dbm, duration_us, msg_id). |
| | `0x0024` `RxDone` | Sim → Node | Signals end of incoming frame reception (channel, rssi_dbm, lqi, ot_error, PSDU bytes, msg_id). |
| | `0x0025` `EnergyScan` | Both | Node requests CCA/energy sample (channel, duration_us); Sim replies with peak RSSI dBm. |
| | `0x0026` `RadioParam` | Both | Get/Set/Response for runtime RF parameters (CSL accuracy, sensitivity, bitrate, etc.). |
| **CLI / Logs** | `0x0030` `CliData` | Both | CLI input line (Sim → Node) or CLI output text (Node → Sim). |
| | `0x0031` `LogRecord` | Node → Sim | Structured OpenThread log record (level, region, text). |
| | `0x0032` `StatusPush` | Node → Sim | Structured node state update (role, RLOC16, ext_addr, partition_id, parent, neighbor delta). |
| **Networking** | `0x0040` `TrelPacket` | Both | TREL encapsulated packet + peer IPv6/port metadata. |
| | `0x0041` `InfraIfPacket` | Both | Border router infrastructure interface ICMPv6/IPv6/mDNS packet. |
| | `0x0042` `HostSocket` | Both | Simulated host UDP/IPv6 packet exchange. |

---

## 5. Standard TLV Types

| `tlv_type` | Name | Wire Type |
| :---: | :--- | :--- |
| `0x0001` | `Magic` | 4-byte ASCII `"OTRF"` |
| `0x0002` | `Version` | `u16 major`, `u16 minor` |
| `0x0003` | `NodeId` | `u32` |
| `0x0004` | `Seed` | `u64` |
| `0x0005` | `Channel` | `u8` |
| `0x0006` | `PowerDbm` | `i8` (dBm) |
| `0x0007` | `SensitivityDbm` | `i8` (dBm) |
| `0x0008` | `DurationUs` | `u64` |
| `0x0009` | `ErrorCode` | `u8` (`otError`) |
| `0x000A` | `MsgId` | `u64` |
| `0x000B` | `RadioState` | `u8 state`, `u8 sub_state` |
| `0x000C` | `Psdu` | raw bytes (`<= 127` bytes for 802.15.4, up to `kMaxPayloadBytes` for future PHYs) |
| `0x000D` | `Lqi` | `u8` |
| `0x000E` | `Text` | UTF-8 bytes |
| `0x000F` | `ExtAddr` | 8-byte IEEE EUI-64 |
| `0x0010` | `ParamKv` | `u16 param_id`, `i32 value` |
| `0x0011` | `Capabilities` | `u32` bitmask |
| `0x0012` | `NetAddr` | `u16 src_port`, `u16 dst_port`, `16B src_ip6`, `16B dst_ip6` |
| `0x0013` | `Payload` | raw bytes (TREL / IPv6 / UDP) |
