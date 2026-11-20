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
"""Typed data models for the OTNS v3 / Nexus visualization & control protocol."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class NodeInfo:
    node_id: int
    x: float
    y: float
    z: float
    role: str
    rloc16: int
    ext_addr: str
    partition_id: int
    parent_id: int
    radio_range: float
    failed: bool
    kind: str
    label: str = ""

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> NodeInfo:
        return cls(
            node_id=int(data["id"]),
            x=float(data.get("x", 0.0)),
            y=float(data.get("y", 0.0)),
            z=float(data.get("z", 0.0)),
            role=str(data.get("role", "disabled")),
            rloc16=int(data.get("rloc16", 0xFFFE)),
            ext_addr=str(data.get("ext_addr", "")),
            partition_id=int(data.get("partition_id", 0)),
            parent_id=int(data.get("parent_id", 0)),
            radio_range=float(data.get("radio_range", 160.0)),
            failed=bool(data.get("failed", False)),
            kind=str(data.get("kind", "inproc")),
            label=str(data.get("label", "")),
        )


@dataclass(frozen=True)
class SimStats:
    total: int
    leaders: int
    routers: int
    children: int
    detached: int
    disabled: int
    partitions: int

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> SimStats:
        return cls(
            total=int(data.get("total", 0)),
            leaders=int(data.get("leaders", 0)),
            routers=int(data.get("routers", 0)),
            children=int(data.get("children", 0)),
            detached=int(data.get("detached", 0)),
            disabled=int(data.get("disabled", 0)),
            partitions=int(data.get("partitions", 0)),
        )


@dataclass(frozen=True)
class SimInfo:
    engine: str
    version: int
    ts_us: int
    speed: float
    paused: bool
    node_count: int
    spawn_enabled: bool = False
    rfsim_endpoint: str = ""

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> SimInfo:
        return cls(
            engine=str(data.get("engine", "")),
            version=int(data.get("version", 1)),
            ts_us=int(data.get("ts_us", 0)),
            speed=float(data.get("speed", 0.0)),
            paused=bool(data.get("paused", True)),
            node_count=int(data.get("node_count", 0)),
            spawn_enabled=bool(data.get("spawn_enabled", False)),
            rfsim_endpoint=str(data.get("rfsim_endpoint", "")),
        )
