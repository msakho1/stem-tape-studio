/**
 * Master performance recording (plan §L 6B, M10).
 *
 * Captures EXACTLY what is heard: the master bus, after FX, faders, solo and
 * master gain, through the same capture worklet and the same chunked local
 * storage as an input take. Nothing is uploaded.
 */

import { emptyManifest, verifyDurability, type TakeManifest } from "@/audio/input/takes";

export interface PerformanceRecorderHost {
  ctx: AudioContext;
  /** The node the user actually hears — normally the master analyser. */
  masterTap(): AudioNode | null;
}

export class PerformanceRecorder {
  manifest: TakeManifest | null = null;
  recording = false;
  lastDetail = "master performance recorder idle";
  private capture: AudioWorkletNode | null = null;
  private worker: Worker | null = null;
  private seq = 0;
  private loaded = false;

  constructor(private host: PerformanceRecorderHost) {}

  private async load() {
    if (this.loaded) return;
    await this.host.ctx.audioWorklet.addModule("/input-capture-processor.js");
    this.loaded = true;
  }

  async start(): Promise<{ ok: boolean; detail: string }> {
    const tap = this.host.masterTap();
    if (!tap) return { ok: false, detail: "no master bus yet — start the transport first" };
    await this.load();
    const ctx = this.host.ctx;
    const id = `perf-${Date.now().toString(36)}`;
    this.manifest = emptyManifest({ id, projectId: "current", trackId: -1, sampleRate: ctx.sampleRate, channels: 2 });
    this.manifest.label = "master performance";

    this.worker = new Worker(new URL("../../workers/recordingWorker.ts", import.meta.url), { type: "module" });
    this.worker.onmessage = (e) => this.onWorker(e.data as Record<string, unknown>);
    this.worker.postMessage({ type: "start", takeId: id, channels: 2, sampleRate: ctx.sampleRate, chunkFrames: Math.round(2 * ctx.sampleRate), relay: true });

    this.capture = new AudioWorkletNode(ctx, "input-capture-processor", {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [1],
      processorOptions: { channels: 2, blockFrames: 4096, poolSize: 8, preRollFrames: ctx.sampleRate },
    });
    this.capture.port.onmessage = (e) => {
      const m = e.data as Record<string, unknown>;
      if (m["type"] === "block") this.worker?.postMessage(m, (m["channels"] as ArrayBuffer[]) ?? []);
      if (m["type"] === "interrupted") this.lastDetail = `interrupted: ${String(m["reason"])}`;
    };
    const sink = ctx.createGain();
    sink.gain.value = 0;
    tap.connect(this.capture);
    this.capture.connect(sink);
    sink.connect(ctx.destination);

    const at = Math.round(ctx.currentTime * ctx.sampleRate) + 128;
    this.capture.port.postMessage({ type: "forceStart", seq: ++this.seq, atFrame: at, preRollFrames: 0 });
    this.recording = true;
    this.lastDetail = `recording the master bus from context frame ${at} — exactly what is heard`;
    return { ok: true, detail: this.lastDetail };
  }

  stop(): { ok: boolean; detail: string } {
    if (!this.recording || !this.capture) return { ok: false, detail: "not recording" };
    const ctx = this.host.ctx;
    this.capture.port.postMessage({ type: "stop", seq: ++this.seq, atFrame: Math.round(ctx.currentTime * ctx.sampleRate) + 128 });
    this.recording = false;
    this.lastDetail = "stopping master performance recording";
    return { ok: true, detail: this.lastDetail };
  }

  private onWorker(m: Record<string, unknown>) {
    if (!this.manifest) return;
    if (m["type"] === "chunk") {
      this.manifest.chunks.push({
        index: Number(m["index"]),
        blobKey: String(m["blobKey"]),
        startFrame: Number(m["startFrame"]),
        frames: Number(m["frames"]),
        bytes: Number(m["bytes"]),
      });
      this.manifest.frames = this.manifest.chunks.reduce((n, c) => n + c.frames, 0);
    }
    if (m["type"] === "finalized") {
      this.manifest.frames = Number(m["frames"]);
      const verdict = verifyDurability(this.manifest);
      this.manifest.state = verdict.ok ? "ready" : "interrupted";
      this.manifest.durable = verdict.ok;
      this.manifest.failureReason = verdict.ok ? null : verdict.detail;
      this.lastDetail = `master performance ${verdict.ok ? "ready" : "interrupted"} — ${verdict.detail}`;
      this.capture?.disconnect();
      this.capture = null;
    }
  }
}
