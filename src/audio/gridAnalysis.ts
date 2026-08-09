/**
 * Automatic song-grid analysis (requirement 8).
 *
 * Local, deterministic, offline. Operates on ALREADY-DECODED PCM: it reads
 * `AudioBuffer.getChannelData()` views sequentially and accumulates a small
 * onset envelope (one float per 10 ms hop). No second decode, no duplicate
 * PCM copy — peak extra memory is O(duration / hopSeconds) floats
 * (~4 kB per minute per stem, released as soon as the grid is produced).
 *
 * Pipeline, all pure and unit-testable:
 *   PCM  → onsetEnvelope()          half-wave-rectified log-energy flux
 *   env  → estimateTempo()          autocorrelation + octave arbitration
 *   env  → estimateBeatPhase()      comb search over one beat period
 *   env  → estimateDownbeat()       bar-position search over beatsPerBar
 *
 * The four stems produce ONE shared song grid: their envelopes are summed on a
 * common time axis before tempo estimation, so a sparse vocal cannot pull the
 * grid away from the drums.
 *
 * Persistence is time-first (seconds), with the analysis frames/sample-rate
 * kept as a cross-check: stems can have unequal lengths and encoder padding,
 * so restoring MUST go through round(timeSeconds * decodedContextSampleRate).
 */

export const HOP_SECONDS = 0.01;
export const WINDOW_SECONDS = 0.04;
export const MIN_BPM = 70;
export const MAX_BPM = 180;
/** Octave arbitration pulls the reported tempo into this musical band. */
export const PREFERRED_BPM = 120;

export interface TempoSegment {
  startS: number;
  bpm: number;
}

export interface SongGrid {
  bpm: number;
  beatsPerBar: number;
  /** Song-time seconds of the first analysed beat. */
  firstBeatS: number;
  /** Song-time seconds of the first analysed downbeat (bar 1, beat 1). */
  firstDownbeatS: number;
  beatSeconds: number;
  barSeconds: number;
  /** Sample rate of the decoded PCM the analysis ran on. */
  analysisSampleRate: number;
  /** Frame count of the longest analysed stem, at analysisSampleRate. */
  analysisFrames: number;
  /** Song-timeline duration in seconds (longest stem). */
  durationS: number;
  segments: TempoSegment[];
  /** Cross-check only: firstDownbeatS / durationS. */
  normalizedDownbeat: number;
  source: "analyzed" | "tapped" | "manual";
  /** Content hashes of the stems that produced this grid. */
  sourceHashes: string[];
}

export interface AnalysisInput {
  /** One decoded channel view. Never copied. */
  channel: Float32Array;
  sampleRate: number;
  hash?: string;
}

/**
 * Half-wave-rectified log-energy flux, one value per `HOP_SECONDS`.
 * Sequential single pass; the returned array is the only allocation.
 */
export function onsetEnvelope(
  channel: Float32Array,
  sampleRate: number,
  hopSeconds = HOP_SECONDS,
  windowSeconds = WINDOW_SECONDS,
): Float32Array {
  const hop = Math.max(1, Math.round(hopSeconds * sampleRate));
  const win = Math.max(hop, Math.round(windowSeconds * sampleRate));
  const count = Math.max(0, Math.floor((channel.length - win) / hop) + 1);
  const env = new Float32Array(Math.max(0, count));
  let prev = -Infinity;
  for (let i = 0; i < count; i++) {
    const start = i * hop;
    let sum = 0;
    for (let j = start; j < start + win; j++) {
      const v = channel[j]!;
      sum += v * v;
    }
    const logE = Math.log(1e-9 + sum / win);
    env[i] = prev === -Infinity ? 0 : Math.max(0, logE - prev);
    prev = logE;
  }
  return env;
}

/** Subtract a moving mean so a loud section cannot dominate the correlation. */
export function normalizeEnvelope(env: Float32Array, radius = 20): Float32Array {
  const out = new Float32Array(env.length);
  let peak = 0;
  for (let i = 0; i < env.length; i++) {
    const lo = Math.max(0, i - radius);
    const hi = Math.min(env.length - 1, i + radius);
    let sum = 0;
    for (let j = lo; j <= hi; j++) sum += env[j]!;
    const v = env[i]! - sum / (hi - lo + 1);
    out[i] = v > 0 ? v : 0;
    if (out[i]! > peak) peak = out[i]!;
  }
  if (peak > 0) for (let i = 0; i < out.length; i++) out[i] = out[i]! / peak;
  return out;
}

function autocorrelationAt(env: Float32Array, lag: number): number {
  if (lag <= 0 || lag >= env.length) return 0;
  let sum = 0;
  for (let i = lag; i < env.length; i++) sum += env[i]! * env[i - lag]!;
  return sum / (env.length - lag);
}

export interface TempoEstimate {
  bpm: number;
  lagHops: number;
  strength: number;
}

/**
 * Autocorrelation tempo estimate with explicit octave arbitration: the raw
 * peak, its half and its double are scored, and the candidate closest to the
 * musical centre wins ties within 15 %.
 */
export function estimateTempo(
  env: Float32Array,
  hopSeconds = HOP_SECONDS,
  minBpm = MIN_BPM,
  maxBpm = MAX_BPM,
): TempoEstimate | null {
  if (env.length < 32) return null;
  const minLag = Math.max(2, Math.round(60 / maxBpm / hopSeconds));
  const maxLag = Math.min(env.length - 2, Math.round(60 / minBpm / hopSeconds));
  if (maxLag <= minLag) return null;

  let bestLag = minLag;
  let bestScore = -Infinity;
  for (let lag = minLag; lag <= maxLag; lag++) {
    // Reinforce with the first harmonic so half-tempo peaks do not win outright.
    const score = autocorrelationAt(env, lag) + 0.5 * autocorrelationAt(env, lag * 2);
    if (score > bestScore) {
      bestScore = score;
      bestLag = lag;
    }
  }

  const candidates = [bestLag / 2, bestLag, bestLag * 2].filter((l) => l >= minLag && l <= maxLag);
  let chosen = bestLag;
  let chosenScore = -Infinity;
  for (const lag of candidates) {
    const l = Math.round(lag);
    const raw = autocorrelationAt(env, l);
    const bpm = 60 / (l * hopSeconds);
    // Musical-centre prior, mild enough that a real 75 or 170 BPM still wins.
    const prior = 1 - Math.min(1, Math.abs(Math.log2(bpm / PREFERRED_BPM))) * 0.18;
    const score = raw * prior;
    if (score > chosenScore) {
      chosenScore = score;
      chosen = l;
    }
  }

  // Parabolic refinement around the chosen integer lag.
  const y0 = autocorrelationAt(env, chosen - 1);
  const y1 = autocorrelationAt(env, chosen);
  const y2 = autocorrelationAt(env, chosen + 1);
  const denom = y0 - 2 * y1 + y2;
  const delta = denom !== 0 ? (0.5 * (y0 - y2)) / denom : 0;
  const refined = chosen + Math.max(-0.5, Math.min(0.5, delta));

  return { bpm: 60 / (refined * hopSeconds), lagHops: refined, strength: y1 };
}

/** Comb search: the phase offset (in seconds) whose beat slots carry most onset energy. */
export function estimateBeatPhase(env: Float32Array, beatHops: number, hopSeconds = HOP_SECONDS): number {
  if (env.length === 0 || beatHops <= 0) return 0;
  const steps = Math.max(1, Math.round(beatHops));
  let bestOffset = 0;
  let bestSum = -Infinity;
  for (let off = 0; off < steps; off++) {
    let sum = 0;
    for (let t = off; t < env.length; t += beatHops) {
      const i = Math.round(t);
      if (i < env.length) sum += env[i]!;
    }
    if (sum > bestSum) {
      bestSum = sum;
      bestOffset = off;
    }
  }
  return bestOffset * hopSeconds;
}

/** Which beat of the bar carries the most energy — that beat is the downbeat. */
export function estimateDownbeat(
  env: Float32Array,
  beatHops: number,
  phaseHops: number,
  beatsPerBar = 4,
): number {
  if (env.length === 0 || beatHops <= 0) return 0;
  const scores = new Array<number>(beatsPerBar).fill(0);
  let beat = 0;
  for (let t = phaseHops; t < env.length; t += beatHops) {
    const i = Math.round(t);
    if (i < env.length) scores[beat % beatsPerBar]! += env[i]!;
    beat++;
  }
  let best = 0;
  for (let b = 1; b < beatsPerBar; b++) if (scores[b]! > scores[best]!) best = b;
  return best;
}

/** Sum per-stem envelopes onto a common time axis (all share `hopSeconds`). */
export function mergeEnvelopes(envelopes: Float32Array[]): Float32Array {
  const len = envelopes.reduce((m, e) => Math.max(m, e.length), 0);
  const out = new Float32Array(len);
  for (const e of envelopes) for (let i = 0; i < e.length; i++) out[i] = out[i]! + e[i]!;
  return out;
}

/**
 * Analyse the loaded stems into ONE shared song grid.
 * Returns null only when there is no usable audio at all.
 */
export function analyzeSongGrid(
  inputs: AnalysisInput[],
  opts: { beatsPerBar?: number; hopSeconds?: number } = {},
): SongGrid | null {
  const usable = inputs.filter((i) => i.channel.length > 0 && i.sampleRate > 0);
  if (usable.length === 0) return null;
  const hopSeconds = opts.hopSeconds ?? HOP_SECONDS;
  const beatsPerBar = opts.beatsPerBar ?? 4;
  const sampleRate = usable[0]!.sampleRate;

  const envelopes = usable.map((i) => normalizeEnvelope(onsetEnvelope(i.channel, i.sampleRate, hopSeconds)));
  const env = mergeEnvelopes(envelopes);
  const tempo = estimateTempo(env, hopSeconds);
  if (!tempo) return null;

  const beatSeconds = 60 / tempo.bpm;
  const beatHops = tempo.lagHops;
  const phaseS = estimateBeatPhase(env, beatHops, hopSeconds);
  const downbeatIndex = estimateDownbeat(env, beatHops, phaseS / hopSeconds, beatsPerBar);
  const firstBeatS = phaseS;
  const firstDownbeatS = phaseS + downbeatIndex * beatSeconds;

  const analysisFrames = usable.reduce((m, i) => Math.max(m, i.channel.length), 0);
  const durationS = usable.reduce((m, i) => Math.max(m, i.channel.length / i.sampleRate), 0);

  return {
    bpm: tempo.bpm,
    beatsPerBar,
    firstBeatS,
    firstDownbeatS,
    beatSeconds,
    barSeconds: beatSeconds * beatsPerBar,
    analysisSampleRate: sampleRate,
    analysisFrames,
    durationS,
    segments: [{ startS: 0, bpm: tempo.bpm }],
    normalizedDownbeat: durationS > 0 ? firstDownbeatS / durationS : 0,
    source: "analyzed",
    sourceHashes: usable.map((i) => i.hash ?? ""),
  };
}

// --------------------------------------------------------------- grid maths

/** Song-time seconds of the bar boundary at or before `positionS`. */
export function barStartAt(grid: SongGrid, positionS: number): number {
  const n = Math.floor((positionS - grid.firstDownbeatS) / grid.barSeconds);
  return grid.firstDownbeatS + n * grid.barSeconds;
}

/** Song-time seconds of the next bar boundary strictly after `positionS`. */
export function nextBarAfter(grid: SongGrid, positionS: number): number {
  const n = Math.floor((positionS - grid.firstDownbeatS) / grid.barSeconds) + 1;
  return grid.firstDownbeatS + n * grid.barSeconds;
}

/** Restore path: seconds → frames on the CURRENT decoded context sample rate. */
export function gridFrames(grid: SongGrid, contextSampleRate: number) {
  return {
    firstBeatFrame: Math.round(grid.firstBeatS * contextSampleRate),
    firstDownbeatFrame: Math.round(grid.firstDownbeatS * contextSampleRate),
    beatFrames: Math.round(grid.beatSeconds * contextSampleRate),
    barFrames: Math.round(grid.barSeconds * contextSampleRate),
  };
}

export function describeGrid(grid: SongGrid | null): string {
  if (!grid) return "no grid — load stems to analyse the song";
  return `${grid.bpm.toFixed(2)} BPM · ${grid.beatsPerBar}/4 · downbeat ${grid.firstDownbeatS.toFixed(3)}s · bar ${grid.barSeconds.toFixed(3)}s (${grid.source})`;
}
