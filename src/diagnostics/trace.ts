/**
 * SP-1 diagnostic flight recorder.
 *
 * A FIXED-SIZE ring buffer (max 500 records). Recording is OFF by default and
 * only armed while the diagnostic drawer is open or an explicit capture runs,
 * so a closed drawer costs exactly one boolean test per instrumented seam.
 *
 * Every record is captured AT the decision point with the caller's own
 * timestamp — never reconstructed afterwards from React state.
 */

export const TRACE_CAPACITY = 500;

export type TraceStage =
  | "serial.line"
  | "midi.raw"
  | "midi.recognized"
  | "surface.decoded"
  | "surface.suppressed"
  | "surface.held"
  | "gesture.arbitration"
  | "gesture.owner"
  | "gesture.rejected"
  | "command.surface"
  | "command.audio"
  | "engine.ack"
  | "state.transport"
  | "led.derived";

export interface TraceRecord {
  seq: number;
  /** performance.now() at the decision point (ms). */
  t: number;
  stage: TraceStage;
  label: string;
  detail?: string;
  data?: Record<string, unknown>;
}

export function traceNow(): number {
  return typeof performance !== "undefined" ? performance.now() : Date.now();
}

export class TraceRing {
  readonly capacity: number;
  private buf: TraceRecord[] = [];
  private seq = 0;
  private listeners = new Set<() => void>();
  /** Public and hot: instrumented seams read this before doing any work. */
  enabled = false;

  constructor(capacity = TRACE_CAPACITY) {
    this.capacity = capacity;
  }

  enable(): void {
    this.enabled = true;
  }

  disable(): void {
    this.enabled = false;
  }

  clear(): void {
    this.buf = [];
    this.seq = 0;
    this.notify();
  }

  record(stage: TraceStage, label: string, data?: Record<string, unknown>, t?: number): void {
    if (!this.enabled) return;
    const rec: TraceRecord = {
      seq: ++this.seq,
      t: typeof t === "number" && Number.isFinite(t) ? t : traceNow(),
      stage,
      label,
    };
    if (data && typeof data["detail"] === "string") rec.detail = data["detail"] as string;
    if (data) rec.data = data;
    this.buf.push(rec);
    if (this.buf.length > this.capacity) this.buf.splice(0, this.buf.length - this.capacity);
    this.notify();
  }

  /** Oldest → newest. */
  list(): TraceRecord[] {
    return this.buf.slice();
  }

  size(): number {
    return this.buf.length;
  }

  /** Newest → oldest, capped. */
  recent(n: number): TraceRecord[] {
    return this.buf.slice(-n).reverse();
  }

  subscribe(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  private notify(): void {
    for (const fn of this.listeners) fn();
  }
}

export const trace = new TraceRing();

/** One readable line per record: `12.3ms  gesture.owner  PLAY HELD → FX owner`. */
export function formatTraceRow(rec: TraceRecord, t0: number): string {
  const rel = (rec.t - t0).toFixed(1).padStart(8);
  const stage = rec.stage.padEnd(20);
  return `${rel}ms ${stage} ${rec.label}${rec.detail ? ` — ${rec.detail}` : ""}`;
}

// Browser-proof seam: the smoke harness reads the ring through this handle.
if (typeof window !== "undefined") {
  (window as unknown as { __stemTapeTrace?: TraceRing }).__stemTapeTrace = trace;
}
