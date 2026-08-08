/**
 * RecordingController — Phase 6 capture, overdub and take playback runtime.
 *
 * Owns: the MediaStream, the shared `input-capture-processor`, the recording
 * worker, the per-track `track-take-mixer-processor` nodes, the take page
 * worker and the project page budget. The reducer in `recordingState.ts` owns
 * the state table; this class only executes and reports.
 *
 * Invariants enforced here:
 *  - permission is requested only from an explicit user action (M6)
 *  - capture and monitoring are independent paths, monitor default OFF (M3)
 *  - one armed external-input target at a time (M5)
 *  - a take with any missing frame is never marked ready (M2 / §C1)
 *  - no audio leaves the device: storage is OPFS/IndexedDB only
 */

import {
  emptyManifest,
  verifyDurability,
  type TakeManifest,
  type TakeTimelineSegment,
} from "./takes";
import { initialRecordingModel, recordingReduce, type RecIntent, type RecordingModel, type TrackContent } from "@/machine/recordingState";
import { listInputs, rawConstraints, readSettings, type AppliedSettings, type InputDeviceInfo } from "./inputDevices";
import { compensationFrames, estimateLatency, latencyStatement, type LatencyModel } from "./latency";
import { TakePageBudgetManager } from "./takePages";
import { chunkKey } from "./chunkStore";

export type MonitorMode = "off" | "dry" | "fx";

/** The slice of the engine the recorder is allowed to touch. */
export interface RecorderHost {
  ctx: AudioContext;
  /** Pre-FX track input (tape sources land here too). */
  inputNodeFor(track: number): AudioNode | null;
  /** Post-FX, pre-fader monitoring sum — still passes fader and solo. */
  faderNodeFor(track: number): AudioNode | null;
  /** Current tape coordinate in FRAMES for that track. */
  tapeFrameFor(track: number): number;
  /** Loop bounds in frames, or null when the track is not looping. */
  loopFramesFor(track: number): { start: number; end: number } | null;
  trackContent(track: number): TrackContent;
  currentRate(): number;
}

export interface RecorderSnapshot {
  supported: boolean;
  inputEnabled: boolean;
  monitor: MonitorMode;
  monitorCeiling: number;
  devices: InputDeviceInfo[];
  settings: AppliedSettings | null;
  meter: { rms: number; peak: number };
  model: RecordingModel;
  takes: TakeManifest[];
  latency: LatencyModel;
  latencyStatement: string;
  transferPath: "direct-port" | "main-thread-relay" | "none";
  blocksEmitted: number;
  blocksRecycled: number;
  poolFree: number;
  poolExhaustions: number;
  pendingBlocks: number;
  budget: ReturnType<TakePageBudgetManager["snapshot"]>;
  lastAck: string;
  log: string[];
  privacy: string;
}

const BLOCK_FRAMES = 4096;
const POOL_SIZE = 8;
const PRE_ROLL_S = 4;
const CHUNK_S = 2;
const PAGE_FRAMES = 24000;
/** Hard ceiling on the monitor path — feedback protection (M3). */
export const MONITOR_CEILING = 0.7;

export class RecordingController {
  model: RecordingModel;
  takes: TakeManifest[] = [];
  monitor: MonitorMode = "off";
  meter = { rms: 0, peak: 0 };
  latency: LatencyModel;
  readonly log: string[] = [];
  devices: InputDeviceInfo[] = [];
  settings: AppliedSettings | null = null;
  transferPath: "direct-port" | "main-thread-relay" | "none" = "none";
  budget: TakePageBudgetManager;

  private stream: MediaStream | null = null;
  private source: MediaStreamAudioSourceNode | null = null;
  private capture: AudioWorkletNode | null = null;
  private monitorFx: GainNode | null = null;
  private monitorDry: GainNode | null = null;
  private recWorker: Worker | null = null;
  private pageWorker: Worker | null = null;
  private mixers = new Map<number, AudioWorkletNode>();
  private modulesLoaded = false;
  private seq = 0;
  private current: { takeId: string; track: number; startContextFrame: number; segments: TakeTimelineSegment[] } | null = null;
  private telemetry = { blocksEmitted: 0, blocksRecycled: 0, poolFree: POOL_SIZE, poolExhaustions: 0, pendingBlocks: 0 };
  private listeners = new Set<() => void>();

  constructor(private host: RecorderHost, budgetBytes = 48 * 1024 * 1024) {
    this.model = initialRecordingModel([0, 1, 2, 3].map((i) => host.trackContent(i)));
    this.latency = estimateLatency(host.ctx, null);
    this.budget = new TakePageBudgetManager(budgetBytes, PAGE_FRAMES, 1, 2);
  }

  onChange(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  private changed() {
    for (const l of this.listeners) l();
  }

  private note(line: string) {
    this.log.unshift(`${new Date().toLocaleTimeString()} · ${line}`);
    this.log.length = Math.min(this.log.length, 60);
  }

  private dispatch(intent: RecIntent) {
    this.model = recordingReduce(this.model, intent);
    this.note(this.model.lastAck);
    this.changed();
  }

  setOverlay(open: boolean) {
    this.model = { ...this.model, fxOverlay: open };
  }

  // ------------------------------------------------------------- lifecycle

  private async loadModules() {
    if (this.modulesLoaded) return;
    await this.host.ctx.audioWorklet.addModule("/input-capture-processor.js");
    await this.host.ctx.audioWorklet.addModule("/take-mixer-processor.js");
    this.modulesLoaded = true;
  }

  /** ONLY from an explicit Enable Input tap (M6). */
  async enableInput(deviceId?: string): Promise<{ ok: boolean; detail: string }> {
    if (typeof navigator === "undefined" || !navigator.mediaDevices?.getUserMedia)
      return { ok: false, detail: "this browser exposes no microphone input API" };
    try {
      await this.loadModules();
      this.stream = await navigator.mediaDevices.getUserMedia(rawConstraints(deviceId));
      this.settings = readSettings(this.stream);
      this.devices = await listInputs();
      this.latency = estimateLatency(this.host.ctx, this.settings.latencyS);

      const ctx = this.host.ctx;
      this.source = ctx.createMediaStreamSource(this.stream);
      this.capture = new AudioWorkletNode(ctx, "input-capture-processor", {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [1],
        processorOptions: {
          channels: 1,
          blockFrames: BLOCK_FRAMES,
          poolSize: POOL_SIZE,
          preRollFrames: Math.round(PRE_ROLL_S * ctx.sampleRate),
          minDurationFrames: Math.round(0.012 * ctx.sampleRate),
        },
      });
      this.capture.port.onmessage = (e) => this.onCaptureMessage(e.data as Record<string, unknown>);
      this.capture.onprocessorerror = () => {
        this.abortTake("capture processor error — committed chunks preserved");
      };
      // Capture path is a sink: connect to a zero-gain node so it renders.
      const sink = ctx.createGain();
      sink.gain.value = 0;
      this.source.connect(this.capture);
      this.capture.connect(sink);
      sink.connect(ctx.destination);

      // Monitoring: two independent, default-closed paths (M3).
      this.monitorFx = ctx.createGain();
      this.monitorDry = ctx.createGain();
      this.monitorFx.gain.value = 0;
      this.monitorDry.gain.value = 0;
      this.source.connect(this.monitorFx);
      this.source.connect(this.monitorDry);
      this.routeMonitor();

      this.startWorkers();
      this.dispatch({ type: "rec.inputEnabled" });
      return { ok: true, detail: `input enabled — ${this.settings.channelCount ?? 1}ch @ ${this.settings.sampleRate ?? ctx.sampleRate} Hz${this.settings.ignored.length ? `, browser ignored ${this.settings.ignored.join(", ")}` : ", raw constraints honoured"}` };
    } catch (e) {
      this.dispatch({ type: "rec.inputDenied" });
      return { ok: false, detail: `input denied or unavailable: ${(e as Error).message}` };
    }
  }

  disableInput() {
    this.stream?.getTracks().forEach((t) => t.stop());
    this.stream = null;
    this.source?.disconnect();
    this.capture?.disconnect();
    this.source = null;
    this.capture = null;
    this.model = { ...this.model, inputEnabled: false, target: null };
    this.note("input disabled — microphone released");
    this.changed();
  }

  private startWorkers() {
    if (!this.recWorker) {
      this.recWorker = new Worker(new URL("../../workers/recordingWorker.ts", import.meta.url), { type: "module" });
      this.recWorker.onmessage = (e) => this.onWorkerMessage(e.data as Record<string, unknown>);
    }
    if (!this.pageWorker) {
      this.pageWorker = new Worker(new URL("../../workers/takePageWorker.ts", import.meta.url), { type: "module" });
      this.pageWorker.onmessage = (e) => this.onPageMessage(e.data as Record<string, unknown>);
    }
    // Direct worklet → worker port when the platform allows transferring it.
    try {
      const chan = new MessageChannel();
      this.capture?.port.postMessage({ type: "attachWriter", seq: ++this.seq, port: chan.port1 }, [chan.port1]);
      this.recWorker.postMessage({ type: "attachWorklet", port: chan.port2 }, [chan.port2]);
      this.transferPath = "direct-port";
    } catch {
      this.transferPath = "main-thread-relay";
      this.note("direct worklet→worker port unavailable — pooled blocks relay through the main thread (transfer preserved)");
    }
  }

  setMonitor(mode: MonitorMode) {
    this.monitor = mode;
    this.routeMonitor();
    this.note(
      mode === "off"
        ? "monitoring off"
        : `monitoring ${mode === "dry" ? "dry (post-FX sum, pre-fader)" : "through the stem FX rack"} at a ${MONITOR_CEILING.toFixed(2)} ceiling — use headphones`,
    );
    this.changed();
  }

  private routeMonitor() {
    const track = this.model.target ?? 0;
    const fxIn = this.host.inputNodeFor(track);
    const fader = this.host.faderNodeFor(track);
    try {
      this.monitorFx?.disconnect();
      this.monitorDry?.disconnect();
    } catch {
      /* not connected */
    }
    if (this.monitorFx && fxIn) this.monitorFx.connect(fxIn);
    if (this.monitorDry && fader) this.monitorDry.connect(fader);
    // Exactly one open at a time.
    if (this.monitorFx) this.monitorFx.gain.value = this.monitor === "fx" ? MONITOR_CEILING : 0;
    if (this.monitorDry) this.monitorDry.gain.value = this.monitor === "dry" ? MONITOR_CEILING : 0;
  }

  // -------------------------------------------------------------- commands

  arm(track: number) {
    const before = this.model.tracks[track]?.phase;
    this.dispatch({ type: "rec.arm", track });
    const after = this.model.tracks[track]?.phase;
    if (after === "waiting-for-sound" && before !== after) {
      this.capture?.port.postMessage({
        type: "arm",
        seq: ++this.seq,
        trackId: track,
        preRollFrames: Math.round(0.25 * this.host.ctx.sampleRate),
      });
      this.routeMonitor();
    }
  }

  tap(track: number) {
    const phase = this.model.tracks[track]?.phase;
    this.dispatch({ type: "rec.tap", track });
    if (phase === "waiting-for-sound" || phase === "waiting-for-grid") this.capture?.port.postMessage({ type: "cancel", seq: ++this.seq });
    if (phase === "recording" || phase === "overdubbing") this.requestStop(track);
  }

  doubleTap(track: number) {
    this.dispatch({ type: "rec.doubleTap", track });
  }

  private requestStop(track: number) {
    const ctx = this.host.ctx;
    const loop = this.host.loopFramesFor(track);
    const now = Math.round(ctx.currentTime * ctx.sampleRate);
    let at = now + Math.round(0.02 * ctx.sampleRate);
    let why = "immediate stop through the approved anti-click fade";
    if (loop) {
      const tape = this.host.tapeFrameFor(track);
      const remaining = Math.max(0, loop.end - tape) / Math.max(0.01, this.host.currentRate());
      at = now + Math.round(remaining);
      why = `stops at the next loop seam in ${Math.round(remaining)} frames`;
    }
    this.capture?.port.postMessage({ type: "stop", seq: ++this.seq, atFrame: at });
    this.note(`rec.stop track ${track + 1} — ${why}`);
  }

  private abortTake(reason: string) {
    const track = this.current?.track ?? this.model.target;
    this.recWorker?.postMessage({ type: "abort", reason });
    if (track != null) this.dispatch({ type: "rec.interrupted", track, reason });
  }

  // ------------------------------------------------------- worklet messages

  private onCaptureMessage(m: Record<string, unknown>) {
    switch (m["type"]) {
      case "meter": {
        this.meter = { rms: Number(m["rms"]), peak: Number(m["peak"]) };
        this.telemetry = {
          blocksEmitted: Number(m["blocksEmitted"] ?? 0),
          blocksRecycled: Number(m["blocksRecycled"] ?? 0),
          poolFree: Number(m["poolFree"] ?? 0),
          poolExhaustions: Number(m["poolExhaustions"] ?? 0),
          pendingBlocks: Number(m["pendingBlocks"] ?? 0),
        };
        const track = Number(m["trackId"]);
        if (track >= 0 && Number(m["framesWritten"]) > 0) {
          this.model = recordingReduce(this.model, { type: "rec.progress", track, frames: Number(m["framesWritten"]) });
        }
        this.changed();
        return;
      }
      case "onset": {
        const track = Number(m["trackId"]);
        const contextFrame = Number(m["contextFrame"]);
        const takeId = `${Date.now().toString(36)}-t${track}`;
        const sr = this.host.ctx.sampleRate;
        this.current = {
          takeId,
          track,
          startContextFrame: contextFrame,
          segments: [
            {
              contextStartFrame: contextFrame,
              contextEndFrame: contextFrame,
              takeStartFrame: 0,
              tapeStartFrame: this.host.tapeFrameFor(track),
              direction: 1,
              rate: { kind: "constant", value: this.host.currentRate() },
              loopIteration: 0,
            },
          ],
        };
        this.takes = [
          ...this.takes,
          emptyManifest({ id: takeId, projectId: "current", trackId: track, sampleRate: sr, channels: 1 }),
        ];
        this.recWorker?.postMessage({ type: "start", takeId, channels: 1, sampleRate: sr, chunkFrames: Math.round(CHUNK_S * sr), relay: this.transferPath !== "direct-port" });
        this.dispatch({ type: "rec.onset", track, contextFrame, takeId });
        return;
      }
      case "block":
        // Relay path only: forward the transferables to the worker untouched.
        this.recWorker?.postMessage(m, (m["channels"] as ArrayBuffer[]) ?? []);
        return;
      case "stopped": {
        const track = Number(m["trackId"]);
        if (track >= 0) this.dispatch({ type: "rec.finalizing", track });
        this.finishSegments(Number(m["endFrame"]));
        this.recWorker?.postMessage({ type: "finalize" });
        return;
      }
      case "interrupted":
        this.abortTake(String(m["reason"]));
        return;
    }
  }

  private finishSegments(endFrame: number) {
    if (!this.current) return;
    const last = this.current.segments[this.current.segments.length - 1];
    if (last) last.contextEndFrame = endFrame;
  }

  private onWorkerMessage(m: Record<string, unknown>) {
    switch (m["type"]) {
      case "chunk": {
        const take = this.takes.find((t) => t.id === this.current?.takeId);
        if (!take) return;
        take.chunks = [
          ...take.chunks,
          { index: Number(m["index"]), blobKey: String(m["blobKey"]), startFrame: Number(m["startFrame"]), frames: Number(m["frames"]), bytes: Number(m["bytes"]) },
        ];
        take.frames = take.chunks.reduce((n, c) => n + c.frames, 0);
        this.changed();
        return;
      }
      case "finalized":
        void this.finalizeTake(String(m["takeId"]), Number(m["frames"]));
        return;
      case "aborted": {
        const take = this.takes.find((t) => t.id === String(m["takeId"]));
        if (take) {
          take.state = "interrupted";
          take.failureReason = String(m["reason"]);
          take.durable = false;
        }
        this.changed();
        return;
      }
      case "failed":
        this.abortTake(String(m["reason"]));
        return;
    }
  }

  private async finalizeTake(takeId: string, frames: number) {
    const take = this.takes.find((t) => t.id === takeId);
    const cur = this.current;
    if (!take || !cur) return;
    take.frames = frames;
    take.passes = [
      {
        passIndex: 0,
        startFrame: 0,
        frames,
        tapeStartFrame: cur.segments[0]?.tapeStartFrame ?? 0,
        segments: cur.segments,
      },
    ];
    take.latencyCompFrames = compensationFrames(this.latency, this.host.ctx.sampleRate);
    const verdict = verifyDurability(take);
    if (!verdict.ok) {
      take.state = "interrupted";
      take.failureReason = verdict.detail;
      this.dispatch({ type: "rec.interrupted", track: cur.track, reason: verdict.detail });
      this.current = null;
      return;
    }
    take.state = "ready";
    take.durable = true;
    this.note(`take ${takeId} finalised — ${verdict.detail}`);
    await this.activateTake(take);
    this.dispatch({ type: "rec.ready", track: cur.track });
    this.current = null;
  }

  // --------------------------------------------------------- take playback

  async activateTake(take: TakeManifest): Promise<{ ok: boolean; detail: string }> {
    if (take.state !== "ready") return { ok: false, detail: `take ${take.id} is ${take.state} — not playable` };
    const gate = this.budget.activate(take.id);
    if (!gate.ok) return gate;
    await this.loadModules();
    const mixer = this.mixerFor(take.trackId);
    this.pageWorker?.postMessage({
      type: "registerLayer",
      layerId: take.id,
      channels: take.channels,
      chunks: take.chunks.map((c) => ({ index: c.index, blobKey: c.blobKey || chunkKey(take.id, c.index), startFrame: c.startFrame, frames: c.frames })),
    });
    mixer.port.postMessage({
      type: "addLayer",
      seq: ++this.seq,
      layerId: take.id,
      channels: take.channels,
      totalFrames: take.frames,
      tapeStartFrame: (take.passes[0]?.tapeStartFrame ?? 0) - take.latencyCompFrames,
      enabled: take.enabled,
      gain: take.gain,
      passIndex: 0,
    });
    const ctx = this.host.ctx;
    mixer.port.postMessage({ type: "anchor", seq: ++this.seq, tapeFrame: this.host.tapeFrameFor(take.trackId) });
    mixer.port.postMessage({ type: "start", seq: ++this.seq, applyAtContextFrame: Math.round(ctx.currentTime * ctx.sampleRate) + 128, tapeFrame: this.host.tapeFrameFor(take.trackId) });
    return { ok: true, detail: `${take.id} active — ${gate.detail}` };
  }

  private mixerFor(track: number): AudioWorkletNode {
    const existing = this.mixers.get(track);
    if (existing) return existing;
    const ctx = this.host.ctx;
    const node = new AudioWorkletNode(ctx, "track-take-mixer-processor", {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [1],
      processorOptions: { trackId: track, pageFrames: PAGE_FRAMES, readAheadPages: 2 },
    });
    node.port.onmessage = (e) => this.onMixerMessage(track, e.data as Record<string, unknown>);
    const dest = this.host.inputNodeFor(track);
    if (dest) node.connect(dest);
    this.mixers.set(track, node);
    return node;
  }

  private onMixerMessage(track: number, m: Record<string, unknown>) {
    if (m["type"] === "needPage") {
      const layerId = String(m["layerId"]);
      const pageIndex = Number(m["pageIndex"]);
      this.budget.noteMiss();
      this.pageWorker?.postMessage({ type: "requestPage", layerId, pageIndex, pageFrames: PAGE_FRAMES });
      return;
    }
    if (m["type"] === "ack" && Number(m["underruns"]) > 0) this.budget.noteUnderrun(0);
    void track;
  }

  private onPageMessage(m: Record<string, unknown>) {
    if (m["type"] === "page") {
      const layerId = String(m["layerId"]);
      const take = this.takes.find((t) => t.id === layerId);
      if (!take) return;
      const mixer = this.mixers.get(take.trackId);
      const buffers = m["channels"] as ArrayBuffer[];
      mixer?.port.postMessage({ type: "page", layerId, pageIndex: Number(m["pageIndex"]), channels: buffers }, buffers);
      for (const ev of this.budget.admit({ trackId: take.trackId, layerId, pageIndex: Number(m["pageIndex"]) })) {
        mixer?.port.postMessage({ type: "evict", layerId: ev.layerId, pageIndex: ev.pageIndex });
      }
      return;
    }
    if (m["type"] === "pageMissing") {
      this.budget.noteUnderrun();
      this.note(`page ${m["pageIndex"]} missing for ${m["layerId"]}: ${m["reason"]} — reported, never silently filled`);
      this.changed();
    }
  }

  setTakeEnabled(takeId: string, enabled: boolean): { ok: boolean; detail: string } {
    const take = this.takes.find((t) => t.id === takeId);
    if (!take) return { ok: false, detail: "no such take" };
    if (enabled) {
      const gate = this.budget.activate(takeId);
      if (!gate.ok) return gate;
    } else this.budget.deactivate(takeId);
    take.enabled = enabled;
    this.mixers.get(take.trackId)?.port.postMessage({ type: "setLayerEnabled", seq: ++this.seq, layerId: takeId, enabled });
    this.changed();
    return { ok: true, detail: `${takeId} ${enabled ? "enabled" : "muted"}` };
  }

  /** Rate/direction follow — the take mixer stays on the tape timeline. */
  followRate(track: number, rate: number, rampFrames = 0) {
    const ctx = this.host.ctx;
    this.mixers.get(track)?.port.postMessage({
      type: "setRate",
      seq: ++this.seq,
      rate,
      rampFrames,
      applyAtContextFrame: Math.round(ctx.currentTime * ctx.sampleRate) + 128,
    });
  }

  followTransport(track: number, playing: boolean) {
    const ctx = this.host.ctx;
    this.mixers.get(track)?.port.postMessage({
      type: playing ? "start" : "stop",
      seq: ++this.seq,
      tapeFrame: this.host.tapeFrameFor(track),
      applyAtContextFrame: Math.round(ctx.currentTime * ctx.sampleRate) + 128,
    });
  }

  snapshot(): RecorderSnapshot {
    return {
      supported: typeof navigator !== "undefined" && !!navigator.mediaDevices?.getUserMedia,
      inputEnabled: this.model.inputEnabled,
      monitor: this.monitor,
      monitorCeiling: MONITOR_CEILING,
      devices: this.devices,
      settings: this.settings,
      meter: this.meter,
      model: this.model,
      takes: this.takes,
      latency: this.latency,
      latencyStatement: latencyStatement(this.latency, this.host.ctx?.sampleRate ?? 48000),
      transferPath: this.transferPath,
      ...this.telemetry,
      budget: this.budget.snapshot(),
      lastAck: this.model.lastAck,
      log: [...this.log],
      privacy: "captured audio is written to this device only (OPFS / IndexedDB) — no take, chunk or export is ever sent over the network",
    };
  }

  dispose() {
    this.disableInput();
    for (const m of this.mixers.values()) {
      m.port.postMessage({ type: "dispose", seq: ++this.seq });
      m.disconnect();
    }
    this.mixers.clear();
    this.recWorker?.terminate();
    this.pageWorker?.terminate();
    this.recWorker = null;
    this.pageWorker = null;
  }
}
