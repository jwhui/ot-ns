export type NodeRole =
  | "disabled"
  | "detached"
  | "child"
  | "router"
  | "leader"
  | "reed"
  | "fed"
  | "med"
  | "sed";

export interface NodeState {
  id: number;
  x: number;
  y: number;
  z?: number;
  role: NodeRole;
  rloc16?: number;
  ext_addr?: string;
  partition_id?: number;
  parent_id?: number;
  radio_range?: number;
  failed?: boolean;
  kind?: "inproc" | "external";
}

export interface LinkState {
  src: number;
  dst: number;
  kind: "parent" | "router" | "child" | "neighbor";
  rssi_dbm?: number;
  lqi?: number;
}

export interface NodeStats {
  total: number;
  leaders: number;
  routers: number;
  children: number;
  sed?: number;
  detached: number;
  disabled: number;
  failed?: number;
  partitions: number;
}

export interface SnapshotEvent {
  v: 1;
  type: "event.snapshot";
  ts_us: number;
  engine: string;
  speed?: number;
  paused?: boolean;
  rf_model?: string;
  nodes: NodeState[];
  links: LinkState[];
  stats?: NodeStats;
}

export interface BatchDiffEvent {
  v: 1;
  type: "event.batch_diff";
  ts_us: number;
  upsert_nodes?: NodeState[];
  remove_nodes?: number[];
  upsert_links?: LinkState[];
  remove_links?: Array<{ src: number; dst: number; kind: string }>;
}

export interface PacketEvent {
  v: 1;
  type: "event.packet";
  ts_us: number;
  src: number;
  dst?: number;
  channel: number;
  duration_us?: number;
  rssi_dbm?: number;
  frame_len?: number;
  protocol?: string;
  summary?: string;
}

export interface StatsEvent {
  v: 1;
  type: "event.stats";
  ts_us: number;
  stats: NodeStats;
}

export interface LogEvent {
  v: 1;
  type: "event.log";
  ts_us: number;
  node_id: number;
  level?: string;
  message: string;
}

export interface HeartbeatEvent {
  v: 1;
  type: "event.heartbeat";
  ts_us: number;
  speed: number;
  paused: boolean;
}

export interface RpcResponse {
  v: 1;
  type: "rpc.res";
  id: number | string;
  result?: Record<string, unknown>;
  error?: { code: string; message: string };
}

export type VizEvent =
  | SnapshotEvent
  | BatchDiffEvent
  | PacketEvent
  | StatsEvent
  | LogEvent
  | HeartbeatEvent;

export type ServerMessage = VizEvent | RpcResponse;
