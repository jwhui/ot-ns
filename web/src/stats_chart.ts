import type { NodeStats } from "./protocol";

interface StatsPoint {
  tsUs: number;
  leaders: number;
  routers: number;
  children: number;
  detached: number;
}

export class StatsChart {
  private ctx: CanvasRenderingContext2D;
  private history: StatsPoint[] = [];

  constructor(private readonly canvas: HTMLCanvasElement) {
    const ctx = canvas.getContext("2d");
    if (!ctx) {
      throw new Error("Stats 2D canvas unavailable");
    }
    this.ctx = ctx;
  }

  clear(): void {
    this.history = [];
    this.draw();
  }

  push(tsUs: number, stats: NodeStats): void {
    const last = this.history[this.history.length - 1];
    if (last && last.tsUs === tsUs) {
      last.leaders = stats.leaders;
      last.routers = stats.routers;
      last.children = stats.children;
      last.detached = stats.detached;
    } else {
      this.history.push({
        tsUs,
        leaders: stats.leaders,
        routers: stats.routers,
        children: stats.children,
        detached: stats.detached,
      });
      if (this.history.length > 180) {
        this.history.shift();
      }
    }
    this.draw();
  }

  private draw(): void {
    const ctx = this.ctx;
    const w = this.canvas.width;
    const h = this.canvas.height;
    ctx.clearRect(0, 0, w, h);

    if (this.history.length < 2) {
      return;
    }

    let maxY = 4;
    for (const pt of this.history) {
      const m = Math.max(pt.leaders, pt.routers, pt.children, pt.detached);
      if (m > maxY) maxY = m;
    }

    const drawSeries = (getter: (p: StatsPoint) => number, color: string): void => {
      ctx.beginPath();
      ctx.strokeStyle = color;
      ctx.lineWidth = 1.6;
      for (let i = 0; i < this.history.length; i++) {
        const x = (i / (this.history.length - 1)) * (w - 8) + 4;
        const y = h - 6 - (getter(this.history[i]) / maxY) * (h - 14);
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();
    };

    drawSeries((p) => p.children, "#22c55e");
    drawSeries((p) => p.routers, "#38bdf8");
    drawSeries((p) => p.leaders, "#f59e0b");
    drawSeries((p) => p.detached, "#ef4444");
  }
}
