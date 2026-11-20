#!/usr/bin/env bash
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
# Native (Docker-free) Linux builder for out-of-tree Matter-over-Thread
# integration binaries against OTNS v3 ot-rfsim:
#   - all-devices-app (linux-<arch>-all-devices-no-ble-no-wifi-openthread-endpoint-otns)
#   - chip-tool       (linux-<arch>-chip-tool)
#   - otbr-agent + ot-ctl (built with -DOTBR_MDNS=openthread, no Avahi/D-Bus needed)
#
# Symlinks third_party/ot-ns/repo to this OTNS v3 checkout so incremental ninja
# rebuilds take seconds after editing ot-rfsim/ or src/protocol/.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OTNS_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

WORK_DIR="${OTNS_MATTER_DIR:-${HOME}/.cache/otns-matter}"
MATTER_REPO="https://github.com/project-chip/connectedhomeip.git"
MATTER_REF="master"
MATTER_DIR=""
OUT_BIN_DIR=""
BUILD_JOBS=""
INSTALL_DEPS=0
SKIP_CHIP_TOOL=0
SKIP_OTBR=0
SKIP_BOOTSTRAP=0
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: script/build-matter-deps.sh [options]

Builds upstream project-chip/connectedhomeip Matter-over-Thread binaries natively
on Linux (x86_64 or arm64) linked against this OTNS v3 ot-rfsim checkout — no Docker.

Options:
  --work-dir <dir>       Cache/workspace directory (default: $OTNS_MATTER_DIR or ~/.cache/otns-matter)
  --matter-dir <dir>     Use an existing connectedhomeip checkout instead of cloning into <work-dir>/connectedhomeip
  --matter-repo <url>    Git URL to clone connectedhomeip from (default: https://github.com/project-chip/connectedhomeip.git)
  --matter-ref <ref>     Branch, tag, or commit SHA to clone (default: master)
  --out-bin-dir <dir>    Directory to place built binaries (default: <work-dir>/bin)
  --jobs <n>             Ninja / CMake parallel job count (default: nproc)
  --install-deps         Install Debian/Ubuntu build prerequisites via sudo apt-get
  --skip-chip-tool       Skip building chip-tool (if already built)
  --skip-otbr            Skip building otbr-agent + ot-ctl (if already built)
  --skip-bootstrap       Skip scripts/bootstrap.sh (if Pigweed env already exists)
  --dry-run              Print planned build targets and paths, then exit
  -h, --help             Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --work-dir)       WORK_DIR="$2"; shift 2 ;;
        --matter-dir)     MATTER_DIR="$2"; shift 2 ;;
        --matter-repo)    MATTER_REPO="$2"; shift 2 ;;
        --matter-ref)     MATTER_REF="$2"; shift 2 ;;
        --out-bin-dir)    OUT_BIN_DIR="$2"; shift 2 ;;
        --jobs)           BUILD_JOBS="$2"; shift 2 ;;
        --install-deps)   INSTALL_DEPS=1; shift ;;
        --skip-chip-tool) SKIP_CHIP_TOOL=1; shift ;;
        --skip-otbr)      SKIP_OTBR=1; shift ;;
        --skip-bootstrap) SKIP_BOOTSTRAP=1; shift ;;
        --dry-run)        DRY_RUN=1; shift ;;
        -h|--help)        usage; exit 0 ;;
        *)                echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Error: upstream connectedhomeip chip_enable_otns is currently Linux-only (detected $(uname -s))." >&2
    echo "Run this script on a Linux host or an arm64/x86_64 Linux VM." >&2
    exit 1
fi

case "$(uname -m)" in
    x86_64)                HOST_ARCH="x64" ;;
    aarch64|arm64)         HOST_ARCH="arm64" ;;
    *)
        echo "Error: unsupported Linux architecture: $(uname -m)" >&2
        exit 1
        ;;
esac

if [[ -z "${BUILD_JOBS}" ]]; then
    BUILD_JOBS="$(nproc 2>/dev/null || echo 4)"
fi

mkdir -p "${WORK_DIR}"
WORK_DIR="$(cd "${WORK_DIR}" && pwd)"

if [[ -z "${MATTER_DIR}" ]]; then
    MATTER_DIR="${WORK_DIR}/connectedhomeip"
fi
if [[ -z "${OUT_BIN_DIR}" ]]; then
    OUT_BIN_DIR="${WORK_DIR}/bin"
fi

ALL_DEVICES_TARGET="linux-${HOST_ARCH}-all-devices-no-ble-no-wifi-openthread-endpoint-otns"
CHIP_TOOL_TARGET="linux-${HOST_ARCH}-chip-tool-no-interactive-no-ble-no-wifi-no-thread-mbedtls"

echo "OTNS root:          ${OTNS_ROOT}"
echo "Matter checkout:    ${MATTER_DIR} (ref=${MATTER_REF})"
echo "Output bin dir:     ${OUT_BIN_DIR}"
echo "All-devices target: ${ALL_DEVICES_TARGET}"
echo "chip-tool target:   ${CHIP_TOOL_TARGET}"
echo "Parallel jobs:      ${BUILD_JOBS}"

if [[ "${DRY_RUN}" -eq 1 ]]; then
    exit 0
fi

if [[ "${INSTALL_DEPS}" -eq 1 ]]; then
    sudo apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ccache \
        cmake \
        curl \
        g++ \
        gcc \
        git \
        iproute2 \
        iputils-ping \
        libssl-dev \
        ninja-build \
        pkg-config \
        python3-dev \
        python3-pip \
        python3-venv \
        unzip
fi

# 1. Clone or reuse connectedhomeip checkout.
if [[ ! -d "${MATTER_DIR}/.git" ]]; then
    echo "=== Cloning connectedhomeip (${MATTER_REF}) ==="
    git clone --depth 1 --branch "${MATTER_REF}" "${MATTER_REPO}" "${MATTER_DIR}" \
        || (git clone --depth 1 "${MATTER_REPO}" "${MATTER_DIR}" \
            && git -C "${MATTER_DIR}" fetch --depth 1 origin "${MATTER_REF}" \
            && git -C "${MATTER_DIR}" checkout FETCH_HEAD)
fi

cd "${MATTER_DIR}"

# 2. Ensure Linux submodules and Pigweed/GN environment are initialized.
if [[ ! -f "${MATTER_DIR}/third_party/openthread/repo/BUILD.gn" ]]; then
    echo "=== Checking out Linux submodules ==="
    ./scripts/checkout_submodules.py --shallow --platform linux
fi

if [[ "${SKIP_BOOTSTRAP}" -eq 0 && ! -f "${MATTER_DIR}/.environment/activate.sh" ]]; then
    echo "=== Bootstrapping Pigweed/GN build environment ==="
    bash scripts/bootstrap.sh
fi

# 3. Point Matter's OTNS platform directly at this OTNS v3 checkout via symlink.
echo "=== Wiring OTNS v3 ot-rfsim into Matter ==="
rm -rf "${MATTER_DIR}/third_party/ot-ns/repo"
ln -s "${OTNS_ROOT}" "${MATTER_DIR}/third_party/ot-ns/repo"

cat > "${MATTER_DIR}/third_party/openthread/platforms/otns/BUILD.gn" <<'EOF'
# Delegated to OTNS v3 ot-rfsim/BUILD.gn
import("//build_overrides/chip.gni")

group("otns") {
  public_deps = [ "${chip_root}/third_party/ot-ns/repo/ot-rfsim:otns" ]
}
EOF

# Upstream src/platform/Linux/BUILD.gn evaluates pkg_config("avahi_client_config")
# whenever chip_mdns == "platform", even though openthread-endpoint builds use
# OpenThread's DNS-SD implementation and never reference avahi_client_config.
python3 - "${MATTER_DIR}/src/platform/Linux/BUILD.gn" <<'PY'
import sys
from pathlib import Path

p = Path(sys.argv[1])
text = p.read_text(encoding="utf-8")
old = 'if (chip_mdns == "platform") {\n  pkg_config("avahi_client_config") {'
new = (
    'if (chip_mdns == "platform" &&\n'
    '    !chip_system_config_use_openthread_inet_endpoints) {\n'
    '  pkg_config("avahi_client_config") {'
)
if old in text:
    p.write_text(text.replace(old, new, 1), encoding="utf-8")
PY

mkdir -p "${OUT_BIN_DIR}"

LAUNCHER_ARG=""
if command -v ccache >/dev/null 2>&1; then
    LAUNCHER_ARG="--pw-command-launcher=ccache"
fi

# 4. Build Matter all-devices-app linked directly to OTNS v3 ot-rfsim.
echo "=== Building ${ALL_DEVICES_TARGET} ==="
./scripts/run_in_build_env.sh \
    "./scripts/build/build_examples.py \
        ${LAUNCHER_ARG} \
        --ninja-jobs ${BUILD_JOBS} \
        --target ${ALL_DEVICES_TARGET} \
        build"
cp "${MATTER_DIR}/out/${ALL_DEVICES_TARGET}/all-devices-app" "${OUT_BIN_DIR}/all-devices-app"

# 5. Build chip-tool controller.
if [[ "${SKIP_CHIP_TOOL}" -eq 0 ]]; then
    echo "=== Building ${CHIP_TOOL_TARGET} ==="
    ./scripts/run_in_build_env.sh \
        "./scripts/build/build_examples.py \
            ${LAUNCHER_ARG} \
            --ninja-jobs ${BUILD_JOBS} \
            --target ${CHIP_TOOL_TARGET} \
            build"
    cp "${MATTER_DIR}/out/${CHIP_TOOL_TARGET}/chip-tool" "${OUT_BIN_DIR}/chip-tool"
fi

# 6. Build otbr-agent + ot-ctl from connectedhomeip's pinned ot-br-posix submodule.
if [[ "${SKIP_OTBR}" -eq 0 ]]; then
    echo "=== Building otbr-agent + ot-ctl ==="
    (
        cd "${MATTER_DIR}/third_party/ot-br-posix/repo"
        if [[ ! -f third_party/openthread/repo/CMakeLists.txt ]]; then
            git submodule update --init --recursive --depth 1
        fi
        CMAKE_BUILD_PARALLEL_LEVEL="${BUILD_JOBS}" PLATFORM=Linux ./script/cmake-build \
            -DBUILD_TESTING=OFF \
            -DCMAKE_BUILD_TYPE=Release \
            -DOTBR_BORDER_ROUTING=ON \
            -DOTBR_MDNS=openthread \
            -DOTBR_REST=OFF \
            -DOTBR_WEB=OFF \
            -DOTBR_VENDOR_NAME=OTNSv3 \
            -DOTBR_PRODUCT_NAME=OTNSv3BorderRouter \
            -DOT_POSIX_SETTINGS_PATH='"/tmp/otbr"' \
            -DOT_FIREWALL=OFF \
            -DOT_LOG_LEVEL=INFO \
            -DOT_TREL=OFF
        cp build/otbr/src/agent/otbr-agent "${OUT_BIN_DIR}/otbr-agent"
        cp build/otbr/third_party/openthread/repo/src/posix/ot-ctl "${OUT_BIN_DIR}/ot-ctl"
    )
fi

echo ""
echo "Built Matter integration binaries in ${OUT_BIN_DIR}:"
ls -lh "${OUT_BIN_DIR}"
echo ""
echo "Run the end-to-end Matter-over-Thread smoke test in an isolated Linux netns (no Docker):"
echo "  # Rootless (when unprivileged user namespaces are enabled):"
echo "  unshare -Urmn -- python3 ${OTNS_ROOT}/script/test-matter-smoke.py \\"
echo "    --otns-bin ${OTNS_ROOT}/build-release/otns \\"
echo "    --rcp-bin ${OTNS_ROOT}/build-release/ot-rfsim/ot-rcp-rfsim \\"
echo "    --matter-app ${OUT_BIN_DIR}/all-devices-app \\"
echo "    --chip-tool ${OUT_BIN_DIR}/chip-tool \\"
echo "    --otbr-agent ${OUT_BIN_DIR}/otbr-agent \\"
echo "    --ot-ctl ${OUT_BIN_DIR}/ot-ctl"
echo "  # Or with sudo on hosts where AppArmor restricts unprivileged userns (e.g., Ubuntu 24.04):"
echo "  sudo unshare --net -- python3 ${OTNS_ROOT}/script/test-matter-smoke.py"
