/**
 * Phase 5C Beat Repeat processor.
 *
 * A bounded per-stem rolling ring buffer. Activation captures the immediately
 * preceding COMPLETED division ending at the activation frame and starts
 * repeating it within one render quantum, seam-crossfaded. If not enough audio
 * has been buffered yet the processor reports `arming` and passes the input
 * through untouched — it never repeats stale memory.
 *
 * The underlying tape playhead is never touched: this processor only observes
 * the audio flowing through it.
 */

const SEAM_FRAMES = 128; // ~2.9 ms at 44.1 kHz — seam crossfade for slice wrap

class BeatRepeatProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const opts = (options && options.processorOptions) || {};
    this.trackId = opts.trackId ?? 0;
    this.capacityFrames = Math.max(1024, Math.floor(opts.capacityFrames || sampleRate));
    this.channels = 2;
    this.ring = [new Float32Array(this.capacityFrames), new Float32Array(this.capacityFrames)];
    this.writeIndex = 0;
    /** How much valid audio has been written since the last clear. */
    this.filled = 0;
    this.active = false;
    this.arming = false;
    this.sliceFrames = 0;
    /** Read cursor within the captured slice. */
    this.sliceRead = 0;
    /** Ring index where the captured slice begins. */
    this.sliceStart = 0;
    this.disposed = false;
    this.port.onmessage = (e) => this.handle(e.data);
    this.port.postMessage({ seq: 0, status: "ready", detail: `beat-repeat ready, capacity ${this.capacityFrames} frames` });
  }

  handle(msg) {
    if (!msg || typeof msg !== "object") return;
    const seq = msg.seq ?? 0;
    switch (msg.type) {
      case "setSlice": {
        const frames = Math.max(1, Math.floor(msg.sliceFrames));
        if (frames > this.capacityFrames) {
          this.port.postMessage({
            seq,
            status: "rejected",
            detail: `slice ${frames} frames exceeds ring capacity ${this.capacityFrames}`,
          });
          return;
        }
        this.sliceFrames = frames;
        if (this.active) this.capture();
        this.port.postMessage({ seq, status: "applied", detail: `slice = ${frames} frames`, sliceFrames: frames });
        return;
      }
      case "activate": {
        if (this.sliceFrames <= 0) {
          this.port.postMessage({ seq, status: "rejected", detail: "no slice length set" });
          return;
        }
        this.active = true;
        this.capture();
        this.port.postMessage({
          seq,
          status: this.arming ? "arming" : "applied",
          detail: this.arming
            ? `arming — ${this.filled}/${this.sliceFrames} frames buffered, one full division required`
            : `repeating ${this.sliceFrames} frames captured at the activation frame`,
          arming: this.arming,
          filled: this.filled,
          sliceFrames: this.sliceFrames,
        });
        return;
      }
      case "deactivate": {
        this.active = false;
        this.arming = false;
        this.port.postMessage({ seq, status: "applied", detail: "released — passthrough restored" });
        return;
      }
      case "clear": {
        this.ring[0].fill(0);
        this.ring[1].fill(0);
        this.filled = 0;
        this.writeIndex = 0;
        this.active = false;
        this.arming = false;
        this.port.postMessage({ seq, status: "applied", detail: "ring cleared" });
        return;
      }
      case "poll": {
        this.port.postMessage({
          seq,
          status: "applied",
          detail: `active=${this.active} arming=${this.arming} filled=${this.filled} slice=${this.sliceFrames}`,
          arming: this.arming,
          filled: this.filled,
          sliceFrames: this.sliceFrames,
        });
        return;
      }
      case "dispose": {
        this.disposed = true;
        this.port.postMessage({ seq, status: "applied", detail: "disposed" });
        return;
      }
      default:
        this.port.postMessage({ seq, status: "rejected", detail: `unknown message ${String(msg.type)}` });
    }
  }

  /** Capture the completed division ending at the current write head. */
  capture() {
    if (this.filled < this.sliceFrames) {
      this.arming = true;
      this.sliceRead = 0;
      return;
    }
    this.arming = false;
    this.sliceStart = (this.writeIndex - this.sliceFrames + this.capacityFrames) % this.capacityFrames;
    this.sliceRead = 0;
  }

  process(inputs, outputs) {
    if (this.disposed) return false;
    const input = inputs[0];
    const output = outputs[0];
    if (!output || output.length === 0) return true;
    const frames = output[0].length;

    // Always keep writing the rolling buffer, active or not.
    for (let f = 0; f < frames; f++) {
      const w = (this.writeIndex + f) % this.capacityFrames;
      for (let c = 0; c < this.channels; c++) {
        const src = input && input[c] ? input[c] : input && input[0] ? input[0] : null;
        this.ring[c][w] = src ? src[f] : 0;
      }
    }
    this.writeIndex = (this.writeIndex + frames) % this.capacityFrames;
    this.filled = Math.min(this.capacityFrames, this.filled + frames);

    const repeating = this.active && !this.arming && this.sliceFrames > 0;
    if (this.active && this.arming && this.filled >= this.sliceFrames) {
      // One full division now exists: leave the visible arming state.
      this.capture();
      this.port.postMessage({
        seq: -1,
        status: "applied",
        detail: `armed — repeating ${this.sliceFrames} frames`,
        arming: false,
        filled: this.filled,
        sliceFrames: this.sliceFrames,
      });
    }

    for (let c = 0; c < output.length; c++) {
      const out = output[c];
      const src = input && input[c] ? input[c] : input && input[0] ? input[0] : null;
      for (let f = 0; f < frames; f++) {
        if (!repeating) {
          out[f] = src ? src[f] : 0;
          continue;
        }
        const readPos = this.sliceRead + f;
        const idx = (this.sliceStart + (readPos % this.sliceFrames)) % this.capacityFrames;
        let sample = this.ring[Math.min(c, this.channels - 1)][idx];
        // Seam crossfade at the slice wrap so the repeat does not click.
        const into = readPos % this.sliceFrames;
        if (this.sliceFrames > SEAM_FRAMES * 2 && into < SEAM_FRAMES) {
          const tailIdx =
            (this.sliceStart + this.sliceFrames - SEAM_FRAMES + into) % this.capacityFrames;
          const x = into / SEAM_FRAMES;
          const g = Math.sin((x * Math.PI) / 2);
          const gTail = Math.cos((x * Math.PI) / 2);
          sample = sample * g + this.ring[Math.min(c, this.channels - 1)][tailIdx] * gTail;
        }
        out[f] = sample;
      }
    }
    if (repeating) this.sliceRead = (this.sliceRead + frames) % this.sliceFrames;
    return true;
  }
}

registerProcessor("beat-repeat-processor", BeatRepeatProcessor);
