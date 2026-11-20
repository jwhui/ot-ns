# Copyright (c) 2026, The OTNS Authors.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 3. Neither the name of the copyright holder nor the
#    names of its contributors may be used to endorse or promote products
#    derived from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

# Multi-stage OTNS v3 Playground image:
#   - Compiles C++17 otns daemon + embedded Vite/TypeScript Canvas 2D web UI
#   - Compiles standalone external ot-cli-ftd-rfsim binary
#   - Ships a minimal non-root runtime image with Python 3 SDK & examples

FROM node:22-bookworm-slim AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        ninja-build \
        python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Install web devDependencies first for layer caching
COPY web/package.json web/package-lock.json web/
RUN npm --prefix web ci

# Copy remaining source tree (including openthread submodule)
COPY . .

RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DOTNS_BUILD_WEB=ON \
        -DOTNS_BUILD_TESTS=OFF \
    && cmake --build /build --target otns ot-cli-ftd-rfsim \
    && strip /build/otns /build/ot-rfsim/ot-cli-ftd-rfsim

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        python3 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --shell /bin/bash otns \
    && mkdir -p /data /opt/otns \
    && chown -R otns:otns /data /opt/otns

COPY --from=builder /build/otns /usr/local/bin/otns
COPY --from=builder /build/ot-rfsim/ot-cli-ftd-rfsim /usr/local/bin/ot-cli-ftd-rfsim
COPY --from=builder /src/python /opt/otns/python

ENV PYTHONPATH=/opt/otns/python \
    OTNS_BIN=/usr/local/bin/otns \
    OTNS_RFSIM_BIN=/usr/local/bin/ot-cli-ftd-rfsim

USER otns
WORKDIR /data

EXPOSE 8999

ENTRYPOINT ["/usr/local/bin/otns"]
CMD ["--bind", "0.0.0.0", "--port", "8999", "--output-dir", "/data"]
