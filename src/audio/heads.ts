/**
 * Heads mode — four virtual playback heads over ONE source loop (Phase 6, §3).
 *
 * Pure math only: no Web Audio, no DOM. Both engines (worklet kernel and node
 * fallback) and the PRINT renderer derive every position from these functions,
 * so the two paths cannot drift apart by construction.
 *
 * Invariants:
 *  1. A head position is DERIVED from the source's own read position every
 *     block — never accumulated. Ten minutes of playback therefore cannot
 *     accumulate drift; the offset is exact modulo arithmetic.
 *  2. Offsets are quarter fractions of the ACTIVE AUDIBLE CYCLE (the resolved
 *     loop/window/chop length), not of a guessed quarter note.
 *  3. Reverse is a negative read step, not a reversed PCM allocation.
 */

export const HEAD_OFFSETS = [0, 0.25, 0.5, 0.75] as const;
export const HEAD_COUNT = 4;

export interface HeadState {
  /** 0..1 within the active cycle. Defaults to HEAD_OFFSETS[i]; scrub overrides it. */
  offset: number;
  level: number;
  muted: boolean;
  reverse: boolean;
  /** True once the user has scrubbed this head away from its quarter default. */
  scrubbed: boolean;
}

export type PrintPhase = "rendering" | "finalising" | "done" | "failed";

export interface PrintState {
  target: number;
  phase: PrintPhase;
  detail: string;
  cycleFrames: number;
}

export interface HeadsState {
  active: boolean;
  /** Track index the four heads read. */
  source: number | null;
  /** Active audible cycle length in SOURCE frames at the moment of entry/relink. */
  cycleFrames: number;
  /** First frame of the active cycle inside the source PCM. */
  cycleStartFrame: number;
  heads: HeadState[];
  /** Engine that is actually serving the heads — surfaced in diagnostics. */
  engine: "worklet" | "node" | null;
  /** Non-null when the serving path is a documented degradation. */
  fallback: string | null;
  print: PrintState | null;
  enteredAtFrame: number;
}

export function defaultHead(i: number): HeadState {
  return { offset: HEAD_OFFSETS[i] ?? 0, level: 0.8, muted: false, reverse: false, scrubbed: false };
}

export function emptyHeads(): HeadsState {
  return {
    active: false,
    source: null,
    cycleFrames: 0,
    cycleStartFrame: 0,
    heads: [0, 1, 2, 3].map(defaultHead),
    engine: null,
    fallback: null,
    print: null,
    enteredAtFrame: 0,
  };
}

export interface SourceCandidate {
  index: number;
  loaded: boolean;
  playing: boolean;
  muted: boolean;
}

/** Default source: the lowest-numbered playing, unmuted, loaded track (§3.1). */
export function chooseSource(candidates: SourceCandidate[]): number | null {
  const ordered = [...candidates].sort((a, b) => a.index - b.index);
  const best = ordered.find((c) => c.loaded && c.playing && !c.muted);
  return best ? best.index : null;
}

export interface EnterResult {
  state: HeadsState;
  ok: boolean;
  detail: string;
}

export function enterHeads(
  prev: HeadsState,
  opts: { source: number | null; cycleFrames: number; cycleStartFrame: number; engine: "worklet" | "node"; fallback?: string | null; frame: number },
): EnterResult {
  if (opts.source == null)
    return { state: prev, ok: false, detail: "heads rejected — no loaded, playing, unmuted source loop to read" };
  if (!(opts.cycleFrames > 1))
    return { state: prev, ok: false, detail: `heads rejected — source track ${opts.source + 1} has no audible cycle (${opts.cycleFrames} frames)` };
  return {
    state: {
      active: true,
      source: opts.source,
      cycleFrames: opts.cycleFrames,
      cycleStartFrame: opts.cycleStartFrame,
      // Positions and directions reset on every new heads session (§3.1).
      heads: [0, 1, 2, 3].map(defaultHead),
      engine: opts.engine,
      fallback: opts.fallback ?? null,
      print: null,
      enteredAtFrame: opts.frame,
    },
    ok: true,
    detail: `heads on — source track ${opts.source + 1}, cycle ${opts.cycleFrames} frames, heads at 0 · 25 · 50 · 75 % (${opts.engine})`,
  };
}

export function exitHeads(prev: HeadsState): HeadsState {
  return { ...emptyHeads(), enteredAtFrame: prev.enteredAtFrame };
}

export function relinkSource(s: HeadsState, source: number, cycleFrames: number, cycleStartFrame: number): HeadsState {
  // Source switch keeps head levels/mutes/directions; only geometry changes.
  return { ...s, source, cycleFrames, cycleStartFrame };
}

export function setHeadLevel(s: HeadsState, i: number, level: number): HeadsState {
  const heads = s.heads.map((h, k) => (k === i ? { ...h, level: Math.max(0, Math.min(1, level)) } : h));
  return { ...s, heads };
}

export function toggleHeadMute(s: HeadsState, i: number): HeadsState {
  return { ...s, heads: s.heads.map((h, k) => (k === i ? { ...h, muted: !h.muted } : h)) };
}

export function toggleHeadReverse(s: HeadsState, i: number): HeadsState {
  return { ...s, heads: s.heads.map((h, k) => (k === i ? { ...h, reverse: !h.reverse } : h)) };
}

/** FUNCTION + fader: ABSOLUTE scrub — the head jumps to the fader position. */
export function scrubHead(s: HeadsState, i: number, position: number): HeadsState {
  const p = ((position % 1) + 1) % 1;
  return { ...s, heads: s.heads.map((h, k) => (k === i ? { ...h, offset: p, scrubbed: true } : h)) };
}

function mod(x: number, n: number): number {
  return ((x % n) + n) % n;
}

/**
 * Read position (SOURCE frames) of one head, derived from the source's own
 * read position. `sourcePosition` is an absolute source-frame read pointer.
 */
export function headReadPosition(head: HeadState, sourcePosition: number, cycleStartFrame: number, cycleFrames: number): number {
  const phase = mod(sourcePosition - cycleStartFrame, cycleFrames);
  const off = head.offset * cycleFrames;
  const p = head.reverse ? mod(off - phase, cycleFrames) : mod(off + phase, cycleFrames);
  return cycleStartFrame + p;
}

/** Offset of head i from head 0, in frames — the acceptance measurement (§8). */
export function headOffsetFrames(s: HeadsState, i: number): number {
  return s.heads[i]!.offset * s.cycleFrames;
}

export function headsSummary(s: HeadsState): string {
  if (!s.active) return "heads off";
  return s.heads
    .map((h, i) => `H${i + 1} ${(h.offset * 100).toFixed(1)}%${h.reverse ? " REV" : ""}${h.muted ? " MUTE" : ""} ${h.level.toFixed(2)}`)
    .join(" · ");
}

/**
 * PRINT render (§4.1): bake exactly ONE audible cycle of the four-head
 * performance in TAPE/SOURCE coordinates. Positions, directions, levels, mutes
 * and cycle geometry are baked; global rate, FX, master and solo are NOT — the
 * printed loop is played back through the live rate, so it is never resampled
 * twice.
 *
 * Integer stepping means the result is exact to one frame and contains no
 * missing or duplicated frames.
 */
export function renderHeadsCycle(source: Float32Array[], cycleStartFrame: number, cycleFrames: number, heads: HeadState[]): Float32Array[] {
  const n = Math.max(1, Math.round(cycleFrames));
  const out = source.map(() => new Float32Array(n));
  const active = heads.filter((h) => !h.muted && h.level > 0);
  if (active.length === 0) return out;
  for (let c = 0; c < source.length; c++) {
    const src = source[c]!;
    const dst = out[c]!;
    for (const h of active) {
      const off = Math.round(h.offset * n);
      for (let i = 0; i < n; i++) {
        const p = h.reverse ? mod(off - i, n) : mod(off + i, n);
        const idx = cycleStartFrame + p;
        const v = idx >= 0 && idx < src.length ? src[idx]! : 0;
        dst[i]! += v * h.level;
      }
    }
    // Hard clamp: a four-head sum can exceed full scale; clipping here is
    // honest and bounded rather than wrapping in the encoder later.
    for (let i = 0; i < n; i++) {
      const v = dst[i]!;
      dst[i] = v > 1 ? 1 : v < -1 ? -1 : v;
    }
  }
  return out;
}

/** Peak of a rendered buffer — used by the acceptance assertions. */
export function peakOf(channels: Float32Array[]): number {
  let p = 0;
  for (const ch of channels) for (let i = 0; i < ch.length; i++) p = Math.max(p, Math.abs(ch[i]!));
  return p;
}

// ---------------------------------------------------------------------------
// Performance-safe Heads (Vocal-only model)
// ---------------------------------------------------------------------------

/** Supporting Heads are capped at this LINEAR gain relative to the focused Head. */
export const SUPPORT_HEAD_CAP = 0.35;

/**
 * Exact quarter-cycle placement of the four Heads, in SOURCE FRAMES.
 *
 * `p` is the shared transport frame at entry, `s` the cycle start frame and
 * `l` the cycle length in frames. Head 1 lands exactly on the frame the normal
 * Vocal lane is producing; Heads 2-4 are +25 / +50 / +75 % of the cycle,
 * wrapped inside it. Nothing is rounded to seconds, beats or React state.
 */
export function quarterCycleHeadFrames(p: number, s: number, l: number): number[] {
  const len = Math.max(1, l);
  return [0, 0.25, 0.5, 0.75].map((f) => s + mod(p - s + f * len, len));
}

export interface HeadMixInput {
  /** Requested fader gain of each Head, 0..1. */
  level: number;
  muted: boolean;
}

/**
 * Bounded multi-Head gain hierarchy.
 *
 * One focused Head keeps its full requested gain. Every other AUDIBLE Head is
 * capped at `SUPPORT_HEAD_CAP` relative to the focused Head's gain. The summed
 * energy `sqrt(Σ g²)` is then normalised to at most 1 so four Heads can never
 * become four equal-volume lead vocals, and the master is never touched.
 */
export function headMixGains(heads: readonly HeadMixInput[], focus: number): number[] {
  const focused = Math.max(0, Math.min(heads.length - 1, focus));
  const focusGain = heads[focused] && !heads[focused]!.muted ? Math.max(0, Math.min(1, heads[focused]!.level)) : 0;
  const raw = heads.map((h, i) => {
    if (h.muted) return 0;
    const want = Math.max(0, Math.min(1, h.level));
    if (i === focused) return want;
    return Math.min(want, SUPPORT_HEAD_CAP * (focusGain > 0 ? focusGain : want));
  });
  const energy = Math.sqrt(raw.reduce((a, g) => a + g * g, 0));
  if (energy > 1) return raw.map((g) => g / energy);
  return raw;
}
