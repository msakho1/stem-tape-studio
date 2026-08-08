/**
 * Frame-anchored tempo grid and punch math (plan §F, 6C).
 *
 * Every decision here is expressed in AudioContext FRAMES, not UI timestamps.
 * Pure functions so punch boundaries are unit-tested to the frame.
 */

export interface GridState {
  /** Learned tempo, null until four taps land. */
  bpm: number | null;
  /** Context frame of a downbeat on the learned grid. */
  anchorFrame: number;
  sampleRate: number;
  source: "none" | "tapped" | "rounded" | "manual";
  rejected: boolean;
  /** Median-filtered inter-tap intervals in frames, newest first. */
  intervals: number[];
}

export const MAX_GRID_BPM = 220;
export const MIN_GRID_BPM = 40;
/** Taps whose interval deviates more than this from the median are rejected. */
export const OUTLIER_TOLERANCE = 0.25;

export function emptyGrid(sampleRate: number): GridState {
  return { bpm: null, anchorFrame: 0, sampleRate, source: "none", rejected: false, intervals: [] };
}

export function median(values: number[]): number {
  if (values.length === 0) return 0;
  const s = [...values].sort((a, b) => a - b);
  const mid = s.length >> 1;
  return s.length % 2 ? s[mid]! : (s[mid - 1]! + s[mid]!) / 2;
}

/**
 * Feed one tap at an exact context frame. Four accepted taps produce a BPM;
 * further taps refine it with the existing 25 % outlier rejection.
 */
export function tapGrid(grid: GridState, contextFrame: number, lastTapFrame: number | null): GridState {
  if (lastTapFrame == null) return { ...grid, anchorFrame: contextFrame, rejected: false };
  const delta = contextFrame - lastTapFrame;
  if (delta <= 0) return grid;
  const secs = delta / grid.sampleRate;
  const bpmCandidate = 60 / secs;
  if (bpmCandidate < MIN_GRID_BPM || bpmCandidate > MAX_GRID_BPM) {
    // Far off — v2.6 rejects the whole grid rather than half-following it.
    return { ...grid, rejected: true, intervals: [] };
  }
  const med = grid.intervals.length ? median(grid.intervals) : delta;
  const accepted = grid.intervals.length < 2 || Math.abs(delta - med) / med <= OUTLIER_TOLERANCE;
  const intervals = accepted ? [delta, ...grid.intervals].slice(0, 8) : grid.intervals;
  const m = median(intervals);
  const bpm = m > 0 ? 60 / (m / grid.sampleRate) : null;
  return {
    ...grid,
    rejected: false,
    intervals,
    bpm: intervals.length >= 3 ? bpm : grid.bpm,
    anchorFrame: contextFrame,
    source: intervals.length >= 3 ? "tapped" : grid.source,
  };
}

export function roundGrid(grid: GridState): GridState {
  if (grid.bpm == null) return grid;
  return { ...grid, bpm: Math.round(grid.bpm), source: "rounded" };
}

export function clearGrid(grid: GridState): GridState {
  return { ...emptyGrid(grid.sampleRate) };
}

export function beatFrames(grid: GridState): number | null {
  if (grid.bpm == null || grid.bpm <= 0) return null;
  return (60 / grid.bpm) * grid.sampleRate;
}

/** Nearest grid boundary at or before `frame`. */
export function previousBoundary(grid: GridState, frame: number): number | null {
  const bf = beatFrames(grid);
  if (bf == null) return null;
  const n = Math.floor((frame - grid.anchorFrame) / bf);
  return grid.anchorFrame + n * bf;
}

export function nextBoundary(grid: GridState, frame: number): number | null {
  const bf = beatFrames(grid);
  if (bf == null) return null;
  const n = Math.ceil((frame - grid.anchorFrame) / bf);
  const b = grid.anchorFrame + n * bf;
  return b <= frame ? b + bf : b;
}

export interface PunchDecision {
  /** Context frame the recording actually starts on. */
  startFrame: number;
  /** Frames of look-back needed to reach that boundary from `pressFrame`. */
  lookBackFrames: number;
  mode: "late-lookback" | "next-boundary" | "immediate" | "pre-roll";
  detail: string;
}

/**
 * Late-press punch: a press within the late window AFTER a boundary starts from
 * that boundary out of the look-back ring. Otherwise, with a grid, the punch
 * ALWAYS quantises to the next boundary — never an unquantised gridded start.
 */
export function decidePunch(
  grid: GridState,
  pressFrame: number,
  opts: { lateWindowMs?: number; preRollFrames?: number; maxLookBackFrames: number },
): PunchDecision {
  const bf = beatFrames(grid);
  const preRoll = opts.preRollFrames ?? 0;
  if (bf == null || grid.bpm == null) {
    return {
      startFrame: pressFrame - Math.min(preRoll, opts.maxLookBackFrames),
      lookBackFrames: Math.min(preRoll, opts.maxLookBackFrames),
      mode: "pre-roll",
      detail: `no grid — start ${Math.min(preRoll, opts.maxLookBackFrames)} frames of pre-roll before the press`,
    };
  }
  const lateWindow = Math.min(((opts.lateWindowMs ?? 120) / 1000) * grid.sampleRate, bf / 8);
  const prev = previousBoundary(grid, pressFrame)!;
  const sincePrev = pressFrame - prev;
  if (sincePrev <= lateWindow && sincePrev <= opts.maxLookBackFrames) {
    return {
      startFrame: prev,
      lookBackFrames: Math.round(sincePrev),
      mode: "late-lookback",
      detail: `late press ${sincePrev.toFixed(0)} frames after the boundary (window ${lateWindow.toFixed(0)}) — recovered from look-back`,
    };
  }
  const next = nextBoundary(grid, pressFrame)!;
  return {
    startFrame: next,
    lookBackFrames: 0,
    mode: "next-boundary",
    detail: `quantised to the next grid boundary in ${(next - pressFrame).toFixed(0)} frames`,
  };
}

/** Punch-out: next loop seam when a loop exists, else the next grid boundary. */
export function decidePunchOut(
  grid: GridState,
  pressFrame: number,
  loopSeamFrame: number | null,
  fadeFrames: number,
): { stopFrame: number; mode: "loop-seam" | "grid-boundary" | "immediate"; detail: string } {
  if (loopSeamFrame != null && loopSeamFrame >= pressFrame)
    return { stopFrame: loopSeamFrame, mode: "loop-seam", detail: `stops at the next loop seam (${loopSeamFrame - pressFrame} frames)` };
  const nb = nextBoundary(grid, pressFrame);
  if (nb != null) return { stopFrame: nb, mode: "grid-boundary", detail: `stops at the next grid boundary (${(nb - pressFrame).toFixed(0)} frames)` };
  return { stopFrame: pressFrame + fadeFrames, mode: "immediate", detail: `free recording — immediate stop through a ${fadeFrames}-frame anti-click fade` };
}
