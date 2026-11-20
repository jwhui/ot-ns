import type { VizEvent } from "./protocol";

export class ReplayPlayer {
  private events: VizEvent[] = [];
  private cursor = 0;
  private currentTsUs = 0;
  private durationUs = 0;
  private playing = false;
  private lastTickMs = 0;

  loadJsonl(text: string): number {
    const parsed: VizEvent[] = [];
    for (const rawLine of text.split("\n")) {
      const line = rawLine.trim();
      if (!line) {
        continue;
      }
      try {
        const obj = JSON.parse(line) as VizEvent;
        if (obj && obj.v === 1 && typeof obj.type === "string" && obj.type.startsWith("event.")) {
          parsed.push(obj);
        }
      } catch {
        // Ignore malformed lines
      }
    }
    this.events = parsed;
    this.cursor = 0;
    this.currentTsUs = 0;
    this.durationUs = parsed.length > 0 ? parsed[parsed.length - 1].ts_us : 0;
    this.playing = false;
    return parsed.length;
  }

  getDurationUs(): number {
    return this.durationUs;
  }

  getCurrentTsUs(): number {
    return this.currentTsUs;
  }

  isPlaying(): boolean {
    return this.playing;
  }

  setPlaying(play: boolean): void {
    this.playing = play;
    this.lastTickMs = performance.now();
  }

  seekTo(targetTsUs: number, apply: (ev: VizEvent) => void): void {
    this.cursor = 0;
    this.currentTsUs = Math.max(0, Math.min(this.durationUs, targetTsUs));
    while (this.cursor < this.events.length && this.events[this.cursor].ts_us <= this.currentTsUs) {
      apply(this.events[this.cursor]);
      this.cursor++;
    }
  }

  tick(nowMs: number, speed: number, apply: (ev: VizEvent) => void): void {
    if (!this.playing) {
      return;
    }
    const dtMs = Math.max(0, Math.min(100, nowMs - this.lastTickMs));
    this.lastTickMs = nowMs;
    const effectiveSpeed = speed > 0 ? speed : 5.0;
    this.currentTsUs += Math.round(dtMs * 1000 * effectiveSpeed);

    while (this.cursor < this.events.length && this.events[this.cursor].ts_us <= this.currentTsUs) {
      apply(this.events[this.cursor]);
      this.cursor++;
    }
    if (this.cursor >= this.events.length) {
      this.playing = false;
      this.currentTsUs = this.durationUs;
    }
  }
}
