# OpenThread Network Simulator (OTNS v3)

> **Status**: Ground-up C++17 OTNS v3 implementation on the `v3` branch. For legacy Go-based OTNS v2, see the `main` branch.

OTNS v3 is a deterministic C++17 discrete-event Thread 1.4 network simulator and visualizer designed for:

- **High scale (200–1,000+ nodes)** via in-process multi-instance OpenThread nodes with zero IPC overhead (~85× real time for 200 nodes on a laptop CPU).
- **Heterogeneous & external nodes** (Matter Linux apps, real OTBR/RCPs, standalone Thread binaries) via a versioned binary/TLV `ot-rfsim` v3 Unix/TCP socket bridge sharing the exact same RF medium and virtual clock.
- **High-fidelity RF physics** via a standalone, zero-OS-dependency C++17 RF library (`libotns_rf`) supporting `Ideal`, `IdealRssi`, `MutualInterference`, `Midisc`, and `Outdoor` (3GPP/ITU path loss, shadow & time-variant fading, IEEE 802.15.4 SINR/BER, CCA/energy scan, 3D spatial neighbor indexing, and deterministic PRNG).
- **Modern, low-maintenance visualization** via an embedded localhost HTTP/1.1 + RFC 6455 WebSocket JSON server and a Vite + TypeScript + batched HTML5 Canvas 2D frontend shared with `openthread/tests/nexus` — zero Go, zero Webpack 4, zero gRPC/Protobuf/proxy chains, and zero committed generated bundles.

## Quick Start

### 1. Headless C++17 Build & Tests (Zero Node.js)

Prerequisites: CMake ≥ 3.24, Ninja, C++17 compiler (GCC or Clang), Python 3.

```bash
cmake -S . -B build -G Ninja -DOTNS_ENABLE_SANITIZERS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### 2. Full Release Build with Embedded Web Visualizer

Requires Node.js ≥ 20 (zero runtime JS dependencies; Vite + TypeScript bundle is compiled and embedded at CMake build time and never committed to Git):

```bash
npm --prefix web ci
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DOTNS_BUILD_WEB=ON
cmake --build build-release
./build-release/otns --port 8999
```

Open `http://127.0.0.1:8999/` in your browser. Use either the browser toolbar / Node Inspector or the interactive terminal REPL (`help` for all commands).

Common `otns` daemon flags:

```text
--port <u16>          HTTP/WebSocket port (default: 8999, 0 = ephemeral)
--bind <ip>           Bind address (default: 127.0.0.1)
--rpc-socket <path>   Optional Unix domain socket for JSON-RPC
--rfsim-socket <path> Optional Unix socket for external ot-rfsim v3 nodes
--rfsim-tcp <port>    Optional TCP loopback port for external ot-rfsim v3 nodes
--output-dir <dir>    Contained directory for PCAP/KPI/topology/replay outputs
--topology <file>     Load JSON topology file at startup
--replay <file>       Stream a .jsonl replay file to connected Web UI clients
--seed <u64>          Deterministic PRNG seed (default: 1)
--rf-model <preset>   Ideal|IdealRssi|MutualInterference|Midisc|Outdoor
--speed <float>       Simulation speed (0 = max, 1 = real-time; default: 1.0)
--headless            Disable interactive stdin REPL
```

### 3. Hybrid External `ot-rfsim` v3 Nodes (Standalone FTD, Real OTBR/RCP & Library Mode)

Launch the daemon with an `ot-rfsim` v3 listener and attach external standalone FTD processes:

```bash
./build-release/otns --port 8999 --rfsim-socket /tmp/otns-rfsim.sock
# In another terminal:
./build-release/ot-rfsim/ot-cli-ftd-rfsim --socket /tmp/otns-rfsim.sock --node-id 2
```

Attach a **real OpenThread POSIX host (`ot-cli`) or `otbr-agent`** over Spinel/HDLC via `ot-rcp-rfsim` (with `otns` running at real-time `--speed 1.0`):

```bash
./build-release/otns --port 8999 --speed 1.0 --rfsim-socket /tmp/otns-rfsim.sock
# In another terminal (otbr-agent or OpenThread POSIX ot-cli):
otbr-agent -I wpan0 -B eth0 \
  "spinel+hdlc+forkpty://$(pwd)/build-release/ot-rfsim/ot-rcp-rfsim?forkpty-arg=--socket&forkpty-arg=/tmp/otns-rfsim.sock&forkpty-arg=--node-id&forkpty-arg=2"
```

External projects (such as Matter or OTBR) can build `ot-rfsim/` standalone and link `openthread-rfsim-v3` (FTD/MTD with optional `otPlatTrel*` and `otPlatInfraIf*` virtual AIL support) or `openthread-rfsim-v3-rcp` (RCP) using the C API in `ot-rfsim/include/ot_rfsim_v3.h`:

```bash
cmake -S ot-rfsim -B build-rfsim -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DOT_RFSIM_TREL=ON -DOT_RFSIM_BORDER_ROUTING=ON
cmake --build build-rfsim
```

To orchestrate externally built Matter apps or custom `ot-rfsim` v3 binaries directly from the Python SDK (`cli.spawn_node(...)`), JSON-RPC (`node.spawn`), or REPL (`spawn`), opt in with an explicit executable allowlist:

```bash
./build-release/otns --port 8999 \
  --rfsim-socket /tmp/otns-rfsim.sock \
  --allow-spawn ./build-release/ot-rfsim
```

`otSysInit()` accepts `--endpoint`/`--socket`/`--node-id`/`--label`, Matter's `--thread-args=<id> --thread-args=<socket>` convention, and `OTNS_RFSIM_ENDPOINT` environment fallback, and integrates with POSIX `select()` loops via `otSysUpdateEvents()`/`otSysProcessEvents()`.

### 4. Python 3 SDK & Examples

Zero external runtime dependencies (Python ≥ 3.10 stdlib):

```bash
OTNS_BIN=./build-release/otns python3 -m unittest discover -s python/tests -v
OTNS_BIN=./build-release/otns python3 python/examples/simple_mesh.py
OTNS_BIN=./build-release/otns python3 python/examples/ping_and_pcap.py
OTNS_BIN=./build-release/otns python3 python/examples/scale_200.py
```

### 5. Docker Playground

```bash
docker build -f docker/Dockerfile -t openthread/otns-v3 .
docker run --rm -it -p 127.0.0.1:8999:8999 openthread/otns-v3
```

See [docker/README.md](docker/README.md) for persistent output volumes and running Python scripts in the container.

## Code Style & Formatting

OTNS v3 follows OpenThread's host-C++ style (`openthread/STYLE_GUIDE.md` naming, Allman braces, `-fno-exceptions -fno-rtti`, matching `ot-br-posix` and `tests/nexus`) enforced via `.clang-format`:

```bash
./script/make-pretty       # Format C/C++ sources in place (clang-format v19)
./script/make-pretty check # Verify formatting without modifying files
```

## Architecture & Specifications

- [Architecture RFC](docs/architecture.md)
- [`ot-rfsim` v3 Binary/TLV Wire Protocol](docs/rfsim_v3_protocol.md)
- [Shared Visualization & Control Protocol (OTNS + Nexus)](docs/viz_rpc_protocol.md)
- [JSON Schema Contract](protocol/schema/viz_rpc_v1.schema.json)

## License

OTNS is released under the [BSD 3-Clause License](LICENSE).
