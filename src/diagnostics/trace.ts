/**
 * SP-1 diagnostic flight recorder.
 *
 * HARD RULES (this file is the contract for every instrumented seam):
 *
 *  1. A record is written AT the decision point, by the code that made the
 *     decision, with that code's own `performance.now()` reading. Nothing may
 *     reconstruct, replay, backfill or drain history into the ring. If you find
 *     yourself iterating an existing log to produce trace records, stop.
 *  2. Capture is epoch-based. `startCapture()` opens a new capture ID; only
 *     events that execute afterwards may enter the ring. `clear()` empties the
 *     ring and opens a new epoch — it never re-imports anything.
 *  3. `stopCapture()` is absolute: zero further records are stored.
 *  4. Periodic polls that observe an UNCHANGED value must record nothing.
 *     Use `recordIfChanged()`; identical semantic frames are counted as
 *     coalesced, not appended.
 *
 * The ring is fixed at 500 records. Statistics (generated / dropped /
 * coalesced) survive rollover so a quiet last second never erases the fact
 * that events happened earlier in the capture.
 */

export const TRACE_CAPACITY = 500;

export type TraceStage =
  | "serial.raw"
  | "serial.parsed"
  | "midi.raw"
  | "midi.device.recognized"
  | "connection.resync"
  | "surface.decoded"
  | "surface.suppressed"
  | "surface.held"
  | "gesture.candidate"
  | "gesture.arbitration"
  | "gesture.owner"
  | "gesture.rejected"
  | "command.surface"
  | "command.engine"
  | "engine.ack"
  | "state.transport"
  | "state.mixer"
  | "state.fx"
  | "led.derived"
  | "led.transmitted"
  | "firmware.led.reported"
  | "capture.control";

export const TRACE_STAGES: readonly TraceStage[] = [
  "serial.raw",
  "serial.parsed",
  "midi.raw",
  "midi.device.recognized",
  "connection.resync",
  "surface.decoded",
  "surface.suppressed",
  "surface.held",
  "gesture.candidate",
  "gesture.arbitration",
  "gesture.owner",
  "gesture.rejected",
  "command.surface",
  "command.engine",
  "engine.ack",
  "state.transport",
  "state.mixer",
  "state.fx",
  "led.derived",
  "led.transmitted",
  "firmware.led.reported",
  "capture.control",
] as const;

/** Everything a caller may attach at the decision point. */
export interface TraceOptions {
  /** performance.now() AT the decision point. Defaults to now. */
  t?: number;
  /** Source-domain timestamp (e.g. Web MIDI event.timeStamp). */
  sourceT?: number;
  /** Input-event correlation. Defaults to the ring's open correlation. */
  corr?: number;
  gesture?: number;
  commandId?: number;
  /** For timer / audio / LED transitions not initiated by a gesture. */
  causeId?: string;
  /** Named reproduction segment this record belongs to. */
  segment?: string;
  detail?: string;
}

export interface TraceRecord {
  seq: number;
  captureId: number;
  corr?: number;
  gesture?: number;
  commandId?: number;
  causeId?: string;
  segment?: string;
  /** performance.now() at the decision point (ms). */
  t: number;
  /** Source-domain timestamp where one exists. */
  sourceT?: number;
  /** Wall clock, for report readability. */
  wall: string;
  stage: TraceStage;
  label: string;
  detail?: string;
  data?: Record<string, unknown>;
}

export interface TraceStats {
  captureId: number;
  running: boolean;
  startedAt: number | null;
  stoppedAt: number | null;
  durationMs: number;
  stored: number;
  capacity: number;
  generated: number;
  dropped: number;
  coalesced: number;
  byStage: Record<string, number>;
}

export function traceNow(): number {
  return typeof performance !== "undefined" ? performance.now() : Date.now();
}

export class TraceRing {
  readonly capacity: number;
  private buf: TraceRecord[] = [];
  private seq = 0;
  private corr = 0;
  private gesture = 0;
  private currentCorr = 0;
  private currentGesture = 0;
  private currentSegment: string | null = null;
  private listeners = new Set<() => void>();

  /** Hot flag read by every instrumented seam before doing any work. */
  enabled = false;

  private captureId = 0;
  private startedAt: number | null = null;
  private stoppedAt: number | null = null;
  private generated = 0;
  private dropped = 0;
  private coalesced = 0;
  private byStage: Record<string, number> = {};
  /** Last semantic signature per dedupe key, for `recordIfChanged`. */
  private signatures = new Map<string, string>();

  constructor(capacity = TRACE_CAPACITY) {
    this.capacity = capacity;
  }

  // ---- capture epochs -----------------------------------------------------

  /**
   * Opens a NEW capture epoch. Nothing that happened before this call can
   * enter the trace; no history is scanned, drained or replayed.
   */
  startCapture(): number {
    this.captureId += 1;
    this.buf = [];
    this.seq = 0;
    this.corr = 0;
    this.gesture = 0;
    this.currentCorr = 0;
    this.currentGesture = 0;
    this.generated = 0;
    this.dropped = 0;
    this.coalesced = 0;
    this.byStage = {};
    this.signatures.clear();
    this.startedAt = traceNow();
    this.stoppedAt = null;
    this.enabled = true;
    this.record("capture.control", `capture ${this.captureId} started`);
    this.notify();
    return this.captureId;
  }

  /** Absolute stop: no later record is stored. */
  stopCapture(): void {
    if (!this.enabled) return;
    this.record("capture.control", `capture ${this.captureId} stopped`);
    this.enabled = false;
    this.stoppedAt = traceNow();
    this.notify();
  }

  /** Legacy aliases. `enable()` opens an epoch only when not already running. */
  enable(): void {
    if (!this.enabled) this.startCapture();
  }

  disable(): void {
    this.stopCapture();
  }

  /** Empties the ring and opens a new epoch. Never re-imports old events. */
  clear(): void {
    const wasRunning = this.enabled;
    this.buf = [];
    this.seq = 0;
    this.generated = 0;
    this.dropped = 0;
    this.coalesced = 0;
    this.byStage = {};
    this.signatures.clear();
    this.captureId += 1;
    this.startedAt = wasRunning ? traceNow() : null;
    this.stoppedAt = wasRunning ? null : traceNow();
    this.notify();
  }

  currentCaptureId(): number {
    return this.captureId;
  }

  // ---- correlation --------------------------------------------------------

  beginCorrelation(): number {
    this.currentCorr = ++this.corr;
    return this.currentCorr;
  }

  /** Re-opens an existing correlation so downstream stages inherit it. */
  useCorrelation(id: number | undefined): void {
    if (typeof id === "number" && id > 0) this.currentCorr = id;
  }

  beginGesture(): number {
    this.currentGesture = ++this.gesture;
    return this.currentGesture;
  }

  endGesture(): void {
    this.currentGesture = 0;
  }

  beginSegment(name: string): void {
    this.currentSegment = name;
  }

  endSegment(): void {
    this.currentSegment = null;
  }

  // ---- recording ----------------------------------------------------------

  record(
    stage: TraceStage,
    label: string,
    data?: Record<string, unknown>,
    optsOrT?: TraceOptions | number,
  ): TraceRecord | null {
    if (!this.enabled) return null;
    const opts: TraceOptions = typeof optsOrT === "number" ? { t: optsOrT } : (optsOrT ?? {});
    const t = typeof opts.t === "number" && Number.isFinite(opts.t) ? opts.t : traceNow();
    const rec: TraceRecord = {
      seq: ++this.seq,
      captureId: this.captureId,
      t,
      wall: new Date().toISOString(),
      stage,
      label,
    };
    if (typeof opts.sourceT === "number" && Number.isFinite(opts.sourceT)) rec.sourceT = opts.sourceT;
    const corr = opts.corr ?? this.currentCorr;
    if (corr) rec.corr = corr;
    const gesture = opts.gesture ?? this.currentGesture;
    if (gesture) rec.gesture = gesture;
    if (typeof opts.commandId === "number") rec.commandId = opts.commandId;
    if (opts.causeId) rec.causeId = opts.causeId;
    const segment = opts.segment ?? this.currentSegment;
    if (segment) rec.segment = segment;
    const detail = opts.detail ?? (data && typeof data["detail"] === "string" ? (data["detail"] as string) : undefined);
    if (detail) rec.detail = detail;
    if (data) rec.data = data;

    this.buf.push(rec);
    this.generated += 1;
    this.byStage[stage] = (this.byStage[stage] ?? 0) + 1;
    if (this.buf.length > this.capacity) {
      const over = this.buf.length - this.capacity;
      this.buf.splice(0, over);
      this.dropped += over;
    }
    this.notify();
    return rec;
  }

  /**
   * Records ONLY when the semantic signature under `key` changed. A periodic
   * poll returning the same frame produces no record; it is counted as
   * coalesced. Returns the record, or null when nothing was written.
   */
  recordIfChanged(
    key: string,
    signature: string,
    stage: TraceStage,
    label: string,
    data?: Record<string, unknown>,
    opts?: TraceOptions,
  ): TraceRecord | null {
    if (!this.enabled) return null;
    if (this.signatures.get(key) === signature) {
      this.coalesced += 1;
      return null;
    }
    this.signatures.set(key, signature);
    return this.record(stage, label, data, opts);
  }

  /** Counts an event that was deliberately folded into a summary record. */
  noteCoalesced(n = 1): void {
    if (!this.enabled) return;
    this.coalesced += n;
  }

  // ---- reading ------------------------------------------------------------

  list(): TraceRecord[] {
    return this.buf.slice();
  }

  size(): number {
    return this.buf.length;
  }

  recent(n: number): TraceRecord[] {
    return this.buf.slice(-n).reverse();
  }

  stats(): TraceStats {
    const end = this.enabled ? traceNow() : (this.stoppedAt ?? this.startedAt ?? 0);
    return {
      captureId: this.captureId,
      running: this.enabled,
      startedAt: this.startedAt,
      stoppedAt: this.stoppedAt,
      durationMs: this.startedAt === null ? 0 : Math.max(0, end - this.startedAt),
      stored: this.buf.length,
      capacity: this.capacity,
      generated: this.generated,
      dropped: this.dropped,
      coalesced: this.coalesced,
      byStage: { ...this.byStage },
    };
  }

  /** Deep, frozen point-in-time copy. Continued capture cannot mutate it. */
  snapshot(): { records: TraceRecord[]; stats: TraceStats } {
    return {
      records: this.buf.map((r) => ({ ...r, ...(r.data ? { data: { ...r.data } } : {}) })),
      stats: this.stats(),
    };
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

/** One readable line per record. */
export function formatTraceRow(rec: TraceRecord, t0: number): string {
  const rel = (rec.t - t0).toFixed(1).padStart(9);
  const stage = rec.stage.padEnd(22);
  const ids =
    `${rec.corr ? `#${rec.corr}` : "#-"}${rec.gesture ? `/g${rec.gesture}` : ""}` +
    `${typeof rec.commandId === "number" ? `/c${rec.commandId}` : ""}${rec.causeId ? `/~${rec.causeId}` : ""}`;
  return `${rel}ms ${ids.padEnd(12)} ${stage} ${rec.label}${rec.detail ? ` — ${rec.detail}` : ""}`;
}

// Browser-proof seam: the smoke harness reads the ring through this handle.
if (typeof window !== "undefined") {
  (window as unknown as { __stemTapeTrace?: TraceRing }).__stemTapeTrace = trace;
}
