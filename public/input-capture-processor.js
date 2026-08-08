/**
 * InputCaptureProcessor — Phase 6A shared capture kernel (plan §C, §C1, §G, M2, M3).
 *
 * ONE instance per project. It is the only audio-thread owner of:
 *   - input metering (RMS + peak, per render quantum, reported by message)
 *   - onset detection (RMS + peak envelope, hysteresis, minimum-duration gate)
 *   - a bounded look-back ring (pre-roll / late punch)
 *   - the preallocated transferable block pool and the write frame counter
 *
 * It NEVER writes storage, never allocates inside process(), and never keeps a
 * two-second buffer: blocks are 4096 frames and the WORKER aggregates them into
 * storage chunks (binding correction M2).
 *
 * Pool exhaustion or a breached high-water mark stops the take immediately and
 * reports `interrupted` — a dropped frame can never produce a `ready` take.
 */

class InputCaptureProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = (options && options.processorOptions) || {};

    this.channels = Math.max(1, Math.min(2, o.channels | 0 || 1));
    this.blockFrames = o.blockFrames | 0 || 4096;
    this.poolSize = o.poolSize | 0 || 8;
    this.preRollFrames = o.preRollFrames | 0 || Math.round(4 * sampleRate);

    // ---- preallocated transferable block pool (never allocated in process())
    /** @type {Float32Array[][]} pool[i][channel] */
    this.pool = [];
    for (let i = 0; i < this.poolSize; i++) {
      const set = [];
      for (let c = 0; c < this.channels; c++) set.push(new Float32Array(this.blockFrames));
      this.pool.push(set);
    }
    this.free = [];
    for (let i = 0; i < this.poolSize; i++) this.free.push(i);
    this.current = -1;
    this.fill = 0;
    this.blocksEmitted = 0;
    this.blocksRecycled = 0;
    this.poolExhaustions = 0;

    // ---- look-back ring
    this.ring = [];
    for (let c = 0; c < this.channels; c++) this.ring.push(new Float32Array(this.preRollFrames));
    this.ringWrite = 0;
    this.ringFilled = 0;

    // ---- metering / onset
    this.rms = 0;
    this.peak = 0;
    this.meterCountdown = 0;
    this.onsetOpenDb = o.onsetOpenDb != null ? o.onsetOpenDb : -38;
    this.onsetCloseDb = o.onsetCloseDb != null ? o.onsetCloseDb : -48;
    this.minDurationFrames = o.minDurationFrames | 0 || Math.round(0.012 * sampleRate);
    this.aboveFrames = 0;

    // ---- state machine mirror (audio thread is authoritative)
    this.state = "idle"; // idle | waiting | recording | stopping
    this.trackId = -1;
    this.startFrame = -1;
    this.stopAtFrame = -1;
    this.preRollRequestFrames = 0;
    this.written = 0;
    this.writerPort = null;
    this.relayMode = true;
    this.highWaterPending = o.highWaterBlocks | 0 || 6;
    this.pendingBlocks = 0;
    this.disposed = false;
    this.monitorThrough = false;

    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  // --------------------------------------------------------------- protocol

  send(msg, transfer) {
    if (transfer) this.port.postMessage(msg, transfer);
    else this.port.postMessage(msg);
  }

  onMessage(msg) {
    if (!msg || typeof msg.type !== "string") return;
    switch (msg.type) {
      case "attachWriter":
        // Direct worklet -> worker MessagePort (no main-thread relay).
        this.writerPort = msg.port;
        this.relayMode = false;
        this.writerPort.onmessage = (e) => this.onWriterMessage(e.data);
        this.send({ type: "ack", seq: msg.seq, status: "applied", detail: "direct worklet→worker port attached", contextFrame: currentFrame });
        return;
      case "recycle": {
        // Relay-mode return path: worker/main hands the buffers back.
        const idx = msg.index | 0;
        const chans = msg.channels || [];
        for (let c = 0; c < chans.length && c < this.channels; c++) {
          this.pool[idx][c] = new Float32Array(chans[c]);
        }
        this.free.push(idx);
        this.blocksRecycled++;
        if (this.pendingBlocks > 0) this.pendingBlocks--;
        return;
      }
      case "arm":
        this.trackId = msg.trackId | 0;
        this.state = "waiting";
        this.aboveFrames = 0;
        this.preRollRequestFrames = Math.min(this.preRollFrames, Math.max(0, msg.preRollFrames | 0));
        this.written = 0;
        this.send({ type: "ack", seq: msg.seq, status: "applied", detail: `armed track ${this.trackId}, waiting for sound`, contextFrame: currentFrame });
        return;
      case "cancel":
        this.state = "idle";
        this.trackId = -1;
        this.send({ type: "ack", seq: msg.seq, status: "applied", detail: "arm cancelled", contextFrame: currentFrame });
        return;
      case "forceStart":
        // Punch / manual start at an exact shared frame.
        this.state = "waiting";
        this.forcedStartFrame = Number(msg.atFrame) || currentFrame;
        this.preRollRequestFrames = Math.min(this.preRollFrames, Math.max(0, msg.preRollFrames | 0));
        this.send({ type: "ack", seq: msg.seq, status: "applied", detail: `forced start armed at ${this.forcedStartFrame}`, contextFrame: currentFrame });
        return;
      case "stop":
        if (this.state === "recording") {
          this.stopAtFrame = Number.isFinite(msg.atFrame) ? Number(msg.atFrame) : currentFrame;
          this.state = "stopping";
        }
        this.send({ type: "ack", seq: msg.seq, status: "applied", detail: `stop scheduled at ${this.stopAtFrame}`, contextFrame: currentFrame });
        return;
      case "setThresholds":
        if (Number.isFinite(msg.openDb)) this.onsetOpenDb = msg.openDb;
        if (Number.isFinite(msg.closeDb)) this.onsetCloseDb = msg.closeDb;
        if (msg.minDurationFrames | 0) this.minDurationFrames = msg.minDurationFrames | 0;
        this.send({ type: "ack", seq: msg.seq, status: "applied", detail: "thresholds updated", contextFrame: currentFrame });
        return;
      case "dispose":
        this.disposed = true;
        this.send({ type: "ack", seq: msg.seq, status: "applied", detail: "capture disposed", contextFrame: currentFrame });
        return;
      default:
        this.send({ type: "ack", seq: msg.seq, status: "rejected", detail: `unknown ${msg.type}`, contextFrame: currentFrame });
    }
  }

  onWriterMessage(data) {
    if (!data) return;
    if (data.type === "recycle") {
      const idx = data.index | 0;
      const chans = data.channels || [];
      for (let c = 0; c < chans.length && c < this.channels; c++) this.pool[idx][c] = new Float32Array(chans[c]);
      this.free.push(idx);
      this.blocksRecycled++;
      if (this.pendingBlocks > 0) this.pendingBlocks--;
    }
  }

  // ------------------------------------------------------------ block plumbing

  acquire() {
    if (this.free.length === 0) return false;
    this.current = this.free.pop();
    this.fill = 0;
    return true;
  }

  emitCurrent(final) {
    if (this.current < 0) return;
    const idx = this.current;
    const set = this.pool[idx];
    const transfer = [];
    const buffers = [];
    for (let c = 0; c < this.channels; c++) {
      buffers.push(set[c].buffer);
      transfer.push(set[c].buffer);
    }
    const msg = {
      type: "block",
      index: idx,
      frames: this.fill,
      channels: buffers,
      startFrameInTake: this.written - this.fill,
      final: !!final,
    };
    if (this.writerPort) this.writerPort.postMessage(msg, transfer);
    else this.port.postMessage(msg, transfer);
    this.blocksEmitted++;
    this.pendingBlocks++;
    this.current = -1;
    this.fill = 0;
  }

  interrupt(reason) {
    this.state = "idle";
    this.send({ type: "interrupted", reason, framesWritten: this.written, contextFrame: currentFrame });
  }

  writeFrame(v0, v1) {
    if (this.current < 0 && !this.acquire()) {
      this.poolExhaustions++;
      this.interrupt("block pool exhausted — storage could not keep up; committed chunks preserved");
      return false;
    }
    const set = this.pool[this.current];
    set[0][this.fill] = v0;
    if (this.channels > 1) set[1][this.fill] = v1;
    this.fill++;
    this.written++;
    if (this.fill >= this.blockFrames) {
      if (this.pendingBlocks >= this.highWaterPending) {
        this.interrupt("write high-water mark breached — storage backpressure");
        return false;
      }
      this.emitCurrent(false);
    }
    return true;
  }

  flushPreRoll(frames) {
    const n = Math.min(frames, this.ringFilled);
    for (let i = n; i > 0; i--) {
      const r = (this.ringWrite - i + this.preRollFrames) % this.preRollFrames;
      if (!this.writeFrame(this.ring[0][r], this.channels > 1 ? this.ring[1][r] : 0)) return false;
    }
    return true;
  }

  // ---------------------------------------------------------------- rendering

  process(inputs, outputs) {
    if (this.disposed) return false;
    const input = inputs[0];
    const out = outputs[0];
    const blockFrames = out && out[0] ? out[0].length : 128;
    const inCh = input && input.length ? input.length : 0;
    const src0 = inCh > 0 ? input[0] : null;
    const src1 = inCh > 1 ? input[1] : src0;

    // The capture node is a sink: monitoring is a SEPARATE graph path (M3).
    if (out) for (let c = 0; c < out.length; c++) out[c].fill(0);

    let sumSq = 0;
    let peak = 0;

    for (let i = 0; i < blockFrames; i++) {
      const a = src0 ? src0[i] : 0;
      const b = src1 ? src1[i] : a;
      const m = this.channels > 1 ? (a + b) * 0.5 : a;
      const abs = m < 0 ? -m : m;
      sumSq += m * m;
      if (abs > peak) peak = abs;

      // look-back ring always runs while the node exists
      this.ring[0][this.ringWrite] = a;
      if (this.channels > 1) this.ring[1][this.ringWrite] = b;
      this.ringWrite = (this.ringWrite + 1) % this.preRollFrames;
      if (this.ringFilled < this.preRollFrames) this.ringFilled++;

      const frameNow = currentFrame + i;

      if (this.state === "waiting") {
        let trigger = false;
        if (this.forcedStartFrame != null && this.forcedStartFrame >= 0 && frameNow >= this.forcedStartFrame) {
          trigger = true;
        } else {
          const level = abs;
          const openLin = Math.pow(10, this.onsetOpenDb / 20);
          const closeLin = Math.pow(10, this.onsetCloseDb / 20);
          if (level >= openLin) this.aboveFrames++;
          else if (level < closeLin) this.aboveFrames = 0;
          if (this.aboveFrames >= this.minDurationFrames) trigger = true;
        }
        if (trigger) {
          this.state = "recording";
          this.startFrame = frameNow;
          this.forcedStartFrame = -1;
          this.written = 0;
          this.send({
            type: "onset",
            trackId: this.trackId,
            contextFrame: frameNow,
            preRollFrames: Math.min(this.preRollRequestFrames, this.ringFilled),
            detail: "onset accepted (RMS+peak hysteresis, min-duration gate)",
          });
          // The attack that triggered us is INSIDE the pre-roll, so the take
          // never clips the transient: flush look-back, then continue live.
          if (!this.flushPreRoll(Math.min(this.preRollRequestFrames, this.ringFilled))) break;
          continue; // this frame already went out with the pre-roll
        }
      }

      if (this.state === "recording" || this.state === "stopping") {
        if (this.state === "stopping" && this.stopAtFrame >= 0 && frameNow >= this.stopAtFrame) {
          this.emitCurrent(true);
          const total = this.written;
          this.state = "idle";
          this.send({ type: "stopped", trackId: this.trackId, framesWritten: total, endFrame: frameNow, startFrame: this.startFrame });
          this.trackId = -1;
          continue;
        }
        if (!this.writeFrame(a, b)) break;
      }
    }

    this.rms = Math.sqrt(sumSq / blockFrames);
    this.peak = peak;
    this.meterCountdown -= blockFrames;
    if (this.meterCountdown <= 0) {
      this.meterCountdown = Math.round(sampleRate / 20);
      this.send({
        type: "meter",
        rms: this.rms,
        peak: this.peak,
        state: this.state,
        trackId: this.trackId,
        framesWritten: this.written,
        contextFrame: currentFrame,
        blocksEmitted: this.blocksEmitted,
        blocksRecycled: this.blocksRecycled,
        poolFree: this.free.length,
        pendingBlocks: this.pendingBlocks,
        poolExhaustions: this.poolExhaustions,
        relayMode: this.relayMode,
      });
    }
    return true;
  }
}

registerProcessor("input-capture-processor", InputCaptureProcessor);
