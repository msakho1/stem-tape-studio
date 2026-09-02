/**
 * LED Stage 2 — per-stem activity envelopes.
 *
 * Four INDEPENDENT envelopes, one per stem, fed from the four post-fader /
 * post-solo analyser taps that already exist in the audio graph. This module
 * is pure: it never touches the audio graph, never allocates per sample, and
 * never renders. `useSp1LedFrame` samples it on the single existing rAF clock
 * and hands the four levels to the authoritative LED resolver.
 *
 * Deliberately NOT in scope here: mute/solo, loop, reverse, FX, slow, scratch.
 * Those compose as modifiers in later stages; this file only produces the base
 * activity layer.
 */

/** Attack time constant. Effectively instant — a transient must read as a hit. */
export const ATTACK_MS = 6;
/** Decay time constant. ~250 ms: musical, not twitchy, not smeared. */
export const DECAY_MS = 250;
/** Below this RMS a stem is treated as genuinely silent (≈ -52 dBFS). */
export const SILENCE_RMS = 0.0025;
/** RMS window mapped onto 0..1, in dBFS. */
export const FLOOR_DB = -48;
export const CEIL_DB = -8;

/** Log-domain map from analyser RMS to a 0..1 activity level. */
export function levelFromRms(rms: number): number {
  if (!(rms > SILENCE_RMS)) return 0;
  const db = 20 * Math.log10(rms);
  const norm = (db - FLOOR_DB) / (CEIL_DB - FLOOR_DB);
  return norm <= 0 ? 0 : norm >= 1 ? 1 : norm;
}

/**
 * Four fixed envelopes. Allocation-stable: the level array is created once and
 * mutated in place, so a 60 Hz visual meter generates no garbage.
 */
export class StemActivityEnvelopes {
  private readonly levels: number[] = [0, 0, 0, 0];
  private last: number | null = null;

  /** Current levels (live array — read, never retain or mutate). */
  get values(): readonly number[] {
    return this.levels;
  }

  /** Drops every envelope to zero. Used on STOP so no RMS is left frozen. */
  reset(): void {
    this.levels[0] = 0;
    this.levels[1] = 0;
    this.levels[2] = 0;
    this.levels[3] = 0;
    this.last = null;
  }

  /**
   * Advances the four envelopes to `now` from four independent RMS readings.
   * `playing === false` collapses everything to silence immediately.
   */
  sample(rms: readonly number[], now: number, playing: boolean): readonly number[] {
    if (!playing) {
      this.reset();
      return this.levels;
    }
    const dt = this.last == null ? 16 : Math.max(0, Math.min(250, now - this.last));
    this.last = now;
    const attack = Math.exp(-dt / ATTACK_MS);
    const decay = Math.exp(-dt / DECAY_MS);
    for (let i = 0; i < 4; i++) {
      const target = levelFromRms(rms[i] ?? 0);
      const cur = this.levels[i]!;
      const k = target > cur ? attack : decay;
      const next = target + (cur - target) * k;
      this.levels[i] = next < 0.004 ? 0 : next > 1 ? 1 : next;
    }
    return this.levels;
  }
}
