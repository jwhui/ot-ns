import type { RpcResponse, ServerMessage, VizEvent } from "./protocol";

interface PendingCall {
  resolve: (value: Record<string, unknown>) => void;
  reject: (err: Error) => void;
}

export class OtnsWsClient {
  private ws: WebSocket | null = null;
  private nextId = 1;
  private pending = new Map<number, PendingCall>();
  private reconnectTimer: number | null = null;
  private closedByUser = false;

  constructor(
    private readonly url: string,
    private readonly onEvent: (ev: VizEvent) => void,
    private readonly onStateChange: (connected: boolean) => void,
  ) {}

  connect(): void {
    this.closedByUser = false;
    this.openSocket();
  }

  disconnect(): void {
    this.closedByUser = true;
    if (this.reconnectTimer !== null) {
      window.clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
    this.onStateChange(false);
  }

  call(method: string, params: Record<string, unknown> = {}): Promise<Record<string, unknown>> {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      return Promise.reject(new Error("WebSocket not connected"));
    }
    const id = this.nextId++;
    const payload = JSON.stringify({ v: 1, type: "rpc.req", id, method, params });

    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.ws?.send(payload);
    });
  }

  private openSocket(): void {
    const ws = new WebSocket(this.url);
    this.ws = ws;

    ws.addEventListener("open", () => {
      this.onStateChange(true);
    });

    ws.addEventListener("message", (ev) => {
      if (typeof ev.data !== "string") {
        return;
      }
      let msg: ServerMessage;
      try {
        msg = JSON.parse(ev.data) as ServerMessage;
      } catch {
        return;
      }
      if (msg.v !== 1) {
        return;
      }
      if (msg.type === "rpc.res") {
        this.handleRpcResponse(msg);
      } else {
        this.onEvent(msg);
      }
    });

    ws.addEventListener("close", () => {
      this.onStateChange(false);
      for (const [, p] of this.pending) {
        p.reject(new Error("Connection closed"));
      }
      this.pending.clear();
      if (!this.closedByUser) {
        this.reconnectTimer = window.setTimeout(() => this.openSocket(), 1500);
      }
    });
  }

  private handleRpcResponse(res: RpcResponse): void {
    const key = typeof res.id === "number" ? res.id : Number(res.id);
    const pending = this.pending.get(key);
    if (!pending) {
      return;
    }
    this.pending.delete(key);
    if (res.error) {
      pending.reject(new Error(`${res.error.code}: ${res.error.message}`));
    } else {
      pending.resolve(res.result ?? {});
    }
  }
}
