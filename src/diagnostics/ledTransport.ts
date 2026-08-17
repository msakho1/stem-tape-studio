/**
 * Physical SP-1 LED sink — host→device LED protocol v1.
 *
 * MIDI channel 16 (status 0xBF):
 *   CC80..87  stage physical LED index 0..7, value 0..127
 *   CC88      commit, value = modulo-128 commit sequence
 *   CC89      heartbeat, value = last committed sequence (250 ms)
 *   CC90      release host ownership
 *   CC91 = 0  capability query (host → device)
 *   CC91 = 1  protocol-v1 capability response (device → host)
 *
 * Firmware lease timeout is 1000 ms; heartbeats keep it alive.
 *
 * Boundaries this module refuses to cross:
 *  • It consumes the already-resolved authoritative physical frame. It never
 *    re-derives transport, loop, scrub or FX state.
 *  • Transmission is only enabled after a protocol-v1 CC91 response arrives
 *    from the recognized SP-1 INPUT. `send()` succeeding proves only that the
 *    browser attempted transmission — never that the firmware committed.
 *  • Web-only indicators are structurally unreachable: the frame has exactly
 *    eight indices.
 */

import { trace, traceNow } from "./trace";

/**
 * Minimal structural contract every authoritative resolver must satisfy.
 * `src/leds/sp1LedEngine.ts` is the production producer; the legacy
 * `resolvePhysicalFrame` shape remains assignable for diagnostics/tests.
 */
export interface TransmittableFrame {
  values: number[];
  signature: string;
  leds: readonly { owner: string; priority?: number; precedence?: number }[];
}

const SHORT = ["T1", "T2", "T3", "T4", "S1", "S2", "S3", "S4"];

function formatFrame(f: TransmittableFrame): string {
  const cell = (i: number) => `${SHORT[i]} ${f.values[i] ?? 0}`;
  const rank = (l: TransmittableFrame["leds"][number]) => l.priority ?? l.precedence ?? 0;
  const top = [...f.leds].sort((a, b) => rank(b) - rank(a))[0];
  return `[${[0, 1, 2, 3].map(cell).join(", ")} | ${[4, 5, 6, 7].map(cell).join(", ")}] owner=${top?.owner ?? "none"}`;
}


export const LED_CHANNEL = 15; // channel 16, zero-based
export const LED_STATUS = 0xb0 | LED_CHANNEL; // 0xBF
export const CC_LED_BASE = 80;
export const CC_COMMIT = 88;
export const CC_HEARTBEAT = 89;
export const CC_RELEASE = 90;
export const CC_CAPABILITY = 91;
export const HEARTBEAT_MS = 250;
export const LEASE_TIMEOUT_MS = 1000;
export const LED_PROTOCOL_VERSION = 1;

export type LedLinkStatus =
  | "no-input"
  | "input-connected"
  | "no-output"
  | "output-matched"
  | "query-sent"
  | "protocol-v1"
  | "lease-active"
  | "legacy-no-response"
  | "error";

export interface MidiOutLike {
  id: string;
  name: string;
  send: (bytes: number[]) => void;
}

export interface LedTransportState {
  status: LedLinkStatus;
  inputConnected: boolean;
  inputName: string | null;
  outputName: string | null;
  outputId: string | null;
  candidateOutputs: { id: string; name: string }[];
  capabilityQuerySent: boolean;
  protocolVersion: number | null;
  leaseActive: boolean;
  commitSequence: number;
  commits: number;
  heartbeats: number;
  stagedMessages: number;
  lastFrame: number[] | null;
  error: string | null;
}

/** Strip vendor noise so an input and an output can be matched by identity. */
export function normalizeMidiIdentity(name: string | null | undefined): string {
  return (name ?? "")
    .toUpperCase()
    .replace(/\b(MIDI\s*)?(IN|OUT|INPUT|OUTPUT|PORT)\b/g, " ")
    .replace(/[^A-Z0-9]+/g, " ")
    .trim();
}

/**
 * Matching NEVER assumes input.id === output.id: hosts mint independent IDs
 * for the two directions.
 */
export function matchOutputsForInput<T extends { id: string; name: string }>(
  inputName: string | null,
  outputs: readonly T[],
): T[] {
  const target = normalizeMidiIdentity(inputName);
  if (!target) return [];
  return outputs.filter((o) => {
    const n = normalizeMidiIdentity(o.name);
    return n === target || n.includes(target) || target.includes(n);
  });
}

interface AnimationSummary {
  causeId: string;
  first: number;
  last: number;
  frames: number;
  min: number[];
  max: number[];
  seqFrom: number;
  seqTo: number;
  latest: number[];
}

export class PhysicalLedTransport {
  private out: MidiOutLike | null = null;
  private lastValues: number[] | null = null;
  private lastSignature: string | null = null;
  private heartbeatTimer: ReturnType<typeof setInterval> | null = null;
  private anim: AnimationSummary | null = null;
  private listeners = new Set<(s: LedTransportState) => void>();

  /** Individual heartbeat/animation frames only enter the ring when true. */
  verboseTransport = false;

  private state: LedTransportState = {
    status: "no-input",
    inputConnected: false,
    inputName: null,
    outputName: null,
    outputId: null,
    candidateOutputs: [],
    capabilityQuerySent: false,
    protocolVersion: null,
    leaseActive: false,
    commitSequence: 0,
    commits: 0,
    heartbeats: 0,
    stagedMessages: 0,
    lastFrame: null,
    error: null,
  };

  snapshot(): LedTransportState {
    return { ...this.state, lastFrame: this.state.lastFrame ? [...this.state.lastFrame] : null };
  }

  subscribe(fn: (s: LedTransportState) => void): () => void {
    this.listeners.add(fn);
    fn(this.snapshot());
    return () => this.listeners.delete(fn);
  }

  private publish(patch: Partial<LedTransportState>): void {
    this.state = { ...this.state, ...patch };
    const snap = this.snapshot();
    for (const fn of this.listeners) fn(snap);
  }

  /** Recognized SP-1 input presence, reported by the MIDI adapter. */
  setInput(name: string | null): void {
    this.publish({
      inputConnected: !!name,
      inputName: name,
      status: name ? (this.state.outputId ? this.state.status : "input-connected") : "no-input",
    });
    if (!name) this.detach("input disconnected");
  }

  /** Offer candidate outputs; the caller may pick one explicitly. */
  offerOutputs(outputs: readonly MidiOutLike[]): void {
    const candidates = matchOutputsForInput(this.state.inputName, outputs);
    this.publish({
      candidateOutputs: candidates.map((o) => ({ id: o.id, name: o.name })),
      status: candidates.length === 0 && this.state.inputConnected ? "no-output" : this.state.status,
    });
    if (candidates.length === 0) return;
    const keep = candidates.find((o) => o.id === this.state.outputId);
    this.selectOutput(keep ?? candidates[0]!);
  }

  selectOutput(out: MidiOutLike): void {
    if (this.out?.id === out.id) return;
    this.out = out;
    this.lastValues = null;
    this.lastSignature = null;
    this.publish({
      outputId: out.id,
      outputName: out.name,
      status: "output-matched",
      protocolVersion: null,
      leaseActive: false,
      capabilityQuerySent: false,
    });
  }

  /** CC91 value 0. Transmission stays disabled until the device answers. */
  queryCapability(): boolean {
    if (!this.out) return false;
    try {
      this.out.send([LED_STATUS, CC_CAPABILITY, 0]);
    } catch (err) {
      this.publish({ status: "error", error: err instanceof Error ? err.message : String(err) });
      return false;
    }
    this.publish({ capabilityQuerySent: true, status: "query-sent" });
    trace.record("led.transmitted", `capability query CC91=0 → ${this.out.name}`, {
      cc: CC_CAPABILITY,
      value: 0,
      output: this.out.name,
    }, { causeId: "led.capability" });
    return true;
  }

  /** Feed every CC seen on the recognized SP-1 input here. */
  handleDeviceCc(cc: number, value: number): boolean {
    if (cc !== CC_CAPABILITY) return false;
    if (value === LED_PROTOCOL_VERSION) {
      this.publish({ protocolVersion: 1, status: "protocol-v1", error: null });
      trace.record("firmware.led.reported", "SP-1 answered CC91=1 — LED protocol v1 supported", {
        cc,
        value,
      }, { causeId: "led.capability" });
      return true;
    }
    this.publish({ status: "legacy-no-response", protocolVersion: 0 });
    return true;
  }

  /** Firmware CDC report of its committed frame (proof of PWM command only). */
  reportFirmwareFrame(info: {
    hostOwned: boolean;
    sequence: number;
    frame: number[];
    rendererReady: boolean;
    pwmErrors: number;
  }): void {
    trace.record(
      "firmware.led.reported",
      `firmware committed seq ${info.sequence} · host-owned=${info.hostOwned} · renderer=${info.rendererReady ? "ready" : "not-ready"}`,
      { ...info, frame: [...info.frame] },
      { causeId: "led.firmware" },
    );
  }

  private canTransmit(): boolean {
    return !!this.out && this.state.protocolVersion === LED_PROTOCOL_VERSION;
  }

  /**
   * Present the authoritative resolved frame. Unchanged frames send nothing
   * (the heartbeat alone keeps the lease). Changed frames stage only the
   * changed indices; the first takeover stages all eight.
   */
  present(resolved: ResolvedPhysicalFrame): "sent" | "unchanged" | "blocked" {
    if (!this.canTransmit()) return "blocked";
    const out = this.out!;
    const values = resolved.values;
    if (this.lastSignature === resolved.signature) return "unchanged";
    const first = this.lastValues === null;
    const changed: number[] = [];
    for (let i = 0; i < 8; i++) {
      if (first || this.lastValues![i] !== values[i]) changed.push(i);
    }
    try {
      for (const i of changed) out.send([LED_STATUS, CC_LED_BASE + i, values[i]!]);
      const seq = (this.state.commitSequence + 1) % 128;
      out.send([LED_STATUS, CC_COMMIT, seq]);
      this.lastValues = [...values];
      this.lastSignature = resolved.signature;
      this.publish({
        commitSequence: seq,
        commits: this.state.commits + 1,
        stagedMessages: this.state.stagedMessages + changed.length,
        lastFrame: [...values],
        leaseActive: true,
        status: "lease-active",
        error: null,
      });
      trace.record(
        "led.transmitted",
        `${formatPhysicalFrame(resolved)} staged=${changed.length}${first ? " (full takeover)" : ""} commit=${seq}`,
        { staged: changed, values: [...values], commitSequence: seq, channel: 16 },
        { causeId: "led.frame" },
      );
      this.startHeartbeat();
      return "sent";
    } catch (err) {
      this.publish({ status: "error", error: err instanceof Error ? err.message : String(err) });
      return "blocked";
    }
  }

  /**
   * Rapid physical brightness frames during breathe/pulse. These NEVER consume
   * one ring record each: they are folded into one summary flushed on change
   * or release (unless VERBOSE LED TRANSPORT is on).
   */
  presentAnimationFrame(values: number[], causeId = "led.animation"): void {
    if (!this.canTransmit()) return;
    const out = this.out!;
    const seq = (this.state.commitSequence + 1) % 128;
    try {
      for (let i = 0; i < 8; i++) out.send([LED_STATUS, CC_LED_BASE + i, values[i] ?? 0]);
      out.send([LED_STATUS, CC_COMMIT, seq]);
    } catch {
      return;
    }
    const t = traceNow();
    this.publish({ commitSequence: seq, lastFrame: [...values] });
    if (this.verboseTransport) {
      trace.record("led.transmitted", `animation frame commit=${seq}`, { values: [...values] }, { causeId });
      return;
    }
    if (!this.anim || this.anim.causeId !== causeId) {
      this.flushAnimation();
      this.anim = {
        causeId,
        first: t,
        last: t,
        frames: 1,
        min: [...values],
        max: [...values],
        seqFrom: seq,
        seqTo: seq,
        latest: [...values],
      };
      return;
    }
    const a = this.anim;
    a.last = t;
    a.frames += 1;
    a.seqTo = seq;
    a.latest = [...values];
    for (let i = 0; i < 8; i++) {
      a.min[i] = Math.min(a.min[i] ?? 0, values[i] ?? 0);
      a.max[i] = Math.max(a.max[i] ?? 0, values[i] ?? 0);
    }
    trace.noteCoalesced(1);
  }

  /** Emits ONE summary record for the accumulated animation frames. */
  flushAnimation(): void {
    const a = this.anim;
    this.anim = null;
    if (!a || a.frames === 0) return;
    trace.record(
      "led.transmitted",
      `animation ${a.causeId} · ${a.frames} frames coalesced · seq ${a.seqFrom}→${a.seqTo}`,
      {
        firstT: a.first,
        lastT: a.last,
        frames: a.frames,
        minPerLed: a.min,
        maxPerLed: a.max,
        latestFrame: a.latest,
        commitSequenceRange: [a.seqFrom, a.seqTo],
      },
      { causeId: a.causeId },
    );
  }

  private startHeartbeat(): void {
    if (this.heartbeatTimer !== null) return;
    if (typeof window === "undefined") return;
    this.heartbeatTimer = setInterval(() => this.heartbeat(), HEARTBEAT_MS);
  }

  /** Counted in statistics; never appended to the ring unless verbose. */
  heartbeat(): void {
    if (!this.canTransmit()) return;
    try {
      this.out!.send([LED_STATUS, CC_HEARTBEAT, this.state.commitSequence]);
    } catch {
      return;
    }
    this.publish({ heartbeats: this.state.heartbeats + 1 });
    if (this.verboseTransport) {
      trace.record("led.transmitted", `heartbeat seq ${this.state.commitSequence}`, {
        cc: CC_HEARTBEAT,
        value: this.state.commitSequence,
      }, { causeId: "led.heartbeat" });
    } else {
      trace.noteCoalesced(1);
    }
  }

  /** CC90 + stop heartbeat. Firmware lease expiry (1 s) is the fallback. */
  release(reason = "explicit release"): void {
    this.flushAnimation();
    if (this.heartbeatTimer !== null) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = null;
    }
    if (this.out && this.state.protocolVersion === LED_PROTOCOL_VERSION) {
      try {
        this.out.send([LED_STATUS, CC_RELEASE, 0]);
        trace.record("led.transmitted", `release CC90 — ${reason}`, { cc: CC_RELEASE }, { causeId: "led.release" });
      } catch {
        /* output already gone; the 1 s firmware lease expiry covers this */
      }
    }
    this.lastValues = null;
    this.lastSignature = null;
    this.publish({ leaseActive: false });
  }

  detach(reason = "detached"): void {
    this.release(reason);
    this.out = null;
    this.publish({
      outputId: null,
      outputName: null,
      protocolVersion: null,
      capabilityQuerySent: false,
      status: this.state.inputConnected ? "no-output" : "no-input",
    });
  }
}

export const ledTransport = new PhysicalLedTransport();

if (typeof window !== "undefined") {
  (window as unknown as { __stemTapeLedTransport?: PhysicalLedTransport }).__stemTapeLedTransport = ledTransport;
}
