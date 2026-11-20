#!/usr/bin/env python3
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
"""Generates a build-directory C++ file embedding static web UI assets."""

import argparse
import os
from pathlib import Path

MIME_MAP = {
    ".html": "text/html; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".mjs": "application/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
}

FALLBACK_HTML = b"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>OTNS v3 (Headless Build)</title>
<style>
  body { font-family: system-ui, sans-serif; background: #0f172a; color: #e2e8f0; margin: 2rem; }
  code { background: #1e293b; padding: 0.15rem 0.4rem; border-radius: 4px; }
</style>
</head>
<body>
  <h1>OTNS v3 Simulator Server</h1>
  <p>This binary was built with <code>-DOTNS_BUILD_WEB=OFF</code> (headless mode).</p>
  <p>WebSocket &amp; JSON-RPC endpoint is live at <code>/ws</code>. Build with <code>-DOTNS_BUILD_WEB=ON</code> or pass <code>--web-dir web/dist</code> to serve the interactive visualizer.</p>
</body>
</html>
"""


def guess_mime(path: Path) -> str:
    return MIME_MAP.get(path.suffix.lower(), "application/octet-stream")


def format_bytes_array(data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        lines.append("    " + ", ".join(f"0x{b:02x}u" for b in chunk) + ",")
    return "\n".join(lines) if lines else "    0x00u,"


def main() -> None:
    parser = argparse.ArgumentParser(description="Embed web/dist assets into C++")
    parser.add_argument("--dist", type=Path, default=None, help="Path to built web/dist directory")
    parser.add_argument("--out", type=Path, required=True, help="Output .cpp path in build dir")
    args = parser.parse_args()

    assets = []
    if args.dist is not None and args.dist.is_dir():
        for root, dirs, files in os.walk(args.dist):
            dirs.sort()
            for fname in sorted(files):
                fpath = Path(root) / fname
                rel = "/" + fpath.relative_to(args.dist).as_posix()
                assets.append((rel, guess_mime(fpath), fpath.read_bytes()))

    if not assets:
        assets.append(("/index.html", "text/html; charset=utf-8", FALLBACK_HTML))

    args.out.parent.mkdir(parents=True, exist_ok=True)
    parts = [
        "// Auto-generated at CMake build time by script/embed_web_assets.py. Do not commit.\n",
        '#include "server/embedded_assets.hpp"\n\n',
        "#include <cstring>\n\n",
        "namespace otns {\nnamespace Server {\nnamespace {\n\n",
    ]

    for idx, (_url, _mime, raw) in enumerate(assets):
        parts.append(f"static const uint8_t kAssetData_{idx}[] = {{\n")
        parts.append(format_bytes_array(raw))
        parts.append("\n};\n\n")

    parts.append("static const EmbeddedAsset kEmbeddedAssets[] = {\n")
    for idx, (url, mime, raw) in enumerate(assets):
        parts.append(f'    {{"{url}", "{mime}", kAssetData_{idx}, {len(raw)}u}},\n')
    parts.append("};\n\n} // namespace\n\n")

    parts.append(
        "const EmbeddedAsset *FindEmbeddedAsset(const std::string &aPath)\n"
        "{\n"
        "    for (const auto &asset : kEmbeddedAssets)\n"
        "    {\n"
        "        if (aPath == asset.mPath)\n"
        "        {\n"
        "            return &asset;\n"
        "        }\n"
        "    }\n"
        "    return nullptr;\n"
        "}\n\n"
        "} // namespace Server\n} // namespace otns\n"
    )

    content = "".join(parts)
    if args.out.exists() and args.out.read_text(encoding="utf-8") == content:
        return
    args.out.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
