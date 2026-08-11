/**
 * Shared recording Web Audio mock.
 *
 * Extracted verbatim from the scrub-handoff acceptance rig so isolation and
 * loop-rejoin tests assert against the SAME schedule model: real start/stop
 * times, real automation curves, real graph edges.
 */

export const SR = 48000;

interface AutoEvent {
  kind: "set" | "linear" | "curve" | "target" | "cancel";
  time: number;
  value: number;
  end?: number;
}

export class MockParam {
  events: AutoEvent[] = [];
  constructor(public value = 0) {}
  setValueAtTime(v: number, t: number) {
    this.events.push({ kind: "set", time: t, value: v });
    return this;
  }
  linearRampToValueAtTime(v: number, t: number) {
    this.events.push({ kind: "linear", time: t, value: v });
    return this;
  }
  exponentialRampToValueAtTime(v: number, t: number) {
    this.events.push({ kind: "linear", time: t, value: v });
    return this;
  }
  setTargetAtTime(v: number, t: number) {
    this.events.push({ kind: "target", time: t, value: v });
    return this;
  }
  setValueCurveAtTime(curve: Float32Array, t: number, dur: number) {
    this.events.push({ kind: "set", time: t, value: curve[0] ?? 0 });
    this.events.push({ kind: "linear", time: t + dur, value: curve[curve.length - 1] ?? 0 });
    return this;
  }
  cancelScheduledValues(t: number) {
    this.events = this.events.filter((e) => e.time < t);
    return this;
  }
  /** Evaluate the automation timeline at `t` (linear segments, held values). */
  at(t: number): number {
    const evs = [...this.events].sort((a, b) => a.time - b.time);
    if (evs.length === 0) return this.value;
    let cur = this.value;
    let curT = -Infinity;
    for (let i = 0; i < evs.length; i++) {
      const e = evs[i]!;
      if (e.time <= t) {
        cur = e.value;
        curT = e.time;
        continue;
      }
      if (e.kind === "linear") {
        const span = e.time - curT;
        if (span <= 0) return e.value;
        const k = Math.min(1, Math.max(0, (t - curT) / span));
        return cur + (e.value - cur) * k;
      }
      return cur;
    }
    return cur;
  }
}

export class MockNode {
  outs: MockNode[] = [];
  connect(dest: MockNode) {
    this.outs.push(dest);
    return dest;
  }
  disconnect() {
    this.outs = [];
  }
}

export class MockGain extends MockNode {
  gain = new MockParam(1);
}

interface Started {
  node: MockSource;
  when: number;
  offset: number;
  duration: number | undefined;
}

export class MockSource extends MockNode {
  buffer: unknown = null;
  playbackRate = new MockParam(1);
  loop = false;
  loopStart = 0;
  loopEnd = 0;
  onended: (() => void) | null = null;
  startedAt: number | null = null;
  startOffset = 0;
  startDuration: number | undefined = undefined;
  stoppedAt: number | null = null;
  constructor(private readonly ctx: MockCtx) {
    super();
  }
  start(when = 0, offset = 0, duration?: number) {
    this.startedAt = when;
    this.startOffset = offset;
    this.startDuration = duration;
    this.ctx.started.push({ node: this, when, offset, duration });
  }
  stop(when = 0) {
    this.stoppedAt = this.stoppedAt == null ? when : Math.min(this.stoppedAt, when);
  }
}

export class MockCtx {
  currentTime = 0;
  sampleRate = SR;
  state = "running";
  destination = new MockNode();
  started: Started[] = [];
  createGain() {
    return new MockGain();
  }
  createBufferSource() {
    return new MockSource(this);
  }
  createBiquadFilter() {
    const n = new MockNode() as MockNode & Record<string, unknown>;
    n["type"] = "lowpass";
    n["frequency"] = new MockParam(20000);
    n["Q"] = new MockParam(1);
    n["gain"] = new MockParam(0);
    n["detune"] = new MockParam(0);
    return n;
  }
  createDelay() {
    const n = new MockNode() as MockNode & Record<string, unknown>;
    n["delayTime"] = new MockParam(0);
    return n;
  }
  createDynamicsCompressor() {
    const n = new MockNode() as MockNode & Record<string, unknown>;
    for (const k of ["threshold", "knee", "ratio", "attack", "release"]) n[k] = new MockParam(0);
    return n;
  }
  createWaveShaper() {
    const n = new MockNode() as MockNode & Record<string, unknown>;
    n["curve"] = null;
    n["oversample"] = "none";
    return n;
  }
  createConstantSource() {
    const n = new MockNode() as MockNode & Record<string, unknown>;
    n["offset"] = new MockParam(0);
    n["start"] = () => {};
    n["stop"] = () => {};
    return n;
  }
  createOscillator() {
    const n = new MockNode() as MockNode & Record<string, unknown>;
    n["frequency"] = new MockParam(440);
    n["type"] = "sine";
    n["start"] = () => {};
    n["stop"] = () => {};
    return n;
  }
  createAnalyser() {
    const n = new MockNode() as MockNode & Record<string, unknown>;
    n["fftSize"] = 256;
    n["getFloatTimeDomainData"] = (a: Float32Array) => a.fill(0);
    n["getByteFrequencyData"] = (a: Uint8Array) => a.fill(0);
    return n;
  }
  createBuffer(channels: number, length: number, sampleRate: number) {
    return makeBuffer(channels, length, sampleRate);
  }
  resume() {
    this.state = "running";
    return Promise.resolve();
  }
  suspend() {
    return Promise.resolve();
  }
  close() {
    return Promise.resolve();
  }
}

/** Impulse-marked stem: a single 1.0 sample every `markEvery` seconds. */
export function makeBuffer(channels: number, length: number, sampleRate: number, markEvery = 0) {
  const data = Array.from({ length: channels }, () => new Float32Array(length));
  if (markEvery > 0) {
    const step = Math.round(markEvery * sampleRate);
    for (const ch of data) for (let i = 0; i < length; i += step) ch[i] = 1;
  }
  return {
    numberOfChannels: channels,
    length,
    sampleRate,
    duration: length / sampleRate,
    getChannelData: (i: number) => data[i]!,
    copyFromChannel: () => {},
    copyToChannel: () => {},
  } as unknown as AudioBuffer;
}

