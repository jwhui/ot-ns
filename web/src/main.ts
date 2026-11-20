import { TopologyRenderer } from "./canvas_renderer";
import { OtnsWsClient } from "./client";
import type { LinkState, NodeState, NodeStats, VizEvent } from "./protocol";
import { ReplayPlayer } from "./replay_player";
import { StatsChart } from "./stats_chart";

function linkKey(src: number, dst: number, kind: string): string {
  return `${src}:${dst}:${kind}`;
}

function formatClock(tsUs: number): string {
  const totalMs = Math.floor(tsUs / 1000);
  const mins = Math.floor(totalMs / 60000);
  const secs = Math.floor((totalMs % 60000) / 1000);
  const ms = totalMs % 1000;
  return `${String(mins).padStart(2, "0")}:${String(secs).padStart(2, "0")}.${String(ms).padStart(3, "0")}`;
}

function byId<T extends HTMLElement>(id: string): T {
  const el = document.getElementById(id);
  if (!el) {
    throw new Error(`Missing element #${id}`);
  }
  return el as T;
}

const connBadge = byId<HTMLSpanElement>("conn-badge");
const engineLabel = byId<HTMLSpanElement>("engine-label");
const clockLabel = byId<HTMLSpanElement>("clock-label");
const btnPlay = byId<HTMLButtonElement>("btn-play");
const btnStep1s = byId<HTMLButtonElement>("btn-step-1s");
const btnStep10s = byId<HTMLButtonElement>("btn-step-10s");
const speedSelect = byId<HTMLSelectElement>("speed-select");
const addTypeSelect = byId<HTMLSelectElement>("add-type-select");
const btnAddNode = byId<HTMLButtonElement>("btn-add-node");
const btnFit = byId<HTMLButtonElement>("btn-fit");
const btnRanges = byId<HTMLButtonElement>("btn-ranges");
const replayInput = byId<HTMLInputElement>("replay-file-input");

const replayBar = byId<HTMLDivElement>("replay-bar");
const btnReplayToggle = byId<HTMLButtonElement>("btn-replay-toggle");
const replayScrubber = byId<HTMLInputElement>("replay-scrubber");
const replayTime = byId<HTMLSpanElement>("replay-time");
const btnReplayExit = byId<HTMLButtonElement>("btn-replay-exit");

const stTotal = byId<HTMLSpanElement>("st-total");
const stLeaders = byId<HTMLSpanElement>("st-leaders");
const stRouters = byId<HTMLSpanElement>("st-routers");
const stChildren = byId<HTMLSpanElement>("st-children");
const stDetached = byId<HTMLSpanElement>("st-detached");
const stPartitions = byId<HTMLSpanElement>("st-partitions");

const nodeDetails = byId<HTMLDivElement>("node-details");
const nodeActions = byId<HTMLDivElement>("node-actions");
const btnNodeToggle = byId<HTMLButtonElement>("btn-node-toggle");
const btnNodeRemove = byId<HTMLButtonElement>("btn-node-remove");
const cliForm = byId<HTMLFormElement>("cli-form");
const cliInput = byId<HTMLInputElement>("cli-input");
const cliOutput = byId<HTMLPreElement>("cli-output");

const nodes = new Map<number, NodeState>();
const links = new Map<string, LinkState>();
let selectedNodeId: number | null = null;
let paused = true;
let currentSpeed = 1.0;
let replayMode = false;
let hasFittedOnce = false;

const statsChart = new StatsChart(byId<HTMLCanvasElement>("stats-canvas"));
const replayPlayer = new ReplayPlayer();

const wsProto = window.location.protocol === "https:" ? "wss" : "ws";
const wsUrl = `${wsProto}://${window.location.host}/ws`;

function updateStatsDom(stats: NodeStats, tsUs: number): void {
  stTotal.textContent = String(stats.total);
  stLeaders.textContent = String(stats.leaders);
  stRouters.textContent = String(stats.routers);
  stChildren.textContent = String(stats.children);
  stDetached.textContent = String(stats.detached);
  stPartitions.textContent = String(stats.partitions);
  statsChart.push(tsUs, stats);
}

function updateInspector(): void {
  if (selectedNodeId === null || !nodes.has(selectedNodeId)) {
    nodeDetails.textContent = "Click a node on the canvas to inspect, drag to move, or run OpenThread CLI commands.";
    nodeActions.classList.add("hidden");
    cliForm.classList.add("hidden");
    return;
  }
  const n = nodes.get(selectedNodeId)!;
  const rlocHex = n.rloc16 !== undefined ? `0x${n.rloc16.toString(16).padStart(4, "0")}` : "—";
  const partHex = n.partition_id !== undefined ? `0x${n.partition_id.toString(16).padStart(8, "0")}` : "—";
  const tagPart = n.label ? ` · ${n.label}` : "";
  nodeDetails.textContent =
    `Node #${n.id} (${n.kind ?? "inproc"}${tagPart})\n` +
    (n.label ? `Label:     ${n.label}\n` : "") +
    `Role:      ${n.role}${n.failed ? " (DISABLED)" : ""}\n` +
    `RLOC16:    ${rlocHex}\n` +
    `Partition: ${partHex}\n` +
    `ExtAddr:   ${n.ext_addr ?? "—"}\n` +
    `Position:  (${n.x.toFixed(1)}, ${n.y.toFixed(1)}, ${(n.z ?? 0).toFixed(1)})`;

  btnNodeToggle.textContent = n.failed ? "Enable" : "Disable";
  if (!replayMode) {
    nodeActions.classList.remove("hidden");
    cliForm.classList.remove("hidden");
  }
}

const renderer = new TopologyRenderer(
  byId<HTMLCanvasElement>("topology-canvas"),
  (nodeId) => {
    selectedNodeId = nodeId;
    cliOutput.textContent = "";
    updateInspector();
  },
  (nodeId, x, y) => {
    if (!replayMode) {
      void client.call("node.move", { node_id: nodeId, x, y }).catch(() => undefined);
    }
  },
);
renderer.setState(nodes, links);

function syncSpeedSelect(speed: number): void {
  currentSpeed = speed;
  const target = String(speed);
  for (const opt of Array.from(speedSelect.options)) {
    if (opt.value === target) {
      speedSelect.value = target;
      return;
    }
  }
}

function applyEvent(ev: VizEvent): void {
  clockLabel.textContent = formatClock(ev.ts_us);

  if (ev.type === "event.snapshot") {
    const isFreshReset = ev.ts_us === 0 || ev.nodes.length === 0;
    nodes.clear();
    links.clear();
    if (isFreshReset) {
      statsChart.clear();
      renderer.clearPackets();
      selectedNodeId = null;
      renderer.setSelectedNode(null);
      cliOutput.textContent = "";
      hasFittedOnce = false;
    }
    for (const n of ev.nodes) {
      nodes.set(n.id, n);
    }
    for (const l of ev.links) {
      links.set(linkKey(l.src, l.dst, l.kind), l);
    }
    if (selectedNodeId !== null && !nodes.has(selectedNodeId)) {
      selectedNodeId = null;
      renderer.setSelectedNode(null);
      cliOutput.textContent = "";
    }
    engineLabel.textContent = `engine: ${ev.engine}${ev.rf_model ? ` (${ev.rf_model})` : ""}`;
    if (ev.speed !== undefined) {
      syncSpeedSelect(ev.speed);
    }
    if (ev.paused !== undefined) {
      paused = ev.paused;
      btnPlay.textContent = paused ? "Play" : "Pause";
    }
    if (ev.stats) {
      updateStatsDom(ev.stats, ev.ts_us);
    }
    if (!hasFittedOnce && nodes.size > 0) {
      renderer.fitToNodes();
      hasFittedOnce = true;
    }
    updateInspector();
    return;
  }

  if (ev.type === "event.batch_diff") {
    if (ev.remove_nodes) {
      for (const id of ev.remove_nodes) {
        nodes.delete(id);
      }
    }
    if (ev.upsert_nodes) {
      for (const n of ev.upsert_nodes) {
        nodes.set(n.id, n);
      }
    }
    if (ev.remove_links) {
      for (const l of ev.remove_links) {
        links.delete(linkKey(l.src, l.dst, l.kind));
      }
    }
    if (ev.upsert_links) {
      for (const l of ev.upsert_links) {
        links.set(linkKey(l.src, l.dst, l.kind), l);
      }
    }
    updateInspector();
    return;
  }

  if (ev.type === "event.packet") {
    renderer.addPacketAnim(ev);
    return;
  }

  if (ev.type === "event.stats") {
    updateStatsDom(ev.stats, ev.ts_us);
    return;
  }

  if (ev.type === "event.heartbeat") {
    paused = ev.paused;
    btnPlay.textContent = paused ? "Play" : "Pause";
    syncSpeedSelect(ev.speed);
  }
}

const client = new OtnsWsClient(
  wsUrl,
  (ev) => {
    if (!replayMode) {
      applyEvent(ev);
    }
  },
  (connected) => {
    if (replayMode) {
      return;
    }
    connBadge.textContent = connected ? "Live" : "Disconnected";
    connBadge.className = `badge ${connected ? "badge-online" : "badge-offline"}`;
  },
);
client.connect();

btnPlay.addEventListener("click", () => {
  if (replayMode) return;
  const method = paused ? "sim.resume" : "sim.pause";
  void client.call(method).catch(() => undefined);
});

btnStep1s.addEventListener("click", () => {
  if (replayMode) return;
  void client.call("sim.go", { duration_us: 1_000_000, speed: 0 }).catch(() => undefined);
});

btnStep10s.addEventListener("click", () => {
  if (replayMode) return;
  void client.call("sim.go", { duration_us: 10_000_000, speed: 0 }).catch(() => undefined);
});

speedSelect.addEventListener("change", () => {
  currentSpeed = Number(speedSelect.value);
  if (!replayMode) {
    void client.call("sim.set_speed", { speed: currentSpeed }).catch(() => undefined);
  }
});

btnAddNode.addEventListener("click", () => {
  if (replayMode) return;
  const angle = nodes.size * 0.9;
  const radius = 35 + Math.min(220, nodes.size * 14);
  const x = Math.round(Math.cos(angle) * radius);
  const y = Math.round(Math.sin(angle) * radius);
  void client.call("node.add", { type: addTypeSelect.value, x, y }).catch(() => undefined);
});

btnFit.addEventListener("click", () => renderer.fitToNodes());

btnRanges.addEventListener("click", () => {
  renderer.setShowRanges(!renderer.getShowRanges());
});

btnNodeToggle.addEventListener("click", () => {
  if (replayMode || selectedNodeId === null) return;
  const n = nodes.get(selectedNodeId);
  if (!n) return;
  void client.call("node.set_enabled", { node_id: n.id, enabled: Boolean(n.failed) }).catch(() => undefined);
});

btnNodeRemove.addEventListener("click", () => {
  if (replayMode || selectedNodeId === null) return;
  const id = selectedNodeId;
  selectedNodeId = null;
  renderer.setSelectedNode(null);
  void client.call("node.remove", { node_id: id }).catch(() => undefined);
});

cliForm.addEventListener("submit", (ev) => {
  ev.preventDefault();
  if (replayMode || selectedNodeId === null) return;
  const cmd = cliInput.value.trim();
  if (!cmd) return;
  cliInput.value = "";
  const id = selectedNodeId;
  cliOutput.textContent += `> ${cmd}\n`;
  void client
    .call("node.cli", { node_id: id, command: cmd })
    .then((res) => {
      const out = typeof res.output === "string" ? res.output : "";
      cliOutput.textContent += out;
      cliOutput.scrollTop = cliOutput.scrollHeight;
    })
    .catch((err: Error) => {
      cliOutput.textContent += `Error: ${err.message}\n`;
    });
});

replayInput.addEventListener("change", () => {
  const file = replayInput.files?.[0];
  if (!file) return;
  void file.text().then((text) => {
    const count = replayPlayer.loadJsonl(text);
    if (count === 0) return;
    replayMode = true;
    hasFittedOnce = false;
    connBadge.textContent = `Replay (${count} ev)`;
    connBadge.className = "badge badge-replay";
    replayBar.classList.remove("hidden");
    nodeActions.classList.add("hidden");
    cliForm.classList.add("hidden");
    replayPlayer.seekTo(0, applyEvent);
    replayPlayer.setPlaying(true);
    btnReplayToggle.textContent = "Pause";
  });
});

btnReplayToggle.addEventListener("click", () => {
  replayPlayer.setPlaying(!replayPlayer.isPlaying());
  btnReplayToggle.textContent = replayPlayer.isPlaying() ? "Pause" : "Play";
});

replayScrubber.addEventListener("input", () => {
  const frac = Number(replayScrubber.value) / 100;
  replayPlayer.seekTo(Math.round(frac * replayPlayer.getDurationUs()), applyEvent);
});

btnReplayExit.addEventListener("click", () => {
  replayMode = false;
  replayPlayer.setPlaying(false);
  replayBar.classList.add("hidden");
  client.disconnect();
  client.connect();
});

window.addEventListener("resize", () => renderer.resize());

function frame(nowMs: number): void {
  if (replayMode) {
    replayPlayer.tick(nowMs, currentSpeed, applyEvent);
    const dur = Math.max(1, replayPlayer.getDurationUs());
    const cur = replayPlayer.getCurrentTsUs();
    replayScrubber.value = String(Math.round((cur / dur) * 100));
    replayTime.textContent = `${(cur / 1e6).toFixed(1)}s / ${(dur / 1e6).toFixed(1)}s`;
    btnReplayToggle.textContent = replayPlayer.isPlaying() ? "Pause" : "Play";
  }
  renderer.render(nowMs);
  window.requestAnimationFrame(frame);
}

window.requestAnimationFrame(frame);
