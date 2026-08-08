/**
 * Anti-click matrix (Phase 5).
 *
 * Every command that can produce a discontinuity, and the ONE mechanism that
 * removes it. Binding correction #6: a loop-mode transition is no longer listed
 * as needing nothing — changing loop length moves the read pointer, which is a
 * waveform discontinuity, so it takes the same dual-source crossfade as a seam.
 */

import { FILTER_FADE_S, SEAM_FADE_S } from "./crossfade";

export type AntiClick =
  | "none"
  | "gain-ramp"
  | "dual-source-crossfade"
  | "complementary-dry-wet"
  | "rate-glide";

export interface MatrixRow {
  command: string;
  effect: string;
  antiClick: AntiClick;
  fadeS: number;
  note: string;
}

export const ANTI_CLICK_MATRIX: MatrixRow[] = [
  {
    command: "loop.enable / loop.disable",
    effect: "engage or release the active window",
    antiClick: "dual-source-crossfade",
    fadeS: SEAM_FADE_S,
    note: "entering or leaving loop mode relocates the read pointer",
  },
  {
    command: "loop.length / chop.div",
    effect: "loopLength = windowWidth / chopDiv",
    antiClick: "dual-source-crossfade",
    fadeS: SEAM_FADE_S,
    note: "changing loop length is a pointer jump, not a no-op",
  },
  {
    command: "loop.seam (wrap)",
    effect: "end → start of the active slice",
    antiClick: "dual-source-crossfade",
    fadeS: SEAM_FADE_S,
    note: "equal-power: the two tape points are uncorrelated",
  },
  {
    command: "chop.index",
    effect: "jump to another slice of the window",
    antiClick: "dual-source-crossfade",
    fadeS: SEAM_FADE_S,
    note: "pointer jump",
  },
  {
    command: "tape.reverse",
    effect: "swap to the reversed buffer at the mirrored position",
    antiClick: "dual-source-crossfade",
    fadeS: SEAM_FADE_S,
    note: "direction flip is a discontinuity in slope and content",
  },
  {
    command: "rate.set / rocker",
    effect: "varispeed, linear in effective BPM",
    antiClick: "rate-glide",
    fadeS: 0,
    note: "exponential glide, integrated exactly; pending seams recomputed",
  },
  {
    command: "filter.mode / filter.cutoff",
    effect: "dry ↔ LP/HP",
    antiClick: "complementary-dry-wet",
    fadeS: FILTER_FADE_S,
    note: "correlated paths — complementary gains, never equal-power",
  },
  {
    command: "track.mute / fader",
    effect: "level change",
    antiClick: "gain-ramp",
    fadeS: 0.008,
    note: "setTargetAtTime on the physical track gain",
  },
  {
    command: "worklet.handoff",
    effect: "node graph → TapeProcessor",
    antiClick: "dual-source-crossfade",
    fadeS: SEAM_FADE_S,
    note: "at one shared transport frame, phase-checked before the switch",
  },
];

export function antiClickFor(command: string): MatrixRow | undefined {
  return ANTI_CLICK_MATRIX.find((r) => r.command.split(" / ").some((c) => c === command));
}
