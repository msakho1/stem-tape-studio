/**
 * Phase 5B per-track worklet controller.
 *
 * Owns one AudioWorkletNode, the sequenced port protocol, PCM ownership
 * transfer, the phase-aligned handoff, processorerror recovery and teardown.
 * It NEVER touches gain, mute, filter or master: it connects into the existing
 * per-track `input` node so the whole Phase 5A external graph still applies.
 */

import { equalPower, sampleCurve, SEAM_FADE_S } from "./crossfade";
import {
  DRIFT_TOLERANCE_FRAMES,
  PROCESSOR_NAME,
  PROCESSOR_URL,
  sharedApplyFrame,
  type BufferMetadata,
  type WorkletAck,
  type WorkletCommand,
  type WorkletMessage,
} from "./workletProtocol";

export type MigrationStatus =
  | "node"
  | "checking"
  | "adopting"
  | "handoff"
  | "worklet"
  | "refused"
  | "failed"
  | "recovering";

export interface PreflightResult {
  ok: boolean;
  checks: { name: string; ok: boolean; detail: string }[];
}

const loadedModules = new WeakSet<BaseAudioContext>();

/** Availability + secure-context + module-load preflight. Never silent. */
export async function preflightWorklet(ctx: BaseAudioContext | null): Promise<PreflightResult> {
  const checks: PreflightResult["checks"] = [];
  const push = (name: string, ok: boolean, detail: string) => checks.push({ name, ok, detail });

  const hasCtx = ctx != null;
  push("audio-context", hasCtx, hasCtx ? "AudioContext exists" : "audio not unlocked");
  const secure = typeof window !== "undefined" ? window.isSecureContext : false;
  push("secure-context", secure, secure ? "secure context" : "AudioWorklet requires a secure context");
  const available = hasCtx && typeof (ctx as AudioContext).audioWorklet !== "undefined" && typeof AudioWorkletNode !== "undefined";
  push("audioworklet-available", available, available ? "AudioWorklet + AudioWorkletNode present" : "AudioWorklet unsupported — staying on the Phase 5A node engine");

  if (!hasCtx || !secure || !available) return { ok: false, checks };

  if (loadedModules.has(ctx)) {
    push("module-load", true, `${PROCESSOR_URL} already registered`);
    return { ok: true, checks };
  }
  try {
    await (ctx as AudioContext).audioWorklet.addModule(PROCESSOR_URL);
    loadedModules.add(ctx);
    push("module-load", true, `registered "${PROCESSOR_NAME}" from ${PROCESSOR_URL}`);
    return { ok: true, checks };
  } catch (err) {
    push("module-load", false, err instanceof Error ? err.message : String(err));
    return { ok: false, checks };
  }
}

/** Copy channel data into freshly allocated, transferable arrays. */
export function copyOwnedChannels(buffer: AudioBuffer): { arrays: Float32Array[]; metadata: BufferMetadata; bytes: number } {
  const arrays: Float32Array[] = [];
  for (let c = 0; c < buffer.numberOfChannels; c++) {
    const arr = new Float32Array(buffer.length);
    buffer.copyFromChannel(arr, c);
    arrays.push(arr);
  }
  return {
    arrays,
    metadata: {
      frames: buffer.length,
      channels: buffer.numberOfChannels,
      sampleRate: buffer.sampleRate,
      durationS: buffer.duration,
    },
    bytes: buffer.length * buffer.numberOfChannels * 4,
  };
}

export class WorkletTrack {
  node: AudioWorkletNode | null = null;
  /** Crossfade gain for the worklet leg of the handoff. */
  gain: GainNode | null = null;
  status: MigrationStatus = "node";
  fallbackReason: string | null = null;
  lastAck: WorkletAck | null = null;
  /** Most recent unsolicited scrub telemetry frame from the kernel. */
  lastTelemetry: WorkletAck | null = null;
  onTelemetry: ((t: WorkletTrack, ack: WorkletAck) => void) | null = null;

  lastSourceFrame: number | null = null;
  driftFrames: number | null = null;
  peakMigrationBytes = 0;
  pcmBytes = 0;
  wrapCount = 0;
  renderGapFrames = 0;
  errored = false;
  recreateAttempts = 0;
  readonly acks: WorkletAck[] = [];

  private seq = 0;
  private pending = new Map<number, (ack: WorkletAck) => void>();

  constructor(
    readonly trackId: number,
    private ctx: AudioContext,
    private destination: AudioNode,
    private onError: (t: WorkletTrack, detail: string) => void,
  ) {}

  private nextSeq() {
    return ++this.seq;
  }

  private send(msg: WorkletMessage, transfer?: Transferable[]): Promise<WorkletAck> {
    const node = this.node;
    if (!node) return Promise.resolve({ seq: msg.seq, status: "failed", detail: "no worklet node" });
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.pending.delete(msg.seq);
        resolve({ seq: msg.seq, status: "failed", detail: `no acknowledgement for ${msg.type} within 2000 ms` });
      }, 2000);
      this.pending.set(msg.seq, (ack) => {
        clearTimeout(timer);
        resolve(ack);
      });
      if (transfer) node.port.postMessage(msg, transfer);
      else node.port.postMessage(msg);
    });
  }

  create(): { ok: boolean; detail: string } {
    try {
      const node = new AudioWorkletNode(this.ctx, PROCESSOR_NAME, {
        numberOfInputs: 0,
        numberOfOutputs: 1,
        outputChannelCount: [2],
        processorOptions: { trackId: this.trackId },
      });
      const gain = this.ctx.createGain();
      gain.gain.value = 0;
      node.connect(gain);
      gain.connect(this.destination);
      node.port.onmessage = (e: MessageEvent<WorkletAck>) => this.receive(e.data);
      // Once processorerror fires the node outputs silence forever: it is never
      // reused, only replaced or abandoned to the node engine.
      node.onprocessorerror = () => {
        this.errored = true;
        this.status = "failed";
        this.fallbackReason = "processorerror — this node outputs silence permanently and will not be reused";
        this.onError(this, this.fallbackReason);
      };
      this.node = node;
      this.gain = gain;
      return { ok: true, detail: `AudioWorkletNode created for track ${this.trackId + 1}` };
    } catch (err) {
      const detail = err instanceof Error ? err.message : String(err);
      this.fallbackReason = detail;
      this.status = "failed";
      return { ok: false, detail };
    }
  }

  private receive(ack: WorkletAck) {
    if (ack.status === "telemetry") {
      // Telemetry is unsolicited (seq -1) and must never resolve a pending
      // command or displace the last command ack.
      this.lastTelemetry = ack;
      if (typeof ack.renderGapFrames === "number") this.renderGapFrames = ack.renderGapFrames;
      this.onTelemetry?.(this, ack);
      return;
    }
    this.lastAck = ack;
    this.acks.unshift(ack);
    if (this.acks.length > 40) this.acks.length = 40;
    if (typeof ack.resultingSourceFrame === "number") this.lastSourceFrame = ack.resultingSourceFrame;
    if (typeof ack.wrapCount === "number") this.wrapCount = ack.wrapCount;
    if (typeof ack.renderGapFrames === "number") this.renderGapFrames = ack.renderGapFrames;
    const fn = this.pending.get(ack.seq);
    if (fn) {
      this.pending.delete(ack.seq);
      fn(ack);
    }
  }


  /** Transfer PCM ownership and wait for the readiness handshake. */
  async adopt(buffer: AudioBuffer): Promise<WorkletAck> {
    this.status = "adopting";
    const { arrays, metadata, bytes } = copyOwnedChannels(buffer);
    this.pcmBytes = bytes;
    // While the node buffer and the owned arrays coexist, this track's PCM is
    // resident twice. The gate upstream is conservative about the whole project.
    this.peakMigrationBytes = bytes * 2;
    const buffers = arrays.map((a) => a.buffer as ArrayBuffer);
    const ack = await this.send({ type: "adopt", seq: this.nextSeq(), channels: buffers, metadata }, buffers);
    if (ack.status !== "ready") {
      this.status = "failed";
      this.fallbackReason = ack.detail;
    }
    return ack;
  }

  /**
   * Phase-aligned handoff. The node leg fades out while the worklet leg fades
   * in over one equal-power seam at a shared, future context frame.
   */
  async handoff(
    sourceFrame: number,
    fadeOutNode: (at: number) => void,
    applyFrame?: number,
  ): Promise<{ ok: boolean; detail: string; driftFrames: number; applyAtContextFrame: number }> {
    this.status = "handoff";
    const sr = this.ctx.sampleRate;
    const at = applyFrame ?? sharedApplyFrame(this.ctx);
    const prep = await this.send({ type: "prepareHandoff", seq: this.nextSeq(), sourceFrame });
    if (prep.status !== "ready") {
      this.status = "failed";
      this.fallbackReason = prep.detail;
      return { ok: false, detail: prep.detail, driftFrames: Number.NaN, applyAtContextFrame: at };
    }
    const ack = await this.send({ type: "start", seq: this.nextSeq(), applyAtContextFrame: at, sourceFrame });
    if (ack.status !== "applied") {
      this.status = "failed";
      this.fallbackReason = ack.detail;
      return { ok: false, detail: ack.detail, driftFrames: Number.NaN, applyAtContextFrame: at };
    }

    const atTime = at / sr;
    const g = this.gain!;
    g.gain.cancelScheduledValues(this.ctx.currentTime);
    g.gain.setValueAtTime(0, Math.max(this.ctx.currentTime, atTime - SEAM_FADE_S));
    g.gain.setValueCurveAtTime(sampleCurve(equalPower, "b"), Math.max(this.ctx.currentTime, atTime - SEAM_FADE_S), SEAM_FADE_S);
    g.gain.setValueAtTime(1, atTime);
    fadeOutNode(Math.max(this.ctx.currentTime, atTime - SEAM_FADE_S));

    const drift = Math.abs((ack.resultingSourceFrame ?? sourceFrame) - sourceFrame);
    this.driftFrames = drift;
    if (drift > DRIFT_TOLERANCE_FRAMES) {
      this.status = "failed";
      this.fallbackReason = `phase drift ${drift} frames exceeds ±${DRIFT_TOLERANCE_FRAMES}`;
      return { ok: false, detail: this.fallbackReason, driftFrames: drift, applyAtContextFrame: at };
    }
    this.status = "worklet";
    return {
      ok: true,
      detail: `handoff at context frame ${at} (t=${atTime.toFixed(4)}s), source frame ${sourceFrame}, drift ${drift} frames, equal-power crossfade ${SEAM_FADE_S * 1000} ms`,
      driftFrames: drift,
      applyAtContextFrame: at,
    };
  }

  post(msg: WorkletCommand): Promise<WorkletAck> {
    return this.send({ ...msg, seq: this.nextSeq() } as WorkletMessage);
  }

  async poll(): Promise<WorkletAck> {
    return this.send({ type: "poll", seq: this.nextSeq() });
  }

  /** Heads routing layer. `null` restores the single pointer, phase intact. */
  async setHeads(
    heads: { offset: number; level: number; muted: boolean; reverse: boolean }[] | null,
    cycleStart: number,
    cycleFrames: number,
  ): Promise<WorkletAck> {
    return this.send({ type: "setHeads", seq: this.nextSeq(), heads, cycleStart, cycleFrames });
  }

  /** Audible head scrub lifecycle (start → preview* → end | cancel). */
  async headScrub(args: {
    head: number;
    phase: "start" | "preview" | "end" | "cancel";
    pointerId: number;
    normalizedPosition: number;
    deltaFrames: number;
  }): Promise<WorkletAck> {
    return this.send({ type: "headScrub", seq: this.nextSeq(), ...args });
  }

  /** Copy one source range out of the processor (PRINT reads it this way). */
  async readRange(start: number, frames: number): Promise<{ ok: boolean; channels: Float32Array[]; detail: string }> {
    const ack = await this.send({ type: "readRange", seq: this.nextSeq(), start, frames });
    if (ack.status !== "applied" || !ack.channels) return { ok: false, channels: [], detail: ack.detail };
    return { ok: true, channels: ack.channels.map((b) => new Float32Array(b)), detail: ack.detail };
  }



  async forceError(inFrames = 0): Promise<WorkletAck> {
    return this.send({ type: "__forceError", seq: this.nextSeq(), inFrames });
  }

  /** Final teardown: dispose the processor, close the port, drop the node. */
  async dispose(): Promise<void> {
    if (this.node) {
      if (!this.errored) await this.send({ type: "dispose", seq: this.nextSeq() });
      try {
        this.node.port.onmessage = null;
        this.node.port.close();
      } catch {
        /* already closed by the processor */
      }
      try {
        this.node.disconnect();
      } catch {
        /* noop */
      }
    }
    try {
      this.gain?.disconnect();
    } catch {
      /* noop */
    }
    this.node = null;
    this.gain = null;
    this.pending.clear();
    this.pcmBytes = 0;
    if (this.status === "worklet" || this.status === "handoff") this.status = "node";
  }
}
