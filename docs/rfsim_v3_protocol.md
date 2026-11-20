# `ot-rfsim` v3 Wire Protocol Specification

## 1. Goals

The `ot-rfsim` v3 protocol connects external OS processes (Matter Linux apps, real OTBR/RCPs, and custom or legacy OpenThread binaries) to the OTNS v3 simulator over a Unix domain stream socket (`AF_UNIX`, `SOCK_STREAM`) or an IPv4 loopback TCP socket (`AF_INET`, bound to `127.0.0.1` by `--rfsim-tcp`; `AF_INET6` is reserved for future use).

Compared to v2's fixed 19-byte packed structs and 2,048-byte buffer cap, v3 provides:

- Explicit protocol magic and semantic versioning (`Hello` / `HelloAck` handshake).
- Length-prefixed framing with strict bounds checking (`kMaxPayloadBytes = 65536`).
- Extensible Type-Length-Value (TLV) payload fields so new radio and host attributes can be added without breaking older clients.
- Unified little-endian integer encoding independent of C compiler struct packing.

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
- `flags`: Bitmask (`0x0001 = FLAG_RELATIVE_DELAY`, `0x0002 = FLAG_CANCEL`, `0x0004 = FLAG_ACK_REQUESTED`, `0x0008 = FLAG_CSMA_CA`, `0x0010 = FLAG_RESPONSE`). Senders MUST set reserved bits to `0`; current v3.0 decoders ignore unknown flag bits for forward compatibility.
- `timestamp_us`: Simulation timestamp in microseconds (or relative delay when `FLAG_RELATIVE_DELAY` is set on `AlarmSet`).

---

## 3. TLV Encoding

Each TLV inside the payload body uses a 4-byte little-endian header:

```text
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           tlv_type (u16)      |         tlv_len (u16)         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    value (tlv_len bytes)                      |
...
```

Decoders MUST validate `offset + 4 + tlv_len <= payload_len` for every TLV. Because `payload_len <= 65536`, a single TLV value cannot exceed `65532` bytes (`kMaxPayloadBytes - 4`). Receivers preserve unknown TLV types at the codec layer and ignore unrecognized optional TLVs when handling messages, enabling forward-compatible minor revisions.

---

## 4. Message Types

| Group | `msg_type` | Direction | Description and TLVs |
| :--- | :---: | :--- | :--- |
| **Lifecycle** | `0x0001` `Hello` | Node → Sim | Handshake carrying `Magic` (`"OTRF"`), `Version` (`3.0`), requested `NodeId` (or `0` for auto-assign), and optional node label in `Text`. |
| | `0x0002` `HelloAck` | Sim → Node | Confirms assigned `NodeId`, `Seed`, and initial `timestamp_us`. |
| | `0x0003` `Shutdown` | Both | Graceful disconnect (no TLVs required). |
| **Time** | `0x0010` `AlarmSet` | Node → Sim | Requests wakeup at `timestamp_us` (supports `FLAG_RELATIVE_DELAY`), or cancels pending alarm when `FLAG_CANCEL` is set. |
| | `0x0011` `AlarmFired` | Sim → Node | Advances node clock to `timestamp_us` and signals alarm expiry. |
| | `0x0012` `StepDone` | Node → Sim | Signals node finished processing the current event and is idle. After `RxDone`, optionally carries `AckPsdu` to return a generated IMM-ACK or ENH-ACK frame to the simulator. |
| **Radio** | `0x0020` `RadioState` | Node → Sim | Reports `RadioState` (`state`, `sub_state`), `Channel`, `PowerDbm` (Tx power), and `SensitivityDbm`. |
| | `0x0021` `TxStart` | Node → Sim | Begins frame transmission with `Channel`, `PowerDbm`, `CcaThresholdDbm`, `Psdu` (`<= 127` bytes for 802.15.4), `MsgId`, and header flags `FLAG_ACK_REQUESTED` / `FLAG_CSMA_CA`. The simulator computes on-air duration from frame length. |
| | `0x0022` `TxDone` | Sim → Node | Signals Tx completion with `MsgId`, `ErrorCode` (`otError`), and on ACKed Tx, `AckPsdu`, `PowerDbm` (ACK RSSI), and `Lqi`. |
| | `0x0023` `RxStart` | Sim → Node | *Reserved (not yet emitted by v3.0 server/client).* Intended for start-of-reception notifications. |
| | `0x0024` `RxDone` | Sim → Node | Delivers received frame with `Channel`, `PowerDbm` (Rx RSSI), `Lqi`, and `Psdu`. |
| | `0x0025` `EnergyScan` | Both | Node requests CCA/energy sample (`Channel`, `DurationUs`); Sim replies with `FLAG_RESPONSE` and peak RSSI in `PowerDbm`. |
| | `0x0026` `RadioParam` | Both | *Reserved (not yet implemented in v3.0 server/client).* Intended for runtime RF parameter get/set (`ParamKv`). |
| **CLI / Logs** | `0x0030` `CliData` | Both | CLI input line (Sim → Node) or CLI output chunk (Node → Sim) in `Text`. |
| | `0x0031` `LogRecord` | Node → Sim | OpenThread log line in `Text` (client also attaches `otLogLevel` in `ErrorCode`; server currently stores `Text` only). |
| | `0x0032` `StatusPush` | Node → Sim | Structured node state update in `StatusInfo` (`role`, `rloc16`, `partition_id`, `ext_addr`). |
| **Networking** | `0x0040` `TrelPacket` | Both | TREL encapsulated packet (`NetAddr` + `Payload`). |
| | `0x0041` `InfraIfPacket` | Both | Border router infrastructure interface ICMPv6 ND or DHCPv6-PD packet (`NetAddr` + `Payload`; `dst_port=0` for ICMPv6 ND, `546`/`547` for DHCPv6-PD). |
| | `0x0042` `HostSocket` | Both | Simulated host UDP/IPv6 packet (`NetAddr` + `Payload`; omit `Payload` to bind `src_port`; set `FLAG_CANCEL` to unbind). |
| | `0x0043` `TrelPeer` | Both | TREL DNS-SD peer register/discover (`NetAddr` + optional TXT `Payload`; set `FLAG_CANCEL` for unregister/removal). |
| | `0x0044` `InfraIfState` | Node → Sim | Join or leave (`FLAG_CANCEL`) the simulated Adjacent Infrastructure Link (AIL). |

---

## 5. Standard TLV Types

| `tlv_type` | Name | Wire Layout | Notes |
| :---: | :--- | :--- | :--- |
| `0x0001` | `Magic` | 4-byte ASCII `"OTRF"` | Required in `Hello`. |
| `0x0002` | `Version` | `u16 major`, `u16 minor` | Little-endian (`3`, `0`). |
| `0x0003` | `NodeId` | `u32` | Little-endian node ID (`0` requests auto-assignment). |
| `0x0004` | `Seed` | `u64` | Deterministic PRNG seed assigned in `HelloAck`. |
| `0x0005` | `Channel` | `u8` | IEEE 802.15.4 channel (11–26). |
| `0x0006` | `PowerDbm` | `i8` | Tx power (`RadioState`, `TxStart`) or Rx/ACK/energy-scan RSSI (`RxDone`, `TxDone`, `EnergyScan`) in dBm. |
| `0x0007` | `SensitivityDbm` | `i8` | Receiver sensitivity threshold in dBm. |
| `0x0008` | `DurationUs` | `u64` | Duration in microseconds (little-endian). |
| `0x0009` | `ErrorCode` | `u8` | `otError` status in `TxDone`; also carries `otLogLevel` in `LogRecord`. |
| `0x000A` | `MsgId` | `u64` | Frame correlation ID (little-endian). |
| `0x000B` | `RadioState` | `u8 state`, `u8 sub_state` | See Section 6.1. |
| `0x000C` | `Psdu` | raw bytes | IEEE 802.15.4 PSDU (`<= 127` bytes enforced on Tx/Rx; codec TLV cap `65532` bytes). |
| `0x000D` | `Lqi` | `u8` | Link Quality Indicator (`0`–`255`). |
| `0x000E` | `Text` | UTF-8 bytes | Node label, CLI text, or log line. |
| `0x000F` | `ExtAddr` | 8-byte IEEE EUI-64 | *Reserved (unused in v3.0; extended address is carried inside `StatusInfo`).* |
| `0x0010` | `ParamKv` | `u16 param_id`, `i32 value` | *Reserved (codec-defined for future `RadioParam` use).* |
| `0x0011` | `Capabilities` | `u32` bitmask | *Reserved (unused in v3.0).* |
| `0x0012` | `NetAddr` | `u16 src_port`, `u16 dst_port`, `16B src_ip6`, `16B dst_ip6` | Ports are little-endian; IPv6 addresses are 16 bytes in standard network (big-endian) byte order. |
| `0x0013` | `Payload` | raw bytes | Encapsulated TREL, IPv6/ICMPv6, UDP, or DNS-SD TXT bytes. |
| `0x0014` | `CcaThresholdDbm` | `i8` | CCA energy detection threshold in dBm. |
| `0x0015` | `StatusInfo` | `u8 role`, `u16 rloc16`, `u32 partition_id`, `8B ext_addr` | 15 bytes: integers are little-endian; `ext_addr` is big-endian IEEE EUI-64 (see Section 6.1). |
| `0x0016` | `AckPsdu` | raw bytes | Generated IMM-ACK or ENH-ACK frame PSDU bytes. |

---

## 6. Normative Wire Conventions and Lock-Step Execution

### 6.1 Enum Values

- `RadioState.state` (`0x000B`): `0` = disabled, `1` = sleep, `2` = receive, `3` = transmit. `sub_state` is `0` in v3.0.
- `StatusInfo.role` (`0x0015`): `0` = disabled, `1` = detached, `2` = child, `3` = router, `4` = leader.

### 6.2 Lock-Step Handshake and Turn-Taking

1. **Startup**: The client connects, sends `Hello`, and blocks for `HelloAck` (initializing its local virtual clock to `HelloAck.timestamp_us`). After OpenThread initialization completes, the client drains tasklets, sends initial `AlarmSet` / `StatusPush` updates, and emits a startup `StepDone`.
2. **Event turns**: Whenever the simulator delivers a simulator-driven event (`AlarmFired`, `TxDone`, `RxDone`, `EnergyScan` response, `CliData`, `TrelPeer`, `TrelPacket`, `InfraIfPacket`, or `HostSocket`), the simulator waits synchronously for a matching `StepDone` before advancing virtual time.
3. **RX acknowledgment path**: On `RxDone`, if the frame matches the node's address filter and requests an ACK, the client generates the IMM-ACK or ENH-ACK PSDU during `RxDone` handling and attaches it in `AckPsdu` on the corresponding `StepDone`.
4. **Spontaneous client messages**: Clients may emit `RadioState`, `TxStart`, `AlarmSet`, `StatusPush`, `CliData`, `LogRecord`, `EnergyScan` requests, or networking frames while processing a turn or during external I/O (for example, Spinel UART activity on an RCP).
