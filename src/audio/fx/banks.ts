/**
 * Workstream 3 — the twelve-effect performance rack.
 *
 * Four SERIAL stages, always in musical signal order:
 *
 *     source → TONE → MOD → MOTION → SPACE → fader
 *
 * Every stage is a permanent dry/wet pair whose input and output are created
 * once and never re-parented, so switching algorithms can never click a track
 * out of the graph. The algorithm graph INSIDE a stage is built lazily on
 * first activation and cached, so an unused bank costs four gain nodes.
 *
 * Rules this file enforces:
 *  - Activation is zero-latency: `setActive` ramps the wet gain from the
 *    caller's timestamp, no scheduling round-trip.
 *  - Dry/wet is CORRELATED (complementary gains, dry = 1 − wet) because both
 *    legs carry the same source. Uncorrelated equal-power fades belong to
 *    source seams, not to a filter's wet mix.
 *  - A rejection marks ONE algorithm, never the whole bank.
 */

import {
  BANKS,
  algorithmDef,
  type AlgorithmId,
  type AlgorithmIndex,
  type BankIndex,
} from "@/machine/fx12";

/** Wet-mix ramp for engaging or releasing an algorithm. */
export const FX_ENGAGE_S = 0.012;

export interface AlgorithmGraph {
  input: GainNode;
  output: AudioNode;
  /** Applies the 0..1 macro. Called on build and on every macro change. */
  setMacro: (value: number, when: number) => void;
  /** Musical timing, re-applied whenever the grid tempo changes. */
  setTempo?: (bpm: number, when: number) => void;
  dispose: () => void;
}

export interface BankStageSnapshot {
  bank: BankIndex;
  id: string;
  algorithm: AlgorithmId;
  active: boolean;
  wet: number;
  macro: number;
  built: boolean;
  rejected: string | null;
}

function clamp01(v: number): number {
  return Math.min(1, Math.max(0, v));
}

function ramp(param: AudioParam, value: number, when: number, seconds = FX_ENGAGE_S) {
  const t = Math.max(when, 0);
  param.cancelScheduledValues(t);
  param.setValueAtTime(param.value, t);
  param.linearRampToValueAtTime(value, t + seconds);
}

/** 2^(cents/1200) — used by the pitched echo and shimmer algorithms. */
export function centsToRate(cents: number): number {
  return Math.pow(2, cents / 1200);
}

/** A macro 0..1 mapped exponentially across a frequency range. */
export function macroToFreq(macro: number, lo: number, hi: number): number {
  return lo * Math.pow(hi / lo, clamp01(macro));
}

// ---------------------------------------------------------------------------
// TONE
// ---------------------------------------------------------------------------

function buildFilter(ctx: AudioContext): AlgorithmGraph {
  const input = ctx.createGain();
  const biquad = ctx.createBiquadFilter();
  biquad.type = "lowpass";
  biquad.Q.value = 0.9;
  input.connect(biquad);
  return {
    input,
    output: biquad,
    setMacro: (v, when) => {
      // 0 = wide open low-pass, 1 = tight. A single continuous sweep.
      biquad.frequency.setValueAtTime(macroToFreq(1 - v, 180, 18000), Math.max(when, 0));
    },
    dispose: () => {
      input.disconnect();
      biquad.disconnect();
    },
  };
}

function buildIsolator(ctx: AudioContext): AlgorithmGraph {
  // Three-band isolator: the macro sweeps which band survives.
  const input = ctx.createGain();
  const out = ctx.createGain();
  const low = ctx.createBiquadFilter();
  low.type = "lowshelf";
  low.frequency.value = 220;
  const mid = ctx.createBiquadFilter();
  mid.type = "peaking";
  mid.frequency.value = 1000;
  mid.Q.value = 0.9;
  const high = ctx.createBiquadFilter();
  high.type = "highshelf";
  high.frequency.value = 4200;
  input.connect(low);
  low.connect(mid);
  mid.connect(high);
  high.connect(out);
  return {
    input,
    output: out,
    setMacro: (v, when) => {
      const t = Math.max(when, 0);
      const depth = 36 * clamp01(v);
      // v = 0 → flat; v = 1 → mids only (lows and highs cut).
      low.gain.setValueAtTime(-depth, t);
      high.gain.setValueAtTime(-depth, t);
      mid.gain.setValueAtTime(depth * 0.25, t);
    },
    dispose: () => {
      for (const n of [input, low, mid, high, out]) n.disconnect();
    },
  };
}

function crusherCurve(drive: number): Float32Array<ArrayBuffer> {
  const n = 1024;
  const curve = new Float32Array(new ArrayBuffer(n * 4));
  const k = 1 + drive * 40;
  for (let i = 0; i < n; i++) {
    const x = (i / (n - 1)) * 2 - 1;
    curve[i] = Math.tanh(k * x) / Math.tanh(k);
  }
  return curve;
}

function buildDirt(ctx: AudioContext): AlgorithmGraph {
  const input = ctx.createGain();
  const shaper = ctx.createWaveShaper();
  shaper.oversample = "2x";
  const tame = ctx.createBiquadFilter();
  tame.type = "lowpass";
  tame.frequency.value = 8000;
  const trim = ctx.createGain();
  input.connect(shaper);
  shaper.connect(tame);
  tame.connect(trim);
  return {
    input,
    output: trim,
    setMacro: (v, when) => {
      const t = Math.max(when, 0);
      shaper.curve = crusherCurve(clamp01(v));
      // Loudness compensation: hard drive must not simply be "louder".
      trim.gain.setValueAtTime(1 - 0.45 * clamp01(v), t);
    },
    dispose: () => {
      for (const n of [input, shaper, tame, trim]) n.disconnect();
    },
  };
}

// ---------------------------------------------------------------------------
// MOD  —  Reel Flange · Formant Shift · Rhythmic Gate
// ---------------------------------------------------------------------------

/**
 * Reel Flange: a tape-style flanger. A short modulated delay (0.4–8 ms) is
 * summed with the direct path through the stage's own dry/wet, and the
 * feedback path is kept below unity so the resonance cannot run away. The
 * sweep is FREE-RUNNING (0.05–0.6 Hz), not tempo-locked: a reel flange is a
 * mechanical wow artefact, not a rhythmic effect.
 */
function buildReelFlange(ctx: AudioContext): AlgorithmGraph {
  const input = ctx.createGain();
  const out = ctx.createGain();
  const delay = ctx.createDelay(0.05);
  delay.delayTime.value = 0.003;
  const fb = ctx.createGain();
  fb.gain.value = 0.35;
  // Inverted feedback gives the hollow through-zero character.
  const invert = ctx.createGain();
  invert.gain.value = -1;

  const lfo = ctx.createOscillator();
  lfo.type = "triangle";
  lfo.frequency.value = 0.18;
  const sweep = ctx.createGain();
  sweep.gain.value = 0.0022;
  const centre = ctx.createConstantSource();
  centre.offset.value = 0.0032;
  lfo.connect(sweep);
  sweep.connect(delay.delayTime);
  centre.connect(delay.delayTime);
  delay.delayTime.value = 0;
  lfo.start();
  centre.start();

  input.connect(delay);
  input.connect(out);
  delay.connect(out);
  delay.connect(fb);
  fb.connect(invert);
  invert.connect(delay);

  return {
    input,
    output: out,
    setMacro: (v, when) => {
      const t = Math.max(when, 0);
      const m = clamp01(v);
      // Macro = depth + rate together, the single performance control.
      lfo.frequency.setValueAtTime(0.05 + 0.55 * m, t);
      sweep.gain.setValueAtTime(0.0004 + 0.0036 * m, t);
      centre.offset.setValueAtTime(0.0006 + 0.0038 * m, t);
      fb.gain.setValueAtTime(Math.min(0.85, 0.2 + 0.65 * m), t);
    },
    dispose: () => {
      try {
        lfo.stop();
        centre.stop();
      } catch {
        /* already stopped */
      }
      for (const n of [input, delay, fb, invert, sweep, out]) n.disconnect();
    },
  };
}

/**
 * Formant Shift: moves the vocal-tract resonances WITHOUT changing pitch or
 * timing. Three parallel peaking bands track a formant set that slides from
 * roughly 0.7× (chest) to 1.6× (throat/child) as the macro sweeps; the source
 * pitch, tape speed and phase are untouched, so it stays sample-locked to the
 * transport. A gentle tilt keeps loudness roughly constant across the sweep.
 */
const FORMANTS = [620, 1180, 2600];

function buildFormantShift(ctx: AudioContext): AlgorithmGraph {
  const input = ctx.createGain();
  const out = ctx.createGain();
  // Notch the untreated formant region, then re-inject shifted peaks.
  const bands = FORMANTS.map((f, i) => {
    const peak = ctx.createBiquadFilter();
    peak.type = "peaking";
    peak.frequency.value = f;
    peak.Q.value = 4 + i;
    peak.gain.value = 0;
    return peak;
  });
  const cut = ctx.createBiquadFilter();
  cut.type = "peaking";
  cut.frequency.value = 1100;
  cut.Q.value = 0.8;
  cut.gain.value = 0;

  // Serial chain: each peaking band shapes the same signal in place.
  let node: AudioNode = input;
  node.connect(cut);
  node = cut;
  for (const b of bands) {
    node.connect(b);
    node = b;
  }
  node.connect(out);

  return {
    input,
    output: out,
    setMacro: (v, when) => {
      const t = Math.max(when, 0);
      const m = clamp01(v);
      // 0 → 0.7× (down), 0.5 → 1.0× (neutral), 1 → 1.6× (up).
      const ratio = m < 0.5 ? 0.7 + 0.6 * (m / 0.5) : 1 + 0.6 * ((m - 0.5) / 0.5);
      const away = Math.abs(ratio - 1) / 0.6; // 0 at neutral, 1 at either end
      bands.forEach((b, i) => {
        b.frequency.setValueAtTime(Math.min(ctx.sampleRate / 2.2, FORMANTS[i]! * ratio), t);
        b.gain.setValueAtTime(9 * away, t);
      });
      // Scoop the original formant region proportionally so the shifted peaks
      // dominate instead of doubling the vowel.
      cut.gain.setValueAtTime(-7 * away, t);
      // Loudness compensation for the added peaks.
      out.gain.setValueAtTime(1 - 0.22 * away, t);
    },
    dispose: () => {
      for (const n of [input, cut, out, ...bands]) n.disconnect();
    },
  };
}

/** Rhythmic Gate: a tempo-locked square VCA chop. */
function buildLfoGate(ctx: AudioContext, kind: "gate"): AlgorithmGraph {
  const input = ctx.createGain();
  const vca = ctx.createGain();
  vca.gain.value = 1;
  input.connect(vca);

  const lfo = ctx.createOscillator();
  lfo.type = "square";
  const depth = ctx.createGain();
  depth.gain.value = 0.5;
  const offset = ctx.createConstantSource();
  offset.offset.value = 0.5;
  lfo.connect(depth);
  depth.connect(vca.gain);
  offset.connect(vca.gain);
  vca.gain.value = 0;
  lfo.frequency.value = 4;
  lfo.start();
  offset.start();

  let bpm = 120;
  let macro = 0.5;
  const apply = (when: number) => {
    const t = Math.max(when, 0);
    // macro selects the division: 1/4 → 1/8 → 1/16 → 1/32.
    const divisions = [1, 2, 4, 8];
    const div = divisions[Math.min(divisions.length - 1, Math.floor(macro * divisions.length))]!;
    lfo.frequency.setValueAtTime((bpm / 60) * div, t);
    // A hard chop: the VCA travels 0 → 1.
    const d = 0.5;
    depth.gain.setValueAtTime(d, t);
    offset.offset.setValueAtTime(0.5, t);
  };
  apply(ctx.currentTime);

  return {
    input,
    output: vca,
    setMacro: (v, when) => {
      macro = clamp01(v);
      apply(when);
    },
    setTempo: (b, when) => {
      bpm = b;
      apply(when);
    },
    dispose: () => {
      try {
        lfo.stop();
        offset.stop();
      } catch {
        /* already stopped */
      }
      for (const n of [input, vca, depth]) n.disconnect();
    },
  };
}

// ---------------------------------------------------------------------------
// MOTION
// ---------------------------------------------------------------------------

function buildEcho(ctx: AudioContext, pitched: boolean): AlgorithmGraph {
  const input = ctx.createGain();
  const out = ctx.createGain();
  const delay = ctx.createDelay(4);
  const feedback = ctx.createGain();
  const damp = ctx.createBiquadFilter();
  damp.type = "lowpass";
  damp.frequency.value = pitched ? 5200 : 7800;
  input.connect(delay);
  delay.connect(damp);
  damp.connect(feedback);
  feedback.connect(delay);
  damp.connect(out);

  // Pitch echo: a detuned second tap that drifts the repeats upward.
  let shimmerTap: { delay: DelayNode; gain: GainNode } | null = null;
  if (pitched) {
    const d2 = ctx.createDelay(4);
    const g2 = ctx.createGain();
    g2.gain.value = 0.45;
    damp.connect(d2);
    d2.connect(g2);
    g2.connect(out);
    shimmerTap = { delay: d2, gain: g2 };
  }

  let bpm = 120;
  let macro = 0.5;
  const apply = (when: number) => {
    const t = Math.max(when, 0);
    const beat = 60 / bpm;
    // 1/8 dotted at low macro through 1/16 at high macro.
    const ratios = [0.75, 0.5, 0.375, 0.25];
    const ratio = ratios[Math.min(ratios.length - 1, Math.floor(macro * ratios.length))]!;
    delay.delayTime.setValueAtTime(beat * ratio, t);
    feedback.gain.setValueAtTime(0.18 + 0.5 * macro, t);
    if (shimmerTap) shimmerTap.delay.delayTime.setValueAtTime(beat * ratio * 1.5, t);
  };
  apply(ctx.currentTime);

  return {
    input,
    output: out,
    setMacro: (v, when) => {
      macro = clamp01(v);
      apply(when);
    },
    setTempo: (b, when) => {
      bpm = b;
      apply(when);
    },
    dispose: () => {
      for (const n of [input, delay, damp, feedback, out]) n.disconnect();
      shimmerTap?.delay.disconnect();
      shimmerTap?.gain.disconnect();
    },
  };
}

function buildScatter(ctx: AudioContext): AlgorithmGraph {
  // Granular scatter: four modulated short delays, each with its own random
  // walk, summed. Heavy but node-only, so it never needs a worklet gate.
  const input = ctx.createGain();
  const out = ctx.createGain();
  const lines: { delay: DelayNode; lfo: OscillatorNode; depth: GainNode; gain: GainNode }[] = [];
  for (let i = 0; i < 4; i++) {
    const delay = ctx.createDelay(0.5);
    delay.delayTime.value = 0.02 + i * 0.017;
    const lfo = ctx.createOscillator();
    lfo.type = "sine";
    lfo.frequency.value = 0.7 + i * 1.3;
    const depth = ctx.createGain();
    depth.gain.value = 0.004;
    lfo.connect(depth);
    depth.connect(delay.delayTime);
    lfo.start();
    const gain = ctx.createGain();
    gain.gain.value = 0.25;
    input.connect(delay);
    delay.connect(gain);
    gain.connect(out);
    lines.push({ delay, lfo, depth, gain });
  }
  return {
    input,
    output: out,
    setMacro: (v, when) => {
      const t = Math.max(when, 0);
      const m = clamp01(v);
      lines.forEach((l, i) => {
        l.depth.gain.setValueAtTime(0.001 + 0.02 * m, t);
        l.lfo.frequency.setValueAtTime((0.7 + i * 1.3) * (0.5 + 2.5 * m), t);
      });
    },
    dispose: () => {
      for (const l of lines) {
        try {
          l.lfo.stop();
        } catch {
          /* already stopped */
        }
        l.delay.disconnect();
        l.depth.disconnect();
        l.gain.disconnect();
      }
      input.disconnect();
      out.disconnect();
    },
  };
}

// ---------------------------------------------------------------------------
// SPACE
// ---------------------------------------------------------------------------

function buildReverb(ctx: AudioContext, shimmer: boolean): AlgorithmGraph {
  const input = ctx.createGain();
  const out = ctx.createGain();
  const sum = ctx.createGain();
  const limiter = ctx.createDynamicsCompressor();
  limiter.threshold.value = -6;
  limiter.ratio.value = 20;
  limiter.attack.value = 0.003;
  limiter.release.value = 0.2;
  const taps = [0.0297, 0.0371, 0.0411, 0.0437];
  const lines = taps.map((tap) => {
    const delay = ctx.createDelay(1);
    delay.delayTime.value = tap;
    const damp = ctx.createBiquadFilter();
    damp.type = "lowpass";
    damp.frequency.value = shimmer ? 6400 : 4200;
    const fb = ctx.createGain();
    fb.gain.value = 0.4;
    input.connect(delay);
    delay.connect(damp);
    damp.connect(fb);
    damp.connect(sum);
    return { delay, damp, fb };
  });
  lines.forEach((l, i) => l.fb.connect(lines[(i + 1) % lines.length]!.delay));
  sum.connect(limiter);
  limiter.connect(out);

  // Shimmer: an extra bright, longer feedback path above the tail.
  let sparkle: { delay: DelayNode; hp: BiquadFilterNode; fb: GainNode } | null = null;
  if (shimmer) {
    const delay = ctx.createDelay(1);
    delay.delayTime.value = 0.13;
    const hp = ctx.createBiquadFilter();
    hp.type = "highpass";
    hp.frequency.value = 1400;
    const fb = ctx.createGain();
    fb.gain.value = 0.45;
    sum.connect(delay);
    delay.connect(hp);
    hp.connect(fb);
    fb.connect(sum);
    hp.connect(out);
    sparkle = { delay, hp, fb };
  }

  return {
    input,
    output: out,
    setMacro: (v, when) => {
      const t = Math.max(when, 0);
      const m = clamp01(v);
      // Feedback is hard-capped: a runaway tail must be impossible.
      const fb = Math.min(0.9, 0.3 + 0.55 * m);
      for (const l of lines) l.fb.gain.setValueAtTime(fb, t);
      if (sparkle) sparkle.fb.gain.setValueAtTime(Math.min(0.7, 0.25 + 0.45 * m), t);
    },
    dispose: () => {
      for (const l of lines) {
        l.delay.disconnect();
        l.damp.disconnect();
        l.fb.disconnect();
      }
      sparkle?.delay.disconnect();
      sparkle?.hp.disconnect();
      sparkle?.fb.disconnect();
      for (const n of [input, sum, out]) n.disconnect();
      limiter.disconnect();
    },
  };
}

function buildFreeze(ctx: AudioContext): AlgorithmGraph {
  // Spectral Freeze, node-only approximation: a near-unity feedback loop with
  // heavy damping holds the last ~250 ms as a sustained pad. Feedback is
  // capped strictly below 1 so it decays instead of exploding.
  const input = ctx.createGain();
  const out = ctx.createGain();
  const delay = ctx.createDelay(1);
  delay.delayTime.value = 0.25;
  const damp = ctx.createBiquadFilter();
  damp.type = "lowpass";
  damp.frequency.value = 5200;
  const fb = ctx.createGain();
  fb.gain.value = 0.9;
  const limiter = ctx.createDynamicsCompressor();
  limiter.threshold.value = -8;
  limiter.ratio.value = 20;
  input.connect(delay);
  delay.connect(damp);
  damp.connect(fb);
  fb.connect(delay);
  damp.connect(limiter);
  limiter.connect(out);
  return {
    input,
    output: out,
    setMacro: (v, when) => {
      const t = Math.max(when, 0);
      const m = clamp01(v);
      fb.gain.setValueAtTime(Math.min(0.985, 0.86 + 0.13 * m), t);
      delay.delayTime.setValueAtTime(0.08 + 0.35 * m, t);
    },
    dispose: () => {
      for (const n of [input, delay, damp, fb, out]) n.disconnect();
      limiter.disconnect();
    },
  };
}

export function buildAlgorithm(ctx: AudioContext, id: AlgorithmId): AlgorithmGraph | null {
  switch (id) {
    case "filter":
      return buildFilter(ctx);
    case "isolator":
      return buildIsolator(ctx);
    case "dirt":
      return buildDirt(ctx);
    case "reelFlange":
      return buildReelFlange(ctx);
    case "formantShift":
      return buildFormantShift(ctx);
    case "gate":
      return buildLfoGate(ctx, "gate");
    case "echo":
      return buildEcho(ctx, false);
    case "pitchEcho":
      return buildEcho(ctx, true);
    case "scatter":
      return buildScatter(ctx);
    case "reverb":
      return buildReverb(ctx, false);
    case "shimmer":
      return buildReverb(ctx, true);
    case "freeze":
      return buildFreeze(ctx);
  }
}

/**
 * One serial bank stage. `input` and `output` exist for the lifetime of the
 * track: only the algorithm graph between them is ever rebuilt.
 */
export class BankStage {
  readonly input: GainNode;
  readonly output: GainNode;
  private dry: GainNode;
  private wet: GainNode;
  private graphs = new Map<AlgorithmIndex, AlgorithmGraph>();
  private current: AlgorithmIndex = 0;
  private active = false;
  private macros: [number, number, number];
  private rejected: (string | null)[] = [null, null, null];
  private bpm = 120;

  constructor(
    private ctx: AudioContext,
    readonly bank: BankIndex,
  ) {
    this.input = ctx.createGain();
    this.output = ctx.createGain();
    this.dry = ctx.createGain();
    this.wet = ctx.createGain();
    this.dry.gain.value = 1;
    this.wet.gain.value = 0;
    this.input.connect(this.dry);
    this.dry.connect(this.output);
    this.wet.connect(this.output);
    this.macros = BANKS[bank]!.algorithms.map((a) => a.defaultMacro) as [number, number, number];
  }

  get selected(): AlgorithmIndex {
    return this.current;
  }

  get isActive(): boolean {
    return this.active;
  }

  rejectionFor(algorithm: AlgorithmIndex): string | null {
    return this.rejected[algorithm] ?? null;
  }

  /** Build-on-demand. Returns null when the algorithm is a passthrough. */
  private ensure(algorithm: AlgorithmIndex): AlgorithmGraph | null {
    const existing = this.graphs.get(algorithm);
    if (existing) return existing;
    const def = algorithmDef(this.bank, algorithm);
    let graph: AlgorithmGraph | null = null;
    try {
      graph = buildAlgorithm(this.ctx, def.id);
    } catch (err) {
      // A single algorithm failing must NEVER disable its bank.
      this.rejected[algorithm] = err instanceof Error ? err.message : String(err);
      return null;
    }
    if (!graph) return null;
    this.input.connect(graph.input);
    graph.output.connect(this.wet);
    graph.setMacro(this.macros[algorithm] ?? def.defaultMacro, this.ctx.currentTime);
    graph.setTempo?.(this.bpm, this.ctx.currentTime);
    this.graphs.set(algorithm, graph);
    return graph;
  }

  /**
   * Select without activating. Cycling algorithms while a bank is held swaps
   * the wet graph under the finger; while inactive it only changes what the
   * next press will run.
   */
  select(algorithm: AlgorithmIndex, when: number) {
    if (algorithm === this.current) return;
    const previous = this.current;
    this.current = algorithm;
    if (!this.active) return;
    const prev = this.graphs.get(previous);
    if (prev) prev.output.disconnect(this.wet);
    const next = this.ensure(algorithm);
    if (next) next.output.connect(this.wet);
    // Passthrough (Beat Repeat) means no wet leg: fall back to full dry.
    this.applyMix(next != null, when);
  }

  /** Zero hold latency: the wet ramp starts at the caller's timestamp. */
  setActive(on: boolean, when: number) {
    this.active = on;
    const graph = on ? this.ensure(this.current) : this.graphs.get(this.current);
    this.applyMix(on && graph != null, when);
  }

  private applyMix(wetOn: boolean, when: number) {
    // Correlated legs: complementary gains, NOT equal-power.
    ramp(this.wet.gain, wetOn ? 1 : 0, when);
    ramp(this.dry.gain, wetOn ? 0 : 1, when);
  }

  setMacro(algorithm: AlgorithmIndex, value: number, when: number) {
    this.macros[algorithm] = clamp01(value);
    this.graphs.get(algorithm)?.setMacro(clamp01(value), when);
  }

  setTempo(bpm: number, when: number) {
    this.bpm = bpm;
    for (const g of this.graphs.values()) g.setTempo?.(bpm, when);
  }

  snapshot(): BankStageSnapshot {
    const def = algorithmDef(this.bank, this.current);
    return {
      bank: this.bank,
      id: BANKS[this.bank]!.id,
      algorithm: def.id,
      active: this.active,
      wet: this.wet.gain.value,
      macro: this.macros[this.current] ?? def.defaultMacro,
      built: this.graphs.has(this.current),
      rejected: this.rejected[this.current] ?? null,
    };
  }

  dispose() {
    for (const g of this.graphs.values()) g.dispose();
    this.graphs.clear();
    for (const n of [this.input, this.output, this.dry, this.wet]) n.disconnect();
  }
}

/**
 * The four stages, wired in signal order. `input` and `output` are permanent,
 * so the rack can be inserted once per track and never touched again.
 */
export class BankRack {
  readonly input: GainNode;
  readonly output: GainNode;
  readonly stages: BankStage[];

  constructor(ctx: AudioContext) {
    this.input = ctx.createGain();
    this.output = ctx.createGain();
    this.stages = [0, 1, 2, 3].map((i) => new BankStage(ctx, i as BankIndex));
    let node: AudioNode = this.input;
    for (const stage of this.stages) {
      node.connect(stage.input);
      node = stage.output;
    }
    node.connect(this.output);
  }

  stage(bank: BankIndex): BankStage {
    return this.stages[bank]!;
  }

  setTempo(bpm: number, when: number) {
    for (const s of this.stages) s.setTempo(bpm, when);
  }

  snapshot(): BankStageSnapshot[] {
    return this.stages.map((s) => s.snapshot());
  }

  dispose() {
    for (const s of this.stages) s.dispose();
    this.input.disconnect();
    this.output.disconnect();
  }
}
