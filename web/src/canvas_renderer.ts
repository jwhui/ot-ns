import type { LinkState, NodeRole, NodeState, PacketEvent } from "./protocol";

interface PacketAnim {
  srcX: number;
  srcY: number;
  dstX: number | null;
  dstY: number | null;
  createdMs: number;
  durationMs: number;
  isAck: boolean;
}

const ROLE_COLORS: Record<NodeRole, string> = {
  leader: "#f59e0b",
  router: "#38bdf8",
  reed: "#38bdf8",
  child: "#22c55e",
  fed: "#22c55e",
  med: "#22c55e",
  sed: "#4ade80",
  detached: "#ef4444",
  disabled: "#64748b",
};

const PARTITION_PALETTE = [
  "#38bdf8",
  "#f59e0b",
  "#a855f7",
  "#ec4899",
  "#14b8a6",
  "#84cc16",
  "#fb7185",
  "#60a5fa",
];

function partitionColor(partitionId: number | undefined): string {
  if (!partitionId) {
    return "rgba(148, 163, 184, 0.25)";
  }
  return PARTITION_PALETTE[partitionId % PARTITION_PALETTE.length];
}

export class TopologyRenderer {
  private ctx: CanvasRenderingContext2D;
  private width = 1;
  private height = 1;
  private dpr = 1;

  // World -> Screen transform: screen = world * scale + offset
  private offsetX = 120;
  private offsetY = 120;
  private scale = 1.6;

  private nodes = new Map<number, NodeState>();
  private links = new Map<string, LinkState>();
  private packets: PacketAnim[] = [];

  private selectedNodeId: number | null = null;
  private hoveredNodeId: number | null = null;
  private showRanges = false;

  // Drag state
  private draggingNodeId: number | null = null;
  private panning = false;
  private dragLastX = 0;
  private dragLastY = 0;
  private didMoveNode = false;

  constructor(
    private readonly canvas: HTMLCanvasElement,
    private readonly onSelectNode: (nodeId: number | null) => void,
    private readonly onMoveNode: (nodeId: number, x: number, y: number) => void,
  ) {
    const ctx = canvas.getContext("2d");
    if (!ctx) {
      throw new Error("2D canvas context unavailable");
    }
    this.ctx = ctx;
    this.bindInput();
    this.resize();
  }

  resize(): void {
    const rect = this.canvas.getBoundingClientRect();
    this.dpr = window.devicePixelRatio || 1;
    this.width = Math.max(1, rect.width);
    this.height = Math.max(1, rect.height);
    this.canvas.width = Math.round(this.width * this.dpr);
    this.canvas.height = Math.round(this.height * this.dpr);
  }

  setShowRanges(show: boolean): void {
    this.showRanges = show;
  }

  getShowRanges(): boolean {
    return this.showRanges;
  }

  setState(nodes: Map<number, NodeState>, links: Map<string, LinkState>): void {
    this.nodes = nodes;
    this.links = links;
  }

  setSelectedNode(id: number | null): void {
    this.selectedNodeId = id;
  }

  clearPackets(): void {
    this.packets = [];
  }

  addPacketAnim(pkt: PacketEvent): void {
    const src = this.nodes.get(pkt.src);
    if (!src) {
      return;
    }
    const dst = pkt.dst ? this.nodes.get(pkt.dst) : undefined;
    if (this.packets.length > 200) {
      this.packets.shift();
    }
    this.packets.push({
      srcX: src.x,
      srcY: src.y,
      dstX: dst ? dst.x : null,
      dstY: dst ? dst.y : null,
      createdMs: performance.now(),
      durationMs: 420,
      isAck: pkt.protocol === "802.15.4-ACK",
    });
  }

  fitToNodes(): void {
    if (this.nodes.size === 0) {
      this.offsetX = this.width / 2;
      this.offsetY = this.height / 2;
      this.scale = 1.6;
      return;
    }
    let minX = Infinity;
    let maxX = -Infinity;
    let minY = Infinity;
    let maxY = -Infinity;
    for (const n of this.nodes.values()) {
      if (n.x < minX) minX = n.x;
      if (n.x > maxX) maxX = n.x;
      if (n.y < minY) minY = n.y;
      if (n.y > maxY) maxY = n.y;
    }
    const pad = 80;
    const spanX = Math.max(80, maxX - minX);
    const spanY = Math.max(80, maxY - minY);
    const sx = (this.width - pad * 2) / spanX;
    const sy = (this.height - pad * 2) / spanY;
    this.scale = Math.max(0.15, Math.min(4.0, Math.min(sx, sy)));
    this.offsetX = this.width / 2 - ((minX + maxX) / 2) * this.scale;
    this.offsetY = this.height / 2 - ((minY + maxY) / 2) * this.scale;
  }

  render(nowMs: number): void {
    const ctx = this.ctx;
    ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
    ctx.clearRect(0, 0, this.width, this.height);

    this.drawGrid(ctx);

    // Viewport world AABB for culling
    const viewMinX = (0 - this.offsetX) / this.scale - 40;
    const viewMaxX = (this.width - this.offsetX) / this.scale + 40;
    const viewMinY = (0 - this.offsetY) / this.scale - 40;
    const viewMaxY = (this.height - this.offsetY) / this.scale + 40;

    // 1. Optional radio range circles (hover/selected or global toggle)
    for (const n of this.nodes.values()) {
      const show = this.showRanges || n.id === this.selectedNodeId || n.id === this.hoveredNodeId;
      if (!show || !n.radio_range) {
        continue;
      }
      const sx = n.x * this.scale + this.offsetX;
      const sy = n.y * this.scale + this.offsetY;
      const r = n.radio_range * this.scale;
      ctx.beginPath();
      ctx.arc(sx, sy, r, 0, Math.PI * 2);
      ctx.strokeStyle = n.id === this.selectedNodeId ? "rgba(56, 189, 248, 0.35)" : "rgba(148, 163, 184, 0.14)";
      ctx.lineWidth = 1;
      ctx.setLineDash([4, 4]);
      ctx.stroke();
      ctx.setLineDash([]);
    }

    // 2. Batched link rendering (1 path per link kind for 60fps at 1000+ nodes)
    this.drawBatchedLinks(ctx, "router", "rgba(56, 189, 248, 0.48)", 1.8);
    this.drawBatchedLinks(ctx, "parent", "rgba(34, 197, 94, 0.45)", 1.25);
    this.drawBatchedLinks(ctx, "child", "rgba(34, 197, 94, 0.22)", 1.0);

    // 3. Packet animations
    this.drawPackets(ctx, nowMs);

    // 4. LOD node rendering
    const lowDetail = this.scale < 0.45 || (this.nodes.size > 250 && this.scale < 0.7);
    const radius = lowDetail ? Math.max(3, 5 * this.scale) : Math.max(7, Math.min(13, 8.5 * Math.sqrt(this.scale)));

    ctx.font = "11px ui-monospace, SFMono-Regular, Menlo, monospace";
    ctx.textAlign = "center";

    for (const n of this.nodes.values()) {
      if (n.x < viewMinX || n.x > viewMaxX || n.y < viewMinY || n.y > viewMaxY) {
        continue;
      }
      const sx = n.x * this.scale + this.offsetX;
      const sy = n.y * this.scale + this.offsetY;
      const fill = n.failed ? "#475569" : ROLE_COLORS[n.role] ?? "#94a3b8";

      if (lowDetail) {
        ctx.fillStyle = fill;
        ctx.beginPath();
        ctx.arc(sx, sy, radius, 0, Math.PI * 2);
        ctx.fill();
        continue;
      }

      // Partition halo
      if (n.partition_id && !n.failed) {
        ctx.beginPath();
        ctx.arc(sx, sy, radius + 3.5, 0, Math.PI * 2);
        ctx.strokeStyle = partitionColor(n.partition_id);
        ctx.lineWidth = 1.5;
        ctx.globalAlpha = 0.5;
        ctx.stroke();
        ctx.globalAlpha = 1.0;
      }

      // Role shape (Leader = hexagon, Router = circle, Child = rounded dot)
      ctx.fillStyle = fill;
      ctx.beginPath();
      if (n.role === "leader") {
        for (let i = 0; i < 6; i++) {
          const ang = (Math.PI / 3) * i - Math.PI / 6;
          const px = sx + (radius + 2) * Math.cos(ang);
          const py = sy + (radius + 2) * Math.sin(ang);
          if (i === 0) ctx.moveTo(px, py);
          else ctx.lineTo(px, py);
        }
        ctx.closePath();
      } else {
        ctx.arc(sx, sy, n.role === "child" ? radius * 0.78 : radius, 0, Math.PI * 2);
      }
      ctx.fill();

      // External ot-rfsim node indicator ring
      if (n.kind === "external") {
        ctx.beginPath();
        ctx.arc(sx, sy, radius + 6, 0, Math.PI * 2);
        ctx.strokeStyle = "#c084fc";
        ctx.lineWidth = 2;
        ctx.stroke();
      }

      // Selection ring
      if (n.id === this.selectedNodeId) {
        ctx.beginPath();
        ctx.arc(sx, sy, radius + 7, 0, Math.PI * 2);
        ctx.strokeStyle = "#ffffff";
        ctx.lineWidth = 2;
        ctx.stroke();
      }

      // Label (extra clearance when selected or external ring is shown)
      const hasOuterRing = n.id === this.selectedNodeId || n.kind === "external";
      ctx.fillStyle = "#e2e8f0";
      ctx.fillText(String(n.id), sx, sy + radius + (hasOuterRing ? 19 : 15));

      // Optional application / external node badge pill (e.g. matter, otbr, rcp)
      if (n.label) {
        const tag = n.label.length > 14 ? `${n.label.slice(0, 13)}…` : n.label;
        ctx.save();
        ctx.font = "600 9.5px ui-monospace, SFMono-Regular, Menlo, monospace";
        const textW = ctx.measureText(tag).width;
        const padX = 5;
        const pillW = textW + padX * 2;
        const pillH = 14;
        const px = sx - pillW / 2;
        const py = sy - radius - (hasOuterRing ? 24 : 20);
        const r = 4;

        ctx.beginPath();
        ctx.moveTo(px + r, py);
        ctx.lineTo(px + pillW - r, py);
        ctx.quadraticCurveTo(px + pillW, py, px + pillW, py + r);
        ctx.lineTo(px + pillW, py + pillH - r);
        ctx.quadraticCurveTo(px + pillW, py + pillH, px + pillW - r, py + pillH);
        ctx.lineTo(px + r, py + pillH);
        ctx.quadraticCurveTo(px, py + pillH, px, py + pillH - r);
        ctx.lineTo(px, py + r);
        ctx.quadraticCurveTo(px, py, px + r, py);
        ctx.closePath();

        const isMatter = n.label.toLowerCase().startsWith("matter");
        const accent = isMatter ? "#38bdf8" : n.kind === "external" ? "#c084fc" : "#94a3b8";
        ctx.fillStyle = "rgba(15, 23, 42, 0.88)";
        ctx.fill();
        ctx.strokeStyle = accent;
        ctx.lineWidth = 1;
        ctx.stroke();

        ctx.fillStyle = accent;
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        ctx.fillText(tag, sx, py + pillH / 2 + 0.5);
        ctx.restore();
      }
    }
  }

  private drawGrid(ctx: CanvasRenderingContext2D): void {
    const stepWorld = this.scale < 0.5 ? 200 : 50;
    const stepPx = stepWorld * this.scale;
    if (stepPx < 16) {
      return;
    }
    const startX = ((this.offsetX % stepPx) + stepPx) % stepPx;
    const startY = ((this.offsetY % stepPx) + stepPx) % stepPx;

    ctx.beginPath();
    ctx.strokeStyle = "rgba(51, 65, 85, 0.22)";
    ctx.lineWidth = 1;
    for (let x = startX; x < this.width; x += stepPx) {
      ctx.moveTo(x, 0);
      ctx.lineTo(x, this.height);
    }
    for (let y = startY; y < this.height; y += stepPx) {
      ctx.moveTo(0, y);
      ctx.lineTo(this.width, y);
    }
    ctx.stroke();
  }

  private drawBatchedLinks(
    ctx: CanvasRenderingContext2D,
    kind: LinkState["kind"],
    color: string,
    width: number,
  ): void {
    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = width;
    for (const link of this.links.values()) {
      if (link.kind !== kind) {
        continue;
      }
      const a = this.nodes.get(link.src);
      const b = this.nodes.get(link.dst);
      if (!a || !b) {
        continue;
      }
      ctx.moveTo(a.x * this.scale + this.offsetX, a.y * this.scale + this.offsetY);
      ctx.lineTo(b.x * this.scale + this.offsetX, b.y * this.scale + this.offsetY);
    }
    ctx.stroke();
  }

  private drawPackets(ctx: CanvasRenderingContext2D, nowMs: number): void {
    const remaining: PacketAnim[] = [];
    for (const p of this.packets) {
      const t = (nowMs - p.createdMs) / p.durationMs;
      if (t >= 1.0) {
        continue;
      }
      remaining.push(p);
      const sx = p.srcX * this.scale + this.offsetX;
      const sy = p.srcY * this.scale + this.offsetY;
      if (p.dstX !== null && p.dstY !== null) {
        const dx = p.dstX * this.scale + this.offsetX;
        const dy = p.dstY * this.scale + this.offsetY;
        const px = sx + (dx - sx) * t;
        const py = sy + (dy - sy) * t;
        ctx.beginPath();
        ctx.arc(px, py, p.isAck ? 3.5 : 4.5, 0, Math.PI * 2);
        ctx.fillStyle = p.isAck ? "#4ade80" : "#facc15";
        ctx.fill();
      } else {
        // Broadcast ripple
        const r = 10 + t * 55 * Math.min(1.5, this.scale);
        ctx.beginPath();
        ctx.arc(sx, sy, r, 0, Math.PI * 2);
        ctx.strokeStyle = `rgba(250, 204, 21, ${(1 - t) * 0.65})`;
        ctx.lineWidth = 1.5;
        ctx.stroke();
      }
    }
    this.packets = remaining;
  }

  private pickNode(screenX: number, screenY: number): number | null {
    const hitRadius = 16;
    let bestId: number | null = null;
    let bestDistSq = hitRadius * hitRadius;

    for (const n of this.nodes.values()) {
      const sx = n.x * this.scale + this.offsetX;
      const sy = n.y * this.scale + this.offsetY;
      const dx = sx - screenX;
      const dy = sy - screenY;
      const d2 = dx * dx + dy * dy;
      if (d2 <= bestDistSq) {
        bestDistSq = d2;
        bestId = n.id;
      }
    }
    return bestId;
  }

  private bindInput(): void {
    this.canvas.addEventListener("mousedown", (ev) => {
      const rect = this.canvas.getBoundingClientRect();
      const sx = ev.clientX - rect.left;
      const sy = ev.clientY - rect.top;
      const hit = this.pickNode(sx, sy);

      this.dragLastX = sx;
      this.dragLastY = sy;
      this.didMoveNode = false;

      if (hit !== null) {
        this.draggingNodeId = hit;
        this.selectedNodeId = hit;
        this.onSelectNode(hit);
      } else {
        this.panning = true;
      }
    });

    window.addEventListener("mousemove", (ev) => {
      const rect = this.canvas.getBoundingClientRect();
      const sx = ev.clientX - rect.left;
      const sy = ev.clientY - rect.top;

      if (this.draggingNodeId !== null) {
        const node = this.nodes.get(this.draggingNodeId);
        if (node) {
          node.x = (sx - this.offsetX) / this.scale;
          node.y = (sy - this.offsetY) / this.scale;
          this.didMoveNode = true;
        }
        return;
      }

      if (this.panning) {
        this.offsetX += sx - this.dragLastX;
        this.offsetY += sy - this.dragLastY;
        this.dragLastX = sx;
        this.dragLastY = sy;
        return;
      }

      this.hoveredNodeId = this.pickNode(sx, sy);
    });

    window.addEventListener("mouseup", () => {
      if (this.draggingNodeId !== null && this.didMoveNode) {
        const node = this.nodes.get(this.draggingNodeId);
        if (node) {
          this.onMoveNode(node.id, Math.round(node.x * 10) / 10, Math.round(node.y * 10) / 10);
        }
      }
      this.draggingNodeId = null;
      this.panning = false;
    });

    this.canvas.addEventListener(
      "wheel",
      (ev) => {
        ev.preventDefault();
        const rect = this.canvas.getBoundingClientRect();
        const sx = ev.clientX - rect.left;
        const sy = ev.clientY - rect.top;
        const factor = ev.deltaY < 0 ? 1.12 : 0.89;
        const nextScale = Math.max(0.12, Math.min(8.0, this.scale * factor));
        const wx = (sx - this.offsetX) / this.scale;
        const wy = (sy - this.offsetY) / this.scale;
        this.scale = nextScale;
        this.offsetX = sx - wx * this.scale;
        this.offsetY = sy - wy * this.scale;
      },
      { passive: false },
    );
  }
}
