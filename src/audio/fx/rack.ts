/**
 * Phase 5C — per-stem FX rack.
 *
 * Signal graph (correction 2 and 3): the rack input is PERMANENT. Engine
 * migration (node ↔ worklet) crossfades the two tape sources into this stable
 * input, so nothing is ever re-parented and tails / filter state
 * survive the migration.
 *
 *   tape source(s) ─ handoff envelope ─→ FxRackInput
 *
 *   FxRackInput → filter (dry/wet, true bypass) → passthrough ─┬→ directGate ─────────┐
 *                                                             ├→ echoInput  → echoRet ┤
 *                                                             └→ reverbInput→ revRet ─┤
 *                                                                                     ↓
 *                                                       faderGain (post-FX, owns tails)
 *                                                                     ↓
 *                                                       soloGain (separate, smoothed)
 *                                                                     ↓
 *                                                                  master
 *
 * Mute closes directGate / echoInput / reverbInput ONLY: the echo and reverb
 * feedback returns keep decaying, and the fader still rides those tails.
 * Releasing an FX closes only that FX's input, never the track gate.
 */

import { equalPower, measuredDryWet, sampleCurve, FILTER_FADE_S } from "../crossfade";
import type { FxFamily } from "@/machine/stemPerformance";


const GATE_TAU = 0.008;
/** Delay-tap crossfade for echo division / rate changes — no Doppler glide. */
export const ECHO_TAP_FADE_S = 0.02;

export interface FilterVariation {
  name: string;
  type: BiquadFilterType;
  cutoff: number;
  q: number;
}

export const FILTER_VARIATIONS: FilterVariation[] = [
  { name: "Warm LP", type: "lowpass", cutoff: 900, q: 0.7 },
  { name: "Resonant LP", type: "lowpass", cutoff: 700, q: 8 },
  { name: "Clean HP", type: "highpass", cutoff: 500, q: 0.7 },
  { name: "Resonant HP", type: "highpass", cutoff: 700, q: 8 },
];

export interface EchoVariation {
  name: string;
  /** Beats per repeat, relative to a quarter note. */
  ratio: number;
  feedback: number;
  wet: number;
}

/** Named, clamped feedback constants — never an unbounded runaway. */
export const ECHO_FEEDBACK_MIN = 0;
export const ECHO_FEEDBACK_MAX = 0.72;

export const ECHO_VARIATIONS: EchoVariation[] = [
  { name: "1/4", ratio: 1, feedback: 0.42, wet: 0.5 },
  { name: "1/8", ratio: 0.5, feedback: 0.5, wet: 0.5 },
  { name: "dotted 1/8", ratio: 0.75, feedback: 0.55, wet: 0.55 },
  { name: "1/8 triplet", ratio: 1 / 3, feedback: 0.6, wet: 0.5 },
];

export interface ReverbVariation {
  name: string;
  /** Base delay line lengths, seconds. */
  taps: number[];
  feedback: number;
  damping: number;
  wet: number;
}

/** Locally generated algorithmic FDN — no impulse response is ever fetched. */
export const REVERB_VARIATIONS: ReverbVariation[] = [
  { name: "Tight Room", taps: [0.0117, 0.0193, 0.0257, 0.0313], feedback: 0.55, damping: 6000, wet: 0.35 },
  { name: "Plate", taps: [0.0197, 0.0293, 0.0411, 0.0537], feedback: 0.72, damping: 5000, wet: 0.45 },
  { name: "Hall", taps: [0.0313, 0.0457, 0.0631, 0.0797], feedback: 0.82, damping: 3800, wet: 0.5 },
  { name: "Atmospheric Wash", taps: [0.0431, 0.0673, 0.0891, 0.1097], feedback: 0.88, damping: 2600, wet: 0.6 },
];

/** Hard ceiling so an Atmospheric Wash cannot become an oscillator. */
export const REVERB_FEEDBACK_CEILING = 0.9;

/** Slowest effective BPM the ring buffer is sized for (1/2 note at this BPM). */
export const MIN_SUPPORTED_BPM = 40;

export interface FxRackSnapshot {
  filter: { mode: string; cutoff: number; q: number; layer: "tape" | "fx-momentary" | "fx-latched" };
  echo: { active: boolean; variation: string; delayS: number; feedback: number };
  reverb: { active: boolean; variation: string; feedback: number };
  inputOpen: boolean;
  workletLoaded: boolean;
}

export class FxRack {
  /** Permanent — never disconnected, never re-parented. */
  readonly input: GainNode;
  readonly output: GainNode;

  private filterNode: BiquadFilterNode;
  private filterDry: GainNode;
  private filterWet: GainNode;
  private postFilter: GainNode;


  private directGate: GainNode;
  private echoInput: GainNode;
  private reverbInput: GainNode;

  /** Permanent unity passthrough where Beat Repeat used to sit. */
  private repeatBypass: GainNode;

  private echoSum: GainNode;
  private echoTaps: { delay: DelayNode; gain: GainNode }[] = [];
  private echoActiveTap = 0;
  private echoFeedback: GainNode;
  private echoReturn: GainNode;

  private reverbLines: { delay: DelayNode; damp: BiquadFilterNode; fb: GainNode }[] = [];
  private reverbSum: GainNode;
  private reverbLimiter: DynamicsCompressorNode;
  private reverbReturn: GainNode;

  /** Three-layer filter model: the rack is the SINGLE automation owner. */
  private tapeFilter: { mode: "off" | "lp" | "hp"; cutoff: number } = { mode: "off", cutoff: 20000 };
  private filterLayer: "tape" | "fx-momentary" | "fx-latched" = "tape";
  private filterSnapshot: { mode: "off" | "lp" | "hp"; cutoff: number } | null = null;
  private filterQ = 0.7;

  private echoState = { active: false, variation: 1, delayS: 0.5 };
  private reverbState = { active: false, variation: 1 };
  private open = true;

  constructor(
    private ctx: AudioContext,
    destination: AudioNode,
  ) {
    const g = () => ctx.createGain();
    this.input = g();
    this.output = g();
    this.filterNode = ctx.createBiquadFilter();
    this.filterDry = g();
    this.filterWet = g();
    this.postFilter = g();
    this.repeatBypass = g();
    this.directGate = g();
    this.echoInput = g();
    this.reverbInput = g();
    this.echoSum = g();
    this.echoFeedback = g();
    this.echoReturn = g();
    this.reverbSum = g();
    this.reverbReturn = g();
    this.reverbLimiter = ctx.createDynamicsCompressor();

    // filter stage — true dry bypass at rest
    this.filterDry.gain.value = 1;
    this.filterWet.gain.value = 0;
    this.filterNode.type = "lowpass";
    this.filterNode.frequency.value = 20000;
    this.input.connect(this.filterDry);
    this.input.connect(this.filterNode);
    this.filterNode.connect(this.filterWet);
    this.filterDry.connect(this.postFilter);
    this.filterWet.connect(this.postFilter);

    // permanent unity passthrough (Beat Repeat removed)
    this.postFilter.connect(this.repeatBypass);

    for (const node of [this.directGate, this.echoInput, this.reverbInput]) {
      this.repeatBypass.connect(node);
      node.gain.value = 1;
    }

    // echo: two taps + crossfade (no delayTime glide → no Doppler pitch bend)
    this.echoInput.connect(this.echoSum);
    for (let i = 0; i < 2; i++) {
      const delay = ctx.createDelay(4);
      const gain = g();
      gain.gain.value = i === 0 ? 1 : 0;
      delay.delayTime.value = 0.5;
      this.echoSum.connect(delay);
      delay.connect(gain);
      gain.connect(this.echoReturn);
      this.echoTaps.push({ delay, gain });
    }
    this.echoFeedback.gain.value = 0;
    this.echoReturn.gain.value = 0;
    this.echoReturn.connect(this.echoFeedback);
    this.echoFeedback.connect(this.echoSum);

    // reverb: 4-line FDN with damping, cross feedback and an output limiter
    const preset = REVERB_VARIATIONS[0]!;
    for (let i = 0; i < 4; i++) {
      const delay = ctx.createDelay(1);
      delay.delayTime.value = preset.taps[i]!;
      const damp = ctx.createBiquadFilter();
      damp.type = "lowpass";
      damp.frequency.value = preset.damping;
      const fb = g();
      fb.gain.value = 0;
      this.reverbInput.connect(delay);
      delay.connect(damp);
      damp.connect(fb);
      damp.connect(this.reverbSum);
      this.reverbLines.push({ delay, damp, fb });
    }
    // Householder-style cross feedback: each line feeds its neighbour.
    this.reverbLines.forEach((line, i) => {
      const target = this.reverbLines[(i + 1) % this.reverbLines.length]!;
      line.fb.connect(target.delay);
    });
    this.reverbLimiter.threshold.value = -6;
    this.reverbLimiter.ratio.value = 20;
    this.reverbLimiter.attack.value = 0.003;
    this.reverbLimiter.release.value = 0.2;
    this.reverbSum.connect(this.reverbLimiter);
    this.reverbLimiter.connect(this.reverbReturn);
    this.reverbReturn.gain.value = 0;

    this.directGate.connect(this.output);
    this.echoReturn.connect(this.output);
    this.reverbReturn.connect(this.output);
    this.output.connect(destination);
  }

  private ramp(param: AudioParam, value: number, tau = GATE_TAU) {
    param.cancelScheduledValues(this.ctx.currentTime);
    param.setTargetAtTime(value, this.ctx.currentTime, tau);
  }

  /** Mute: close the three inputs, leave the feedback returns decaying. */
  setInputOpen(open: boolean) {
    this.open = open;
    const v = open ? 1 : 0;
    this.ramp(this.directGate.gain, v);
    this.ramp(this.echoInput.gain, v);
    this.ramp(this.reverbInput.gain, v);
  }

  // -------------------------------------------------------------- filter

  private applyFilter(mode: "off" | "lp" | "hp", cutoff: number, q: number, correlation = 1) {
    const now = this.ctx.currentTime;
    if (mode !== "off") {
      this.filterNode.type = mode === "lp" ? "lowpass" : "highpass";
      this.filterNode.frequency.setTargetAtTime(cutoff, now, 0.02);
      this.filterNode.Q.setTargetAtTime(q, now, 0.02);
    }
    const target = mode === "off" ? 0 : 1;
    const curve = (x: number) => measuredDryWet(target === 1 ? x : 1 - x, correlation);
    this.filterDry.gain.cancelScheduledValues(now);
    this.filterWet.gain.cancelScheduledValues(now);
    this.filterDry.gain.setValueCurveAtTime(sampleCurve(curve, "a"), now, FILTER_FADE_S);
    this.filterWet.gain.setValueCurveAtTime(sampleCurve(curve, "b"), now, FILTER_FADE_S);
    this.filterQ = q;
  }

  /** Layer 1 — the base Tape filter (FN + Fader 4). */
  setTapeFilter(mode: "off" | "lp" | "hp", cutoff: number, correlation = 1) {
    this.tapeFilter = { mode, cutoff };
    if (this.filterLayer === "tape") this.applyFilter(mode, cutoff, 0.7, correlation);
  }

  /** Layer 2/3 — FX preset override. Momentary snapshots the tape filter. */
  setFxFilter(variationIndex: number | null, latched = false) {
    if (variationIndex == null) {
      const snap = this.filterSnapshot ?? this.tapeFilter;
      this.filterLayer = "tape";
      this.filterSnapshot = null;
      // Release restores the UNDERLYING tape filter, never a forced dry.
      this.applyFilter(snap.mode, snap.cutoff, 0.7);
      return;
    }
    if (this.filterLayer === "tape") this.filterSnapshot = { ...this.tapeFilter };
    this.filterLayer = latched ? "fx-latched" : "fx-momentary";
    const v = FILTER_VARIATIONS[variationIndex] ?? FILTER_VARIATIONS[0]!;
    this.applyFilter(v.type === "lowpass" ? "lp" : "hp", v.cutoff, v.q);
  }

  /** Latching commits the override; the snapshot is discarded. */
  commitFilterLatch() {
    if (this.filterLayer === "fx-momentary") this.filterLayer = "fx-latched";
    this.filterSnapshot = null;
  }

  // ---------------------------------------------------------------- echo

  echoDelayFor(variationIndex: number, effectiveBpm: number): number {
    const v = ECHO_VARIATIONS[variationIndex] ?? ECHO_VARIATIONS[0]!;
    return (60 / Math.max(1, effectiveBpm)) * v.ratio;
  }

  setEcho(active: boolean, variationIndex: number, effectiveBpm: number) {
    const v = ECHO_VARIATIONS[variationIndex] ?? ECHO_VARIATIONS[0]!;
    const delayS = this.echoDelayFor(variationIndex, effectiveBpm);
    const now = this.ctx.currentTime;
    if (Math.abs(delayS - this.echoState.delayS) > 1e-6) {
      // Two taps with a short equal-power crossfade — NOT delayTime smoothing.
      const next = 1 - this.echoActiveTap;
      const from = this.echoTaps[this.echoActiveTap]!;
      const to = this.echoTaps[next]!;
      to.delay.delayTime.setValueAtTime(delayS, now);
      from.gain.gain.setValueCurveAtTime(sampleCurve(equalPower, "a"), now, ECHO_TAP_FADE_S);
      to.gain.gain.setValueCurveAtTime(sampleCurve(equalPower, "b"), now, ECHO_TAP_FADE_S);
      this.echoActiveTap = next;
    }
    const fb = Math.min(ECHO_FEEDBACK_MAX, Math.max(ECHO_FEEDBACK_MIN, v.feedback));
    this.ramp(this.echoFeedback.gain, active ? fb : 0, 0.02);
    // Release closes only the echo's own return; the tail decays out.
    this.ramp(this.echoReturn.gain, active ? v.wet : 0, 0.02);
    this.echoState = { active, variation: variationIndex + 1, delayS };
  }

  // -------------------------------------------------------------- reverb

  setReverb(active: boolean, variationIndex: number) {
    const v = REVERB_VARIATIONS[variationIndex] ?? REVERB_VARIATIONS[0]!;
    const fb = Math.min(REVERB_FEEDBACK_CEILING, v.feedback);
    this.reverbLines.forEach((line, i) => {
      line.delay.delayTime.setTargetAtTime(v.taps[i]!, this.ctx.currentTime, 0.05);
      line.damp.frequency.setTargetAtTime(v.damping, this.ctx.currentTime, 0.05);
      this.ramp(line.fb.gain, active ? fb : 0, 0.05);
    });
    this.ramp(this.reverbReturn.gain, active ? v.wet : 0, 0.03);
    this.reverbState = { active, variation: variationIndex + 1 };
  }

  /** Fade every DSP tail out — used on song switch / stem replacement. */
  flushTails() {
    this.ramp(this.echoFeedback.gain, 0, 0.02);
    this.ramp(this.echoReturn.gain, 0, 0.02);
    for (const line of this.reverbLines) this.ramp(line.fb.gain, 0, 0.03);
    this.ramp(this.reverbReturn.gain, 0, 0.03);
    this.echoState = { ...this.echoState, active: false };
    this.reverbState = { ...this.reverbState, active: false };
  }

  snapshot(): FxRackSnapshot {
    return {
      filter: {
        mode: this.filterLayer === "tape" ? this.tapeFilter.mode : "fx",
        cutoff: this.filterNode.frequency.value,
        q: this.filterQ,
        layer: this.filterLayer,
      },
      echo: {
        active: this.echoState.active,
        variation: ECHO_VARIATIONS[this.echoState.variation - 1]?.name ?? "1/4",
        delayS: this.echoState.delayS,
        feedback: this.echoFeedback.gain.value,
      },
      reverb: {
        active: this.reverbState.active,
        variation: REVERB_VARIATIONS[this.reverbState.variation - 1]?.name ?? "Tight Room",
        feedback: this.reverbLines[0]?.fb.gain.value ?? 0,
      },
      inputOpen: this.open,
      workletLoaded: false,

    };
  }

  dispose() {
    try {
      this.output.disconnect();
      this.input.disconnect();
    } catch {
      /* noop */
    }
  }
}

export const FX_FAMILY_LABEL: Record<FxFamily, string> = {
  filter: "Filter",
  echo: "Echo",
  reverb: "Reverb",
};
