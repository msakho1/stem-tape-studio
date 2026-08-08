/**
 * TapeProcessor — Phase 5B source-generating AudioWorkletProcessor.
 *
 * One instance per track. It owns that track's PCM (transferred Float32Arrays),
 * generates output with a fractional read pointer and Hermite interpolation,
 * supports negative movement (zero-copy reverse), wraps inside the active loop
 * with an internal equal-power crossfade, and applies every structural change
 * at an exact shared context frame.
 *
 * Render-loop rules honoured here:
 *  - the render quantum is READ from the output array every call, never assumed
 *    to be 128 (see MDN AudioWorkletProcessor.process());
 *  - no allocation, no logging, no object creation, no array resizing inside
 *    the per-frame loop. Every scratch value is a preallocated field;
 *  - process() returns true while the processor remains usable.
 *
 * Gain, mute, filter and master stay OUTSIDE, on the existing node graph.
 */

const TWO_OVER_PI = Math.PI / 2;

class TapeProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = (options && options.processorOptions) || {};
    this.trackId = o.trackId | 0;

    /** @type {Float32Array[]} */
    this.channels = [];
    this.sourceFrames = 0;
    this.sourceSampleRate = sampleRate;
    /** Resampling ratio when the PCM rate differs from the context rate. */
    this.rateScale = 1;

    this.readPosition = 0;
    this.rate = 1;
    this.targetRate = 1;
    this.rateStep = 0;
    this.rampFramesLeft = 0;
    this.direction = 1;
    this.playing = false;

    this.windowStart = 0;
    this.windowEnd = 0;
    this.chopDiv = 1;
    this.chopIndex = 0;
    this.loopStart = 0;
    this.loopEnd = 0;
    this.loopEnabled = false;
    this.loopMode = "fixed";
    this.crossfadeFrames = Math.round(0.012 * sampleRate);

    /** @type {{frame:number, msg:any}[]} preallocated-ish FIFO, never resized in process() */
    this.scheduledCommands = [];

    this.adopted = false;
    this.rendered = false;
    this.wrapCount = 0;
    this.renderGapFrames = 0;
    this.expectedNextFrame = -1;
    this.forceErrorAt = -1;
    this.disposed = false;
    /**
     * Heads mode: four virtual heads reading THIS SAME PCM at fractional
     * offsets of the active cycle. Null when heads mode is off. No PCM copy is
     * ever made; reverse is a negative read step.
     * @type {null | {cycleStart:number, cycleFrames:number, heads:{offset:number,level:number,muted:boolean,reverse:boolean}[]}}
     */
    this.heads = null;

    /**
     * Per-head phase anchor. `phi = phase - anchor`, so setting the anchor to
     * the current phase makes a head read exactly `offset * cycleFrames` at
     * that instant — this is how a released scrub resumes normal playback from
     * the scrubbed position without touching the underlying transport.
     */
    this.headAnchors = [0, 0, 0, 0];
    /**
     * Per-head scrub voice. Non-null only while a pointer is scrubbing head i.
     * Preallocated shape; no object is created inside process().
     * @type {(null | {pointerId:number, target:number, actual:number, velocity:number, gain:number, mix:number, releasing:boolean, previews:number, savedOffset:number, savedAnchor:number})[]}
     */
    this.headScrubs = [null, null, null, null];
    this.scrubLag = Math.max(1, 0.03 * sampleRate);
    this.scrubXfadeFrames = Math.max(1, Math.round(0.012 * sampleRate));
    this.maxScrubRate = 32;
    this.scrubSilenceRate = 0.02;
    /** Telemetry accumulators (read by the diagnostics panel). */
    this.telemetryCountdown = 0;
    this.rmsAcc = 0;
    this.rmsFrames = 0;

    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  /** Current derived read position (absolute source frames) of head i. */
  headReadFrame(i) {
    if (!this.heads) return this.readPosition;
    const cf = this.heads.cycleFrames;
    const cs = this.heads.cycleStart;
    const hd = this.heads.heads[i];
    if (!hd) return this.readPosition;
    let phase = (this.readPosition - cs - this.headAnchors[i]) % cf;
    if (phase < 0) phase += cf;
    const off = hd.offset * cf;
    let p = hd.reverse ? (off - phase) % cf : (off + phase) % cf;
    if (p < 0) p += cf;
    return cs + p;
  }


  // ------------------------------------------------------------- protocol

  reply(seq, status, detail, appliedAtContextFrame, resultingSourceFrame) {
    this.port.postMessage({
      seq,
      status,
      detail,
      appliedAtContextFrame,
      resultingSourceFrame,
      trackId: this.trackId,
      wrapCount: this.wrapCount,
      renderGapFrames: this.renderGapFrames,
      rate: this.rate,
      direction: this.direction,
      playing: this.playing,
    });
  }

  onMessage(msg) {
    if (!msg || typeof msg.type !== "string") return;
    switch (msg.type) {
      case "adopt": {
        const meta = msg.metadata || {};
        const chans = msg.channels || [];
        this.channels.length = 0;
        for (let i = 0; i < chans.length; i++) this.channels.push(new Float32Array(chans[i]));
        this.sourceFrames = meta.frames | 0;
        this.sourceSampleRate = meta.sampleRate || sampleRate;
        this.rateScale = this.sourceSampleRate / sampleRate;
        this.windowStart = 0;
        this.windowEnd = this.sourceFrames;
        this.recomputeLoop();
        this.adopted = this.channels.length > 0 && this.sourceFrames > 0;
        this.reply(
          msg.seq,
          this.adopted ? "ready" : "failed",
          this.adopted
            ? `adopted ${this.channels.length}ch × ${this.sourceFrames} frames @ ${this.sourceSampleRate} Hz (owned, zero-copy)`
            : "adopt received no usable channel data",
          currentFrame,
          this.readPosition,
        );
        return;
      }
      case "prepareHandoff": {
        this.readPosition = Number(msg.sourceFrame) || 0;
        this.reply(msg.seq, "ready", `pointer pre-positioned at source frame ${this.readPosition}`, currentFrame, this.readPosition);
        return;
      }
      case "poll": {
        this.reply(msg.seq, "applied", "poll", currentFrame, this.readPosition);
        return;
      }
      case "setHeads": {
        // Immediate: heads config is a routing layer, not a transport change.
        this.heads = msg.heads
          ? {
              cycleStart: Number(msg.cycleStart) || 0,
              cycleFrames: Math.max(1, Number(msg.cycleFrames) || 1),
              heads: msg.heads,
            }
          : null;
        this.reply(
          msg.seq,
          "applied",
          this.heads ? `heads on — 4 pointers over one PCM, cycle ${this.heads.cycleFrames} frames` : "heads off — single pointer restored",
          currentFrame,
          this.readPosition,
        );
        return;
      }
      case "readRange": {
        // Copy a source range out for PRINT rendering. Copies are made here
        // because the PCM itself is owned by this processor after adopt.
        const start = Math.max(0, Math.round(Number(msg.start) || 0));
        const len = Math.max(0, Math.round(Number(msg.frames) || 0));
        const chans = [];
        for (let c = 0; c < this.channels.length; c++) {
          const src = this.channels[c];
          const dst = new Float32Array(len);
          for (let i = 0; i < len; i++) dst[i] = start + i < src.length ? src[start + i] : 0;
          chans.push(dst.buffer);
        }
        this.port.postMessage(
          { seq: msg.seq, status: "applied", detail: `read ${len} frames from ${start}`, channels: chans, frames: len, trackId: this.trackId, readAtContextFrame: currentFrame },
          chans,
        );
        return;
      }

      case "dispose": {
        this.disposed = true;
        this.playing = false;
        this.channels.length = 0;
        this.reply(msg.seq, "applied", "disposed — PCM released, port closing", currentFrame, this.readPosition);
        this.port.close();
        return;
      }
      case "__forceError": {
        this.forceErrorAt = currentFrame + (msg.inFrames | 0);
        this.reply(msg.seq, "applied", "forced failure armed", currentFrame, this.readPosition);
        return;
      }
      default: {
        if (!this.adopted) {
          this.reply(msg.seq, "rejected", `${msg.type} before adopt`, currentFrame, this.readPosition);
          return;
        }
        const at = Number(msg.applyAtContextFrame);
        if (!Number.isFinite(at)) {
          this.reply(msg.seq, "rejected", `${msg.type} without applyAtContextFrame`, currentFrame, this.readPosition);
          return;
        }
        // Ordered insert by frame; commands arrive rarely, never in process().
        this.scheduledCommands.push({ frame: at, msg });
        this.scheduledCommands.sort((a, b) => a.frame - b.frame);
        this.reply(msg.seq, "applied", `${msg.type} scheduled for context frame ${at}`, at, this.readPosition);
      }
    }
  }

  recomputeLoop() {
    const a = Math.min(this.windowStart, this.windowEnd);
    const b = Math.max(this.windowStart, this.windowEnd);
    const width = Math.max(1, b - a);
    const div = this.chopDiv < 1 ? 1 : this.chopDiv | 0;
    const idx = ((this.chopIndex % div) + div) % div;
    const slice = width / div;
    this.loopStart = a + idx * slice;
    this.loopEnd = this.loopStart + slice;
    const maxFade = Math.max(1, Math.floor(slice * 0.25));
    this.crossfadeFrames = Math.min(Math.round(0.012 * sampleRate), maxFade);
  }

  applyCommand(m) {
    switch (m.type) {
      case "start":
        if (Number.isFinite(m.sourceFrame)) this.readPosition = m.sourceFrame;
        this.playing = true;
        break;
      case "stop":
        this.playing = false;
        break;
      case "restart":
        this.readPosition = this.loopEnabled ? this.loopStart : 0;
        this.playing = true;
        break;
      case "setRate": {
        this.targetRate = m.rate;
        const frames = m.rampFrames | 0;
        if (frames > 0) {
          this.rampFramesLeft = frames;
          this.rateStep = (this.targetRate - this.rate) / frames;
        } else {
          this.rate = this.targetRate;
          this.rampFramesLeft = 0;
          this.rateStep = 0;
        }
        break;
      }
      case "setWindow":
        this.windowStart = m.start;
        this.windowEnd = m.end;
        this.loopEnabled = m.enabled !== false;
        this.recomputeLoop();
        if (this.loopEnabled && (this.readPosition < this.loopStart || this.readPosition >= this.loopEnd)) {
          this.readPosition = this.loopStart;
        }
        break;
      case "setChop":
        this.chopDiv = m.division;
        this.chopIndex = m.index;
        this.loopEnabled = true;
        this.recomputeLoop();
        if (this.readPosition < this.loopStart || this.readPosition >= this.loopEnd) this.readPosition = this.loopStart;
        break;
      case "setLoopMode":
        this.loopMode = m.mode === "variable" ? "variable" : "fixed";
        break;
      case "setDirection":
        this.direction = m.direction < 0 ? -1 : 1;
        break;
      default:
        break;
    }
  }

  // ------------------------------------------------------------- rendering

  /** Hermite (Catmull-Rom) interpolation; silence outside the source range. */
  sampleAt(ch, pos) {
    const n = this.sourceFrames;
    if (pos < 0 || pos >= n) return 0;
    const i = pos | 0;
    const f = pos - i;
    const y0 = i > 0 ? ch[i - 1] : ch[i];
    const y1 = ch[i];
    const y2 = i + 1 < n ? ch[i + 1] : ch[i];
    const y3 = i + 2 < n ? ch[i + 2] : y2;
    const c0 = y1;
    const c1 = 0.5 * (y2 - y0);
    const c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3;
    const c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + c0;
  }

  process(_inputs, outputs) {
    if (this.disposed) return false;

    const out = outputs[0];
    if (!out || out.length === 0) return true;
    // Render quantum is read, never assumed.
    const blockFrames = out[0].length;

    if (this.expectedNextFrame >= 0 && currentFrame > this.expectedNextFrame) {
      this.renderGapFrames += currentFrame - this.expectedNextFrame;
    }
    this.expectedNextFrame = currentFrame + blockFrames;

    if (this.forceErrorAt >= 0 && currentFrame >= this.forceErrorAt) {
      this.forceErrorAt = -1;
      throw new Error(`TapeProcessor ${this.trackId}: forced failure for recovery testing`);
    }

    const outChannels = out.length;
    const srcChannels = this.channels.length;

    if (!this.adopted) {
      for (let c = 0; c < outChannels; c++) out[c].fill(0);
      return true;
    }

    // Drain any commands due before this block starts.
    while (this.scheduledCommands.length > 0 && this.scheduledCommands[0].frame <= currentFrame) {
      this.applyCommand(this.scheduledCommands.shift().msg);
    }

    if (!this.playing) {
      for (let c = 0; c < outChannels; c++) out[c].fill(0);
      this.rendered = true;
      return true;
    }

    const xf = this.crossfadeFrames;
    const loopLen = this.loopEnd - this.loopStart;
    const looping = this.loopEnabled && loopLen > 2;

    for (let i = 0; i < blockFrames; i++) {
      // Sub-block precision: a command scheduled mid-quantum applies exactly.
      while (this.scheduledCommands.length > 0 && this.scheduledCommands[0].frame <= currentFrame + i) {
        this.applyCommand(this.scheduledCommands.shift().msg);
      }

      if (this.rampFramesLeft > 0) {
        this.rate += this.rateStep;
        this.rampFramesLeft--;
        if (this.rampFramesLeft === 0) this.rate = this.targetRate;
      }

      let a = 1;
      let b = 0;
      let tailPos = 0;
      if (looping && xf > 0) {
        const dist = this.direction > 0 ? this.loopEnd - this.readPosition : this.readPosition - this.loopStart;
        if (dist < xf) {
          const t = (xf - dist) / xf;
          a = Math.cos(t * TWO_OVER_PI);
          b = Math.sin(t * TWO_OVER_PI);
          tailPos = this.direction > 0 ? this.readPosition - loopLen : this.readPosition + loopLen;
        }
      }

      for (let c = 0; c < outChannels; c++) {
        const src = this.channels[srcChannels === 1 ? 0 : Math.min(c, srcChannels - 1)];
        let v;
        if (this.heads) {
          // Four heads, one PCM. Every head position is DERIVED from this
          // block's readPosition, so no head can accumulate drift and the
          // underlying pointer stays phase-correct when heads mode exits.
          const cf = this.heads.cycleFrames;
          const cs = this.heads.cycleStart;
          let phase = (this.readPosition - cs) % cf;
          if (phase < 0) phase += cf;
          v = 0;
          for (let h = 0; h < this.heads.heads.length; h++) {
            const hd = this.heads.heads[h];
            if (hd.muted || hd.level <= 0) continue;
            const off = hd.offset * cf;
            let p = hd.reverse ? (off - phase) % cf : (off + phase) % cf;
            if (p < 0) p += cf;
            v += this.sampleAt(src, cs + p) * hd.level;
          }
          if (v > 1) v = 1;
          else if (v < -1) v = -1;
        } else {
          v = this.sampleAt(src, this.readPosition) * a;
          if (b > 0) v += this.sampleAt(src, tailPos) * b;
        }
        out[c][i] = v;
      }


      this.readPosition += this.rate * this.rateScale * this.direction;

      if (looping) {
        if (this.direction > 0 && this.readPosition >= this.loopEnd) {
          this.readPosition -= loopLen;
          this.wrapCount++;
        } else if (this.direction < 0 && this.readPosition < this.loopStart) {
          this.readPosition += loopLen;
          this.wrapCount++;
        }
      }
    }

    this.rendered = true;
    return true;
  }
}

registerProcessor("tape-processor", TapeProcessor);
