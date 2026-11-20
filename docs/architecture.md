# OTNS v3 Architecture RFC

## 1. Motivation

OTNS v2 proved the value of realistic RF/interference simulation, multi-version Thread interoperability, Matter/OTBR integration, and interactive visualization. However, maintaining OTNS v2 revealed several structural bottlenecks:

1. **Community language mismatch**: The simulator core is written in Go while almost the entire OpenThread ecosystem (`openthread`, `ot-br-posix`, `ot-commissioner`, `tests/nexus`, Matter) is C/C++, shrinking the reviewer pool to 1–2 maintainers.
2. **Frontend & transport toolchain rot**: `/web/site` relies on EOL Webpack 4, PixiJS 5, an unused deprecated native `grpc` C++ Node addon, an external 2020 `x86_64`-only `grpcwebproxy` binary, legacy `protoc-gen-go@v1.5.2`, and `go-bindata` committing a 3 MB generated `bindata.go` blob that breaks automated dependency updates.
3. **High-N scaling ceiling**: Running 1 OS process per node over Unix domain sockets in lock-step virtual time hits file-descriptor limits (`ulimit -n`) and kernel context-switch overhead at 200–1,000 nodes.
4. **Fragmentation with Nexus**: `openthread/tests/nexus` and `openthread/ot-ns` currently maintain separate visualization schemas, separate radio models, and separate UIs.

OTNS v3 addresses all four via a ground-up **C++17** architecture with **layered convergence** toward Nexus.

---

## 2. Core Design Principles

1. **C++17, minimal dependencies**: OpenThread host-C++ style (`openthread/STYLE_GUIDE.md` naming & `.clang-format` enforced via `./script/make-pretty check`), CMake + Ninja build, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror -fno-exceptions -fno-rtti`, ASan/UBSan clean, zero Boost/gRPC/Protobuf/OpenSSL.
2. **Deterministic single-threaded simulation loop**: One discrete-event scheduler drives virtual time at 1 µs resolution with deterministic seeded PRNG (`SplitMix64` / `Xoshiro256**`) and deterministic ordered iteration (no unordered hash iteration affecting RF or scheduling outcomes).
3. **Hybrid node model (scales to 500–1,000+ nodes without losing external-process support)**:
   - **In-process multi-instance OpenThread nodes** (`OPENTHREAD_CONFIG_MULTIPLE_INSTANCE_ENABLE=1`) execute directly on the simulator event loop with zero IPC syscalls, zero FD explosion, and 10–100× faster execution.
   - **External OS-process nodes** (Matter Linux apps, real OTBR/RCPs, custom or older Thread binaries) attach to the exact same RF medium and event scheduler via the redesigned, versioned binary/TLV `ot-rfsim` v3 protocol over Unix or TCP sockets.
4. **Shared virtual radio & standalone RF library (`libotns_rf`)**:
   - `libotns_rf` has **zero OS, socket, or OpenThread dependencies** so it can be unit-tested in isolation and reused directly by `openthread/tests/nexus` in the future.
   - Uses spatial grid indexing so `MutualInterference` neighbor lookup scales cleanly past 500+ nodes without $O(N^2)$ scans on every transmission.
5. **Single HTTP/WebSocket JSON control & visualization surface**:
   - One embedded localhost HTTP/1.1 + RFC 6455 WebSocket server handles static assets, JSON-RPC control commands, rate-coalesced visualization diffs (30–60 Hz), and `.jsonl` replay streams.
   - Shared JSON Schema contract (`protocol/schema/viz_rpc_v1.schema.json`) allows the same Vite + TypeScript + batched Canvas 2D web visualizer to connect to either OTNS v3 or a future Nexus WebSocket observer.
6. **Security by default**:
   - Loopback-only binding (`127.0.0.1`) by default, strict WebSocket `Origin` checks, canonicalized `--output-dir` containment on all file outputs (PCAP, KPI, topology, replay), and zero committed generated web bundles.

---

## 3. Layered Module Architecture

| Target | Path | Dependencies | Purpose |
| :--- | :--- | :--- | :--- |
| `libotns_rf` | `src/rf/` | C++17 stdlib only | Deterministic PRNG, 3D spatial grid index, ITU/3GPP path loss, shadow & time-variant fading, IEEE 802.15.4 SINR/BER error model, `Ideal` and `MutualInterference` radio models. |
| `libotns_protocol` | `src/protocol/` | C++17 stdlib only | Versioned `ot-rfsim` v3 binary/TLV encoder & bounds-checked decoder (shared with external `ot-rfsim` v3 nodes). |
| `libotns` | `src/core/`, `src/nodes/` | `libotns_rf`, `libotns_protocol`, `openthread-ftd`, `openthread-cli-ftd`, `nlohmann/json` | Discrete-event scheduler, `SimNode` abstraction, `InProcNode` (multi-instance OT FTD/MTD/SED/BR), `RfSimNode` (external socket bridge), PCAP/WPAN-TAP writer, 802.15.4 dissector, KPI, TREL/infra-if/host bridge, JSON topology loader. |
| `otns` | `src/server/`, `src/cli/` | `libotns` | Embedded POSIX `poll`/`epoll`/`kqueue` HTTP/WS server, JSON-RPC control plane, coalesced `VizPublisher`, JSONL recorder/replayer, interactive CLI REPL. |
| `ot-rfsim` v3 | `ot-rfsim/` | `openthread`, `libotns_protocol` | Clean C/C++ OpenThread platform for external binaries and Matter library mode. |
| `web` | `web/` | Vite, TypeScript | Batched HTML5 Canvas 2D topology visualizer (with LOD culling for 500–1,000 nodes), Stats view, Replay player. Optional at build time (`-DOTNS_BUILD_WEB=OFF` needs zero Node.js). |
| `python/otns` | `python/` | Python 3.10+ | Typed async/sync Python SDK over WS/Unix JSON-RPC + `pytest` integration & scale test suite. |

---

## 4. Roadmap

- **Milestone 1 (Current)**: `v3` branch & RFC docs, CMake C++17 build with ASan/UBSan, `libotns_rf` (PRNG, spatial index, Ideal & MutualInterference), `libotns_protocol` binary/TLV codec, `libotns` event scheduler + in-process multi-instance OpenThread nodes, multi-node formation/ping tests, and 100-node scale benchmark.
- **Milestone 2**: `ot-rfsim` v3 platform (standalone + Matter library mode), `RfSimNode` socket server, real-time pacing, TREL/infra-if/host networking, PCAP/WPAN-TAP, KPI, JSON topologies.
- **Milestone 3**: Embedded HTTP/1.1 + WebSocket server, JSON-RPC control plane, coalesced `VizPublisher`, JSONL replay, interactive CLI, Vite + TS Canvas 2D web visualizer, Python SDK + `pytest` suite.
- **Milestone 4**: Cross-platform CI (Linux x86_64/arm64, macOS Apple Silicon, ASan/UBSan), Docker playground, Matter & OTBR smoke tests, Nexus WebSocket observer PR in `openthread`, community cutover.
- **Phase 2 (Post-MVP)**: Energy profiling & charts, optional YAML topology loader, legacy OT v1.1–1.3 build matrix.
