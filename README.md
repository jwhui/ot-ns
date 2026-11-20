# OpenThread Network Simulator (OTNS v3)

> **Status**: Active ground-up v3 development on the `v3` branch. For legacy Go-based OTNS v2, see the `main` branch.

OTNS v3 is a C++17 discrete-event Thread network simulator and visualizer designed for:

- **High scale (200–1,000+ nodes)** via in-process multi-instance OpenThread nodes with zero IPC overhead.
- **Heterogeneous & external nodes** (Matter Linux apps, real OTBR/RCPs, custom/legacy Thread binaries) via a versioned binary/TLV `ot-rfsim` v3 Unix/TCP socket bridge running on the same RF medium.
- **High-fidelity RF physics** via a standalone, zero-OS-dependency C++17 RF library (`libotns_rf`) supporting `Ideal` and `MutualInterference` (3GPP/ITU path loss, shadow & time-variant fading, SINR/BER, CCA/energy scan, spatial neighbor indexing, and deterministic PRNG).
- **Modern, low-maintenance visualization** via an embedded localhost HTTP/1.1 + WebSocket JSON server and a Vite + TypeScript + batched HTML5 Canvas 2D frontend shared with `openthread/tests/nexus` — zero Go, zero Webpack 4, zero gRPC/Protobuf/proxy chains, and zero committed generated bundles.

## Quick Start (Headless C++17 Build & Tests)

Prerequisites: CMake ≥ 3.24, Ninja, C++17 compiler (GCC or Clang). Headless builds require **zero Node.js**.

```bash
cmake -S . -B build -G Ninja -DOTNS_ENABLE_SANITIZERS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Code Style & Formatting

OTNS v3 follows OpenThread's host-C++ style (`openthread/STYLE_GUIDE.md` naming, Allman braces, `-fno-exceptions -fno-rtti`, matching `ot-br-posix` and `tests/nexus`) using the repository's `.clang-format` rules:

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
