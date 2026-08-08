/**
 * TrackTakeMixerProcessor — Phase 6A paged take playback (binding correction M1).
 *
 * ONE instance per track (four total), NOT one per take. It mixes every enabled
 * take/pass sublayer for its track by reading a bounded, main-thread-supplied
 * page cache. It owns no storage and allocates nothing per frame.
 *
 * A page that is not resident is an UNDERRUN, counted and reported — never
 * silently interpolated over, and never allowed to mark a take playable.
 */

const HALF_PI = Math.PI / 2;

class TrackTakeMixerProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = (options && options.processorOptions) || {};
    this.trackId = o.trackId | 0;
    this.pageFrames = o.pageFrames | 0 || 24000;
    this.readAheadPages = o.readAheadPages | 0 || 2;

    /** layerId -> layer */
    this.layers = new Map();
    this.tapeFrame = 0;
    this.rate = 1;
    this.targetRate = 1;
    this.rateStep = 0;
    this.rampFramesLeft = 0;
    this.direction = 1;
    this.playing = false;

    this.loopEnabled = false;
    this.loopStart = 0;
    this.loopEnd = 0;
    this.crossfadeFrames = Math.round(0.012 * sampleRate);

    this.underruns = 0;
    this.pageMisses = 0;
    this.wrapCount = 0;
    this.scheduled = [];
    this.pendingRequests = new Set();
    this.disposed = false;

    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  reply(seq, status, detail) {
    this.port.postMessage({
      type: "ack",
      seq,
      status,
      detail,
      trackId: this.trackId,
      contextFrame: currentFrame,
      tapeFrame: this.tapeFrame,
      underruns: this.underruns,
      pageMisses: this.pageMisses,
      layers: this.layers.size,
      residentPages: this.residentPageCount(),
    });
  }

  residentPageCount() {
    let n = 0;
    for (const l of this.layers.values()) n += l.pages.size;
    return n;
  }

  onMessage(m) {
    if (!m || typeof m.type !== "string") return;
    switch (m.type) {
      case "addLayer": {
        this.layers.set(m.layerId, {
          id: m.layerId,
          channels: Math.max(1, m.channels | 0 || 1),
          totalFrames: m.totalFrames | 0,
          tapeStartFrame: Number(m.tapeStartFrame) || 0,
          enabled: m.enabled !== false,
          gain: Number.isFinite(m.gain) ? m.gain : 1,
          pages: new Map(),
          passIndex: m.passIndex | 0,
        });
        this.reply(m.seq, "applied", `layer ${m.layerId} added (${m.totalFrames} frames @ tape ${m.tapeStartFrame})`);
        return;
      }
      case "setLayerEnabled": {
        const l = this.layers.get(m.layerId);
        if (l) l.enabled = !!m.enabled;
        this.reply(m.seq, l ? "applied" : "rejected", `layer ${m.layerId} enabled=${!!m.enabled}`);
        return;
      }
      case "removeLayer": {
        this.layers.delete(m.layerId);
        this.reply(m.seq, "applied", `layer ${m.layerId} removed`);
        return;
      }
      case "page": {
        const l = this.layers.get(m.layerId);
        if (!l) return;
        const chans = [];
        for (let c = 0; c < m.channels.length; c++) chans.push(new Float32Array(m.channels[c]));
        l.pages.set(m.pageIndex | 0, chans);
        this.pendingRequests.delete(`${m.layerId}:${m.pageIndex}`);
        return;
      }
      case "evict": {
        const l = this.layers.get(m.layerId);
        if (l) l.pages.delete(m.pageIndex | 0);
        return;
      }
      case "setLoop":
        this.loopEnabled = !!m.enabled;
        this.loopStart = Number(m.startFrame) || 0;
        this.loopEnd = Number(m.endFrame) || 0;
        this.reply(m.seq, "applied", `loop ${this.loopStart}..${this.loopEnd} enabled=${this.loopEnabled}`);
        return;
      case "anchor":
        this.tapeFrame = Number(m.tapeFrame) || 0;
        this.reply(m.seq, "applied", `anchored at tape frame ${this.tapeFrame}`);
        return;
      case "dispose":
        this.disposed = true;
        this.layers.clear();
        this.reply(m.seq, "applied", "take mixer disposed");
        this.port.close();
        return;
      case "poll":
        this.reply(m.seq, "applied", "poll");
        return;
      default: {
        const at = Number(m.applyAtContextFrame);
        if (!Number.isFinite(at)) {
          this.reply(m.seq, "rejected", `${m.type} without applyAtContextFrame`);
          return;
        }
        this.scheduled.push({ frame: at, msg: m });
        this.scheduled.sort((a, b) => a.frame - b.frame);
        this.reply(m.seq, "applied", `${m.type} scheduled at ${at}`);
      }
    }
  }

  apply(m) {
    switch (m.type) {
      case "start":
        if (Number.isFinite(m.tapeFrame)) this.tapeFrame = m.tapeFrame;
        this.playing = true;
        break;
      case "stop":
        this.playing = false;
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
      case "setDirection":
        this.direction = m.direction < 0 ? -1 : 1;
        break;
      default:
        break;
    }
  }

  requestPage(layerId, pageIndex) {
    const key = `${layerId}:${pageIndex}`;
    if (this.pendingRequests.has(key)) return;
    this.pendingRequests.add(key);
    this.port.postMessage({ type: "needPage", trackId: this.trackId, layerId, pageIndex, contextFrame: currentFrame });
  }

  /** Linear-interpolated read from the resident page cache; null page = underrun. */
  readLayer(layer, takePos, ch) {
    if (takePos < 0 || takePos >= layer.totalFrames) return 0;
    const i = takePos | 0;
    const f = takePos - i;
    const page = i / this.pageFrames | 0;
    const data = layer.pages.get(page);
    if (!data) {
      this.pageMisses++;
      this.underruns++;
      this.requestPage(layer.id, page);
      return null;
    }
    const arr = data[Math.min(ch, data.length - 1)];
    const off = i - page * this.pageFrames;
    const y1 = arr[off];
    let y2;
    if (off + 1 < arr.length) y2 = arr[off + 1];
    else {
      const nx = layer.pages.get(page + 1);
      if (!nx) {
        this.requestPage(layer.id, page + 1);
        y2 = y1;
      } else y2 = nx[Math.min(ch, nx.length - 1)][0];
    }
    return y1 + (y2 - y1) * f;
  }

  process(_inputs, outputs) {
    if (this.disposed) return false;
    const out = outputs[0];
    if (!out || out.length === 0) return true;
    const blockFrames = out[0].length;
    const outCh = out.length;

    while (this.scheduled.length > 0 && this.scheduled[0].frame <= currentFrame) this.apply(this.scheduled.shift().msg);

    if (!this.playing || this.layers.size === 0) {
      for (let c = 0; c < outCh; c++) out[c].fill(0);
      return true;
    }

    const loopLen = this.loopEnd - this.loopStart;
    const looping = this.loopEnabled && loopLen > 2;
    const xf = Math.min(this.crossfadeFrames, looping ? Math.floor(loopLen * 0.25) : this.crossfadeFrames);

    for (let i = 0; i < blockFrames; i++) {
      while (this.scheduled.length > 0 && this.scheduled[0].frame <= currentFrame + i) this.apply(this.scheduled.shift().msg);
      if (this.rampFramesLeft > 0) {
        this.rate += this.rateStep;
        this.rampFramesLeft--;
        if (this.rampFramesLeft === 0) this.rate = this.targetRate;
      }

      let a = 1;
      let b = 0;
      let tailTape = 0;
      if (looping && xf > 0) {
        const dist = this.direction > 0 ? this.loopEnd - this.tapeFrame : this.tapeFrame - this.loopStart;
        if (dist < xf) {
          const t = (xf - dist) / xf;
          a = Math.cos(t * HALF_PI);
          b = Math.sin(t * HALF_PI);
          tailTape = this.direction > 0 ? this.tapeFrame - loopLen : this.tapeFrame + loopLen;
        }
      }

      for (let c = 0; c < outCh; c++) {
        let sum = 0;
        for (const layer of this.layers.values()) {
          if (!layer.enabled) continue;
          const v = this.readLayer(layer, this.tapeFrame - layer.tapeStartFrame, c);
          if (v != null) sum += v * layer.gain * a;
          if (b > 0) {
            const vt = this.readLayer(layer, tailTape - layer.tapeStartFrame, c);
            if (vt != null) sum += vt * layer.gain * b;
          }
        }
        out[c][i] = sum;
      }

      this.tapeFrame += this.rate * this.direction;
      if (looping) {
        if (this.direction > 0 && this.tapeFrame >= this.loopEnd) {
          this.tapeFrame -= loopLen;
          this.wrapCount++;
        } else if (this.direction < 0 && this.tapeFrame < this.loopStart) {
          this.tapeFrame += loopLen;
          this.wrapCount++;
        }
      }
    }

    // Read-ahead: ask for the pages we are about to need (before wrap too).
    for (const layer of this.layers.values()) {
      if (!layer.enabled) continue;
      const here = this.tapeFrame - layer.tapeStartFrame;
      for (let k = 0; k <= this.readAheadPages; k++) {
        const p = ((here + k * this.pageFrames * this.direction) / this.pageFrames) | 0;
        if (p >= 0 && p * this.pageFrames < layer.totalFrames && !layer.pages.has(p)) this.requestPage(layer.id, p);
      }
      if (looping) {
        const wrapPos = (this.direction > 0 ? this.loopStart : this.loopEnd) - layer.tapeStartFrame;
        const p = (wrapPos / this.pageFrames) | 0;
        if (p >= 0 && p * this.pageFrames < layer.totalFrames && !layer.pages.has(p)) this.requestPage(layer.id, p);
      }
    }
    return true;
  }
}

registerProcessor("track-take-mixer-processor", TrackTakeMixerProcessor);
