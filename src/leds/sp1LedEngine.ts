/**
 * AUTHORITATIVE SP-1 PHYSICAL LED ENGINE.
 *
 * One deterministic resolver owns every physical LED decision. The virtual
 * SP-1 (DOM) and the M0 host→device MIDI driver consume the SAME resolved
 * frame — neither may derive behaviour separately, and CSS may never own
 * animation phase or lifecycle.
 *
 * Physical map (exactly eight MCU-controlled LEDs):
 *   0 Track 1 · 1 Track 2 · 2 Track 3 · 3 Track 4
 *   4 side LED nearest PLAY · 5 PLAY-side middle
 *   6 FUNCTION-side middle  · 7 side LED nearest FUNCTION
 *
 * The two printed dots by FUNCTION and the red PLAY triangle are artwork.
 * The website's `play-indicator` is a UI-only illustration: it is never a
 * ninth physical LED, is never transmitted, and never substitutes for
 * physical index 4.
 */

import { SP1_CONNECT_GREETING_LAP_MS, SP1_CONNECT_GREETING_MS, type SurfaceState } from "@/machine/surface";
import { algorithmDef, bankOfButton } from "@/machine/fx12";
import { fxStateOf, fxTargetOf } from "@/machine/stemPerformance";
import { GLOBAL_SCRUB_SPEEDS, type ScrubSpeedIndex } from "@/audio/inertia";
import type { PhysicalLedId } from "@/diagnostics/physical";

// ---------------------------------------------------------------- map ------

export interface PhysicalLedSlot {
  index: number;
  id: PhysicalLedId;
  name: string;
}

export const PHYSICAL_LED_MAP: readonly PhysicalLedSlot[] = [
  { index: 0, id: "track-led-1", name: "Track 1" },
  { index: 1, id: "track-led-2", name: "Track 2" },
  { index: 2, id: "track-led-3", name: "Track 3" },
  { index: 3, id: "track-led-4", name: "Track 4" },
  { index: 4, id: "side-led-1", name: "side LED nearest PLAY" },
  { index: 5, id: "side-led-2", name: "PLAY-side middle" },
  { index: 6, id: "side-led-3", name: "FUNCTION-side middle" },
  { index: 7, id: "side-led-4", name: "side LED nearest FUNCTION" },
] as const;

export const PLAY_SIDE_INDEX = 4;

// -------------------------------------------------------------- modes ------

export type Sp1LedMode =
  | "off"
  | "dim"
  | "solid"
  | "blink"
  | "rapid-pulse"
  | "breathe"
  | "chase"
  | "activity"
  | "one-shot-single-flash"
  | "one-shot-double-flash";

export type PhaseAnchor =
  | "none"
  | "app-clock"
  | "gesture-start"
  | "loop-wrap"
  | "one-shot-start"
  | "stem-audio";

export const BRIGHT_FULL = 127;
export const BRIGHT_DIM = 40;
export const BRIGHT_TAIL = 18;

/** Confirmed stock cadences, in ms. */
export const PERIOD = {
  blink: 400,
  latchedBlink: 800,
  rapidPulse: 120,
  breathe: 2400,
  oneShotSingle: 220,
  oneShotDouble: 320,
} as const;

/** Four persistent shuttle speeds and their chase periods (one full row lap). */
export const SCRUB_MULTIPLIERS = GLOBAL_SCRUB_SPEEDS;
export const SCRUB_LED_PERIODS_MS: readonly [number, number, number, number] = [640, 500, 320, 200];

/** Loop-division blink periods, index by division 1/2/4/8. */
export function loopPeriodFor(division: 1 | 2 | 4 | 8): number {
  return 800 / division;
}

// ---------------------------------------------------------- precedence -----

/** One deterministic precedence table. Higher wins. */
export const PRECEDENCE = {
  power: 100,
  error: 96,
  connectGreeting: 95,
  loading: 94,
  rejectionFlash: 90,
  confirmationFlash: 88,
  heads: 84,
  scrub: 80,
  fxActive: 76,
  fxSelection: 72,
  loop: 68,
  muteSoloLink: 52,
  activeStem: 46,
  transport: 40,
  songRow: 24,
  battery: 8,
  base: 0,
} as const;

export type PrecedenceKey = keyof typeof PRECEDENCE;

export type LedProvenance =
  | "stock-hardware-confirmed"
  | "stock-guide"
  | "tape-looper-source"
  | "stem-tape-override"
  | "implementation-observation"
  | "unverified";

/** Owners above this are temporary and must name a restoration target. */
const TEMPORARY: PrecedenceKey[] = ["rejectionFlash", "confirmationFlash", "connectGreeting"];

// ------------------------------------------------------------- state -------

export interface Sp1LedTrackState {
  loaded: boolean;
  muted: boolean;
  soloed: boolean;
  linked: boolean;
  pressed: boolean;
  /** Per-lane tape reverse (universal lane layer), independent of Heads. */
  reverse: boolean;
  /** This lane's own capture loop. */
  looping: boolean;
  /** Scratch readiness — isolated stem scratch (fed by the scratch engine). */
  scratching: boolean;
  head: { loaded: boolean; muted: boolean; reverse: boolean; latched: boolean };
}

export interface Sp1LedBankState {
  /** Physical side index this bank occupies (4..7). */
  sideIndex: number;
  label: string;
  algorithmId: string;
  momentary: boolean;
  latched: boolean;
  rejected: string | null;
}

export interface AuthoritativeSp1LedState {
  power: "off" | "on";
  loading: boolean;
  error: string | null;
  playing: boolean;
  /**
   * LED Stage 2 — per-stem activity, 0..1, one INDEPENDENT envelope per stem
   * measured off that stem's own post-fader/post-solo analyser tap.
   */
  levels: readonly number[];
  tracks: Sp1LedTrackState[];
  activeStem: number;
  anySolo: boolean;
  fxOverlay: boolean;
  fxScope: "global" | "stem";
  banks: Sp1LedBankState[];
  globalLoop: { active: boolean; latched: boolean; division: 1 | 2 | 4 | 8 };
  /**
   * Real loop-wrap anchor, 0..1 through the current global loop, derived from
   * the audio engine's own position. `null` when the engine has no loop phase
   * to offer — the accent then falls back to the app clock.
   */
  loopPhase: number | null;
  /** Persistent slowed-tape state (musical rate below unity). */
  slow: boolean;
  /** Scratch readiness — master (all-stem) scratch, fed by the scratch engine. */
  scratch: { master: boolean };
  scrub: { direction: 0 | 1 | -1; speedIndex: ScrubSpeedIndex; latched: boolean; inertia: boolean };
  heads: { active: boolean };
  song: number;
  bankJumpArmed: boolean;
  /** Physical SP-1 recognized on the wire — finite greeting chase, or null. */
  connectGreeting: { startedAt: number } | null;
  /** Finite one-shots, expressed as absolute app-clock start times. */
  flash: { kind: "fx-latch" | "fx-unlatch" | "heads-reject" | "pitch"; startedAt: number } | null;
}

/** Extra LED-only context the reducer does not carry. */
export interface Sp1LedContext {
  levels?: readonly number[];
  /** 0..1 phase through the global loop, from the audio engine. */
  loopPhase?: number | null;
  scratch?: { master?: boolean; stems?: readonly boolean[] };
  /** Momentary semitone/rate confirmation, app-clock start time. */
  pitchFlashAt?: number | null;
}


/** Projects the reducer state onto the authoritative LED state. */
export function sp1LedStateFrom(
  state: SurfaceState,
  now: number,
  levelsOrContext: readonly number[] | Sp1LedContext = [0, 0, 0, 0],
): AuthoritativeSp1LedState {
  const ctx: Sp1LedContext = Array.isArray(levelsOrContext)
    ? { levels: levelsOrContext as readonly number[] }
    : (levelsOrContext as Sp1LedContext);
  const levels = ctx.levels ?? [0, 0, 0, 0];
  const perf = state.perf;
  const fx = fxStateOf(perf, fxTargetOf(perf));
  const banks: Sp1LedBankState[] = [0, 1, 2, 3].map((i) => {
    const bankIndex = bankOfButton(i);
    const bank = fx.banks[bankIndex]!;
    const def = algorithmDef(bankIndex, bank.selectedAlgorithm);
    const alg = bank.algorithms[bank.selectedAlgorithm]!;
    return {
      sideIndex: 4 + i,
      label: def.label,
      algorithmId: def.id,
      momentary: bank.momentary,
      latched: bank.latched,
      rejected: alg.rejected,
    };
  });

  const flash = (() => {
    if (state.headsRejectFlashAt != null && now - state.headsRejectFlashAt >= 0 && now - state.headsRejectFlashAt < PERIOD.oneShotDouble)
      return { kind: "heads-reject" as const, startedAt: state.headsRejectFlashAt };
    if (state.fxFlashAt != null && now - state.fxFlashAt >= 0 && now - state.fxFlashAt < PERIOD.oneShotSingle)
      return { kind: "fx-latch" as const, startedAt: state.fxFlashAt };
    // Momentary pitch/semitone confirmation. Purely finite: once it expires the
    // LED returns to whatever the composite says, with no state of its own.
    if (ctx.pitchFlashAt != null && now - ctx.pitchFlashAt >= 0 && now - ctx.pitchFlashAt < PERIOD.oneShotSingle)
      return { kind: "pitch" as const, startedAt: ctx.pitchFlashAt };
    return null;
  })();

  return {
    power: state.power,
    loading: false,
    error: state.grid.rejected ? "grid rejected" : null,
    playing: state.playing,
    levels,
    tracks: state.tracks.map((t, i) => ({
      loaded: t.content !== "empty",
      muted: t.content === "muted",
      soloed: perf.tracks[i]!.soloed,
      linked: perf.tracks[i]!.linked,
      pressed: state.pressed.includes(`track-button-${i + 1}` as never),
      reverse: t.laneReverse,
      looping: t.laneLoop.active,
      scratching: ctx.scratch?.stems?.[i] === true,
      head: { loaded: t.content !== "empty", muted: t.headMuted, reverse: t.headReverse, latched: t.headLatched },
    })),
    activeStem: perf.activeStem,
    anySolo: perf.tracks.some((t) => t.soloed),
    fxOverlay: perf.fxOverlay,
    fxScope: perf.fxScope === "global" ? "global" : "stem",
    banks,
    globalLoop: { ...state.globalLoop },
    loopPhase: ctx.loopPhase ?? null,
    slow: state.speed < 0.995,
    scratch: { master: ctx.scratch?.master === true },
    scrub: {
      direction: state.globalScrub,
      speedIndex: state.scrubSpeed,
      latched: state.scrubLatched,
      inertia: false,
    },
    heads: { active: state.headsMode },
    song: state.song,
    bankJumpArmed: state.bankJumpArmed,
    connectGreeting:
      state.sp1ConnectedAt != null &&
      now - state.sp1ConnectedAt >= 0 &&
      now - state.sp1ConnectedAt < SP1_CONNECT_GREETING_MS
        ? { startedAt: state.sp1ConnectedAt }
        : null,
    flash,
  };
}


// ------------------------------------------------------------ resolve ------

export interface ResolvedSp1Led {
  index: number;
  id: PhysicalLedId;
  name: string;
  mode: Sp1LedMode;
  /** Sampled 0..127 for this animation time. */
  brightness: number;
  /** Static floor for animated modes (latched states never go fully dark). */
  floor: number;
  owner: string;
  precedenceKey: PrecedenceKey;
  precedence: number;
  phaseAnchor: PhaseAnchor;
  periodMs: number | null;
  direction: "forward" | "reverse" | "none";
  provenance: LedProvenance;
  /** Highest-precedence candidate that did NOT win. */
  lostTo: string | null;
  /** Owner this LED restores to when a temporary behaviour ends. */
  restoreTo: string | null;
  /**
   * Persistent states composed ON TOP of the winning base, in application
   * order. These do not replace the base layer — they shape it, so loop,
   * reverse, FX, mute/solo, slow and scratch can all read at once.
   */
  modifiers: LedModifierKind[];
}

/** Composable persistent states, applied over the winning base candidate. */
export type LedModifierKind =
  | "mute"
  | "solo"
  | "non-solo"
  | "loop"
  | "reverse"
  | "fx"
  | "slow"
  | "scratch";

interface LedModifier {
  kind: LedModifierKind;
  /** Loop accent uses the real audio wrap phase when one exists. */
  phase?: number | null;
  periodMs?: number;
}


export interface ResolvedPhysicalLedFrame {
  leds: ResolvedSp1Led[];
  /** Exactly eight sampled MIDI values, index-ordered. */
  values: number[];
  /** Semantic signature — animation sampling never changes it. */
  signature: string;
  /** True while at least one mode needs a ticker. */
  animated: boolean;
  animationTimeMs: number;
}

interface Candidate {
  mode: Sp1LedMode;
  owner: string;
  key: PrecedenceKey;
  provenance: LedProvenance;
  periodMs?: number | null;
  phaseAnchor?: PhaseAnchor;
  direction?: "forward" | "reverse" | "none";
  floor?: number;
  /** Chase: this LED's position in the row (0..3). */
  chaseSlot?: number;
  /** One-shot start time on the app clock. */
  startedAt?: number;
  /** Activity mode: 0..1 envelope level for this stem. */
  level?: number;
}

const ANIMATED_MODES: Sp1LedMode[] = [
  "activity",
  "blink",
  "rapid-pulse",
  "breathe",
  "chase",
  "one-shot-single-flash",
  "one-shot-double-flash",
];

export function isAnimatedMode(mode: Sp1LedMode): boolean {
  return ANIMATED_MODES.includes(mode);
}

/** Deterministic brightness sample. Phase is anchored to the app clock. */
export function sampleBrightness(c: Candidate, t: number, index: number): number {
  const floor = c.floor ?? 0;
  const period = c.periodMs ?? 0;
  switch (c.mode) {
    case "off":
      return 0;
    case "dim":
      return BRIGHT_DIM;
    case "solid":
      return BRIGHT_FULL;
    case "activity": {
      // Base layer: the stem's own audible level. Silence reads genuinely
      // quiet; a transient reads as a hit. No compression to "fully on".
      const level = Math.max(0, Math.min(1, c.level ?? 0));
      const base = Math.max(floor, BRIGHT_TAIL * 0.5);
      return Math.round(base + (BRIGHT_FULL - base) * level);
    }
    case "blink":
    case "rapid-pulse": {
      const p = period || PERIOD.blink;
      return (t % p) < p / 2 ? BRIGHT_FULL : floor;
    }
    case "breathe": {
      const p = period || PERIOD.breathe;
      const phase = (t % p) / p;
      const v = (1 - Math.cos(phase * Math.PI * 2)) / 2;
      return Math.round(floor + (BRIGHT_FULL - floor) * (0.22 + 0.78 * v));
    }
    case "chase": {
      const p = period || SCRUB_LED_PERIODS_MS[1]!;
      const slots = 4;
      const step = Math.floor((t % p) / (p / slots)) % slots;
      const head = c.direction === "reverse" ? (slots - 1 - step) : step;
      const slot = c.chaseSlot ?? index;
      if (slot === head) return BRIGHT_FULL;
      const behind = c.direction === "reverse" ? (head + 1) % slots : (head + slots - 1) % slots;
      if (slot === behind) return BRIGHT_TAIL;
      return floor;
    }
    case "one-shot-single-flash": {
      const dt = t - (c.startedAt ?? 0);
      return dt >= 0 && dt < PERIOD.oneShotSingle ? BRIGHT_FULL : floor;
    }
    case "one-shot-double-flash": {
      const dt = t - (c.startedAt ?? 0);
      if (dt < 0 || dt >= PERIOD.oneShotDouble) return floor;
      const slice = PERIOD.oneShotDouble / 4;
      const n = Math.floor(dt / slice);
      return n === 0 || n === 2 ? BRIGHT_FULL : floor;
    }
  }
}

// ---------------------------------------------------------- modifiers ------

/** Loop accent duty — the fraction of a loop cycle the wrap tick occupies. */
const LOOP_TICK = 0.14;
const MUTE_CEILING = BRIGHT_TAIL; // hot source audio can never look "active"
const NON_SOLO_SCALE = 0.32;
const SOLO_SCALE = 1.35;

function cyclePhase(m: LedModifier, t: number, fallbackPeriod: number): number {
  if (m.phase != null) return ((m.phase % 1) + 1) % 1; // real audio wrap anchor
  const p = m.periodMs || fallbackPeriod;
  return (t % p) / p;
}

/**
 * Composition, not arbitration. Each persistent state SHAPES the winning base
 * layer instead of replacing it, so loop + reverse + FX + slow all remain
 * readable at once.
 */
export function applyModifiers(base: number, mods: readonly LedModifier[], t: number): number {
  let v = base;
  for (const m of mods) {
    switch (m.kind) {
      case "mute":
        // Strongly suppressed but still present: reads as muted, never active.
        v = Math.min(MUTE_CEILING, Math.round(v * 0.16));
        break;
      case "solo":
        // Emphasis WITHOUT flattening: the meter still moves underneath.
        v = Math.round(Math.min(BRIGHT_FULL, Math.max(v, BRIGHT_DIM) * SOLO_SCALE));
        break;
      case "non-solo":
        v = Math.round(v * NON_SOLO_SCALE);
        break;
      case "loop": {
        // Wrap tick on the real loop phase when the engine offers one.
        const ph = cyclePhase(m, t, PERIOD.latchedBlink);
        v = ph < LOOP_TICK ? Math.max(v, 104) : Math.round(v * 0.82);
        break;
      }
      case "reverse": {
        // Backwards stutter notch — unique to the reversed lane, and the
        // stem's own activity keeps modulating between notches.
        const ph = cyclePhase(m, t, PERIOD.latchedBlink);
        v = ph >= 0.62 && ph < 0.86 ? Math.round(v * 0.28) : v;
        break;
      }
      case "fx": {
        // FX presence: a lifted floor plus a shallow ripple. Never a takeover.
        const ph = cyclePhase(m, t, PERIOD.blink);
        const ripple = 1 + 0.16 * Math.sin(ph * Math.PI * 2);
        v = Math.round(Math.max(v, 26) * ripple);
        break;
      }
      case "slow": {
        // Persistent but subtle: a slow sag, always below the unmodified level.
        const ph = cyclePhase(m, t, 1600);
        v = Math.round(v * (0.72 + 0.14 * (1 - Math.cos(ph * Math.PI * 2)) / 2));
        break;
      }
      case "scratch": {
        // Scratch readiness — the compositor already consumes master and
        // per-stem scratch flags; the scratch engine only has to set them.
        const ph = cyclePhase(m, t, 160);
        v = ph < 0.5 ? Math.min(BRIGHT_FULL, v + 34) : Math.round(v * 0.55);
        break;
      }
    }
  }
  return Math.max(0, Math.min(BRIGHT_FULL, Math.round(v)));
}

/** Modifier keys whose winning base must NOT be reshaped (finite/critical). */
const UNCOMPOSED: PrecedenceKey[] = [
  "power",
  "error",
  "connectGreeting",
  "loading",
  "rejectionFlash",
  "confirmationFlash",
  "heads",
  "scrub",
];

function anyFxEngaged(s: AuthoritativeSp1LedState): boolean {
  return s.banks.some((b) => b.latched || b.momentary);
}

function trackModifiers(s: AuthoritativeSp1LedState, i: number): LedModifier[] {
  const t = s.tracks[i]!;
  const mods: LedModifier[] = [];
  if (!t.loaded) return mods;
  if (t.muted) mods.push({ kind: "mute" });
  else if (t.soloed) mods.push({ kind: "solo" });
  else if (s.anySolo) mods.push({ kind: "non-solo" });

  const looping = s.globalLoop.active || s.globalLoop.latched || t.looping;
  if (looping) mods.push({ kind: "loop", phase: s.loopPhase, periodMs: loopPeriodFor(s.globalLoop.division) });
  if (t.reverse) mods.push({ kind: "reverse" });
  // Authoritative FX state, overlay open or not. Stem-scoped FX only accents
  // the stem it is actually applied to.
  if (anyFxEngaged(s) && (s.fxScope === "global" || s.activeStem === i)) mods.push({ kind: "fx" });
  if (s.slow) mods.push({ kind: "slow" });
  if (s.scratch.master || t.scratching) mods.push({ kind: "scratch" });
  return mods;
}

function sideModifiers(s: AuthoritativeSp1LedState, index: number): LedModifier[] {
  const mods: LedModifier[] = [];
  const looping = s.globalLoop.active || s.globalLoop.latched;
  // The loop keeps its wrap tick on the PLAY-side LED even when an FX bank
  // owns that LED, so FX can never erase the loop indication.
  if (looping && index === PLAY_SIDE_INDEX)
    mods.push({ kind: "loop", phase: s.loopPhase, periodMs: loopPeriodFor(s.globalLoop.division) });
  if (s.slow && index === PLAY_SIDE_INDEX) mods.push({ kind: "slow" });
  if (s.scratch.master && index === PLAY_SIDE_INDEX) mods.push({ kind: "scratch" });
  return mods;
}


function pick(cands: Candidate[]): { win: Candidate; lost: Candidate | null; restore: Candidate | null } {
  const sorted = [...cands].sort((a, b) => PRECEDENCE[b.key] - PRECEDENCE[a.key]);
  const win = sorted[0]!;
  const lost = sorted[1] ?? null;
  const restore = TEMPORARY.includes(win.key) ? (sorted.find((c) => !TEMPORARY.includes(c.key)) ?? null) : null;
  return { win, lost, restore };
}

const BASE: Candidate = { mode: "off", owner: "base — nothing to show", key: "base", provenance: "tape-looper-source" };

function trackCandidates(s: AuthoritativeSp1LedState, i: number): Candidate[] {
  const out: Candidate[] = [BASE];
  const t = s.tracks[i]!;

  if (s.power === "off")
    return [{ mode: "off", owner: "powered off", key: "power", provenance: "tape-looper-source" }];

  if (s.error)
    out.push({ mode: "blink", owner: `error — ${s.error}`, key: "error", provenance: "tape-looper-source", periodMs: PERIOD.blink });
  if (s.connectGreeting)
    out.push({
      mode: "chase",
      owner: "Stem Tape SP-1 connected — Track LED greeting",
      key: "connectGreeting",
      provenance: "implementation-observation",
      periodMs: SP1_CONNECT_GREETING_LAP_MS,
      direction: "forward",
      chaseSlot: i,
      phaseAnchor: "app-clock",
    });
  if (s.loading)
    out.push({ mode: "breathe", owner: "song loading", key: "loading", provenance: "tape-looper-source", periodMs: PERIOD.breathe });

  if (s.flash?.kind === "heads-reject")
    out.push({
      mode: "one-shot-double-flash",
      owner: "heads refused — no decoded Vocal (double flash)",
      key: "rejectionFlash",
      provenance: "stem-tape-override",
      startedAt: s.flash.startedAt,
      phaseAnchor: "one-shot-start",
      periodMs: PERIOD.oneShotDouble,
    });
  if (s.flash?.kind === "pitch")
    out.push({
      mode: "one-shot-single-flash",
      owner: "semitone / rate confirmation (momentary)",
      key: "confirmationFlash",
      provenance: "stem-tape-override",
      startedAt: s.flash.startedAt,
      phaseAnchor: "one-shot-start",
      periodMs: PERIOD.oneShotSingle,
    });
  if (s.flash?.kind === "fx-latch" || s.flash?.kind === "fx-unlatch")

    out.push({
      mode: "one-shot-single-flash",
      owner: "FX latch confirmation — all four Track LEDs flash once",
      key: "confirmationFlash",
      provenance: "stem-tape-override",
      startedAt: s.flash.startedAt,
      phaseAnchor: "one-shot-start",
      periodMs: PERIOD.oneShotSingle,
    });

  if (s.heads.active) {
    const h = t.head;
    if (h.latched)
      out.push({ mode: "solid", owner: `head ${i + 1} latched`, key: "heads", provenance: "stem-tape-override" });
    else if (h.muted)
      out.push({ mode: "dim", owner: `head ${i + 1} muted`, key: "heads", provenance: "stem-tape-override" });
    else if (h.loaded)
      out.push({
        mode: "chase",
        owner: `head ${i + 1} ${h.reverse ? "reverse" : "forward"} chase over loaded content`,
        key: "heads",
        provenance: "tape-looper-source",
        periodMs: SCRUB_LED_PERIODS_MS[1],
        direction: h.reverse ? "reverse" : "forward",
        chaseSlot: i,
        phaseAnchor: "app-clock",
      });
    else out.push({ mode: "dim", owner: `head ${i + 1} empty lane`, key: "heads", provenance: "tape-looper-source", floor: 0 });
  }

  if (s.scrub.direction !== 0 || s.scrub.latched) {
    const dir = s.scrub.direction >= 0 ? "forward" : "reverse";
    out.push({
      mode: "chase",
      owner: `scrub ${dir} ×${SCRUB_MULTIPLIERS[s.scrub.speedIndex]!.toFixed(2)}${s.scrub.latched ? " (latched)" : " (momentary)"}`,
      key: "scrub",
      provenance: "stock-hardware-confirmed",
      periodMs: SCRUB_LED_PERIODS_MS[s.scrub.speedIndex]!,
      direction: dir,
      chaseSlot: i,
      phaseAnchor: "gesture-start",
      floor: s.scrub.latched ? 12 : 0,
    });
  }

  // ---- base layer -----------------------------------------------------
  // During PLAY every loaded stem's base is its OWN audio activity — mute,
  // solo, loop, reverse, FX, slow and scratch are composed over it as
  // modifiers (see trackModifiers) rather than replacing it.
  const meterBase = s.playing && t.loaded;
  if (meterBase)
    out.push({
      mode: "activity",
      owner: `stem ${i + 1} activity`,
      key: "transport",
      provenance: "stem-tape-override",
      periodMs: null,
      phaseAnchor: "stem-audio",
      level: s.levels?.[i] ?? 0,
      floor: 6,
    });
  else if (t.loaded)
    out.push({
      mode: "dim",
      owner: `stem ${i + 1} loaded, stopped`,
      key: "transport",
      provenance: "tape-looper-source",
      periodMs: null,
    });

  // ---- stopped / overlay-only semantics --------------------------------
  // These are the states that have no meter to shape, so they still own the
  // LED outright. While playing they become modifiers instead.
  if (!meterBase) {
    if (t.soloed) out.push({ mode: "solid", owner: `stem ${i + 1} soloed`, key: "muteSoloLink", provenance: "stem-tape-override" });
    if (t.muted) out.push({ mode: "dim", owner: `stem ${i + 1} muted`, key: "muteSoloLink", provenance: "tape-looper-source" });
    if (s.activeStem === i && t.loaded)
      out.push({ mode: "breathe", owner: `active stem ${i + 1}`, key: "activeStem", provenance: "stem-tape-override", periodMs: PERIOD.breathe });
    if (s.fxOverlay && !t.soloed && t.linked && s.activeStem !== i)
      out.push({ mode: "dim", owner: `stem ${i + 1} idle (overlay)`, key: "base", provenance: "stem-tape-override" });
  }
  // Link state is an editing affordance, not a performance state: it only owns
  // the LED when there is no live meter to preserve, or while the FX overlay
  // (where linking is edited) is open.
  if (!t.linked && (!meterBase || s.fxOverlay))
    out.push({ mode: "blink", owner: `stem ${i + 1} unlinked`, key: "muteSoloLink", provenance: "stem-tape-override", periodMs: PERIOD.latchedBlink });


  if (t.pressed)
    out.push({ mode: "solid", owner: `Track ${i + 1} held (input feedback)`, key: "fxSelection", provenance: "implementation-observation" });

  return out;
}

function sideCandidates(s: AuthoritativeSp1LedState, index: number): Candidate[] {
  const slot = index - 4; // 0..3
  const out: Candidate[] = [BASE];

  if (s.power === "off")
    return [{ mode: "off", owner: "powered off", key: "power", provenance: "tape-looper-source" }];

  out.push({ mode: "off", owner: "battery/status baseline idle", key: "battery", provenance: "tape-looper-source" });

  if (s.loading)
    out.push({
      mode: "chase",
      owner: "song loading",
      key: "loading",
      provenance: "tape-looper-source",
      periodMs: SCRUB_LED_PERIODS_MS[2],
      direction: "forward",
      chaseSlot: slot,
      phaseAnchor: "app-clock",
    });

  // Song row (stopped baseline).
  if (s.bankJumpArmed && slot === s.song % 4)
    out.push({ mode: "blink", owner: `bank jump armed — slot ${slot + 1}`, key: "songRow", provenance: "tape-looper-source", periodMs: PERIOD.blink });
  else if (slot === s.song % 4)
    out.push({ mode: "solid", owner: `song slot ${slot + 1}`, key: "songRow", provenance: "tape-looper-source" });

  // Normal transport — index 4 (nearest PLAY) is fully lit while playing.
  if (s.playing && index === PLAY_SIDE_INDEX)
    out.push({
      mode: "solid",
      owner: "transport playing — side LED nearest PLAY fully illuminated",
      key: "transport",
      provenance: "stock-hardware-confirmed",
    });

  // Global loop: index 4 carries the loop, 5..7 carry the division.
  const loop = s.globalLoop;
  if (loop.active || loop.latched) {
    if (index === PLAY_SIDE_INDEX)
      out.push({
        mode: "blink",
        owner: `global loop ${loop.latched ? "latched" : "momentary"} · 1/${loop.division}`,
        key: "loop",
        provenance: "stock-hardware-confirmed",
        periodMs: loopPeriodFor(loop.division),
        floor: loop.latched ? 32 : 0,
        phaseAnchor: "loop-wrap",
      });
    else {
      const bars = Math.log2(loop.division); // 0..3 extra LEDs
      if (slot >= 1 && slot <= bars)
        out.push({
          mode: "dim",
          owner: `loop division 1/${loop.division}`,
          key: "loop",
          provenance: "stock-hardware-confirmed",
        });
    }
  }

  // FX bank feedback while the overlay is open.
  if (s.fxOverlay) {
    const bank = s.banks[slot]!;
    const gate = bank.algorithmId === "gate";
    if (bank.rejected)
      out.push({
        mode: "one-shot-double-flash",
        owner: `${bank.label} rejected: ${bank.rejected}`,
        key: "rejectionFlash",
        provenance: "stem-tape-override",
        startedAt: s.flash?.startedAt ?? 0,
        periodMs: PERIOD.oneShotDouble,
        phaseAnchor: "one-shot-start",
      });
    else if (bank.momentary || bank.latched)
      out.push({
        mode: gate ? "rapid-pulse" : "blink",
        owner: `${bank.label} ${bank.latched ? "latched" : "momentary"} (${s.fxScope} scope)`,
        key: "fxActive",
        provenance: "stock-hardware-confirmed",
        periodMs: gate ? PERIOD.rapidPulse : PERIOD.blink,
        floor: bank.latched ? 32 : 0,
        phaseAnchor: "app-clock",
      });
    else
      out.push({
        mode: "dim",
        owner: `${bank.label} selected, inactive`,
        key: "fxSelection",
        provenance: "stem-tape-override",
      });
  } else {
    // Authoritative FX state, overlay CLOSED: an engaged bank stays visible on
    // its own side LED. Loop keeps its wrap tick over the top (sideModifiers).
    const bank = s.banks[slot]!;
    if (bank.momentary || bank.latched) {
      const gate = bank.algorithmId === "gate";
      out.push({
        mode: gate ? "rapid-pulse" : "blink",
        owner: `${bank.label} ${bank.latched ? "latched" : "momentary"} (${s.fxScope} scope, overlay closed)`,
        key: "fxActive",
        provenance: "stem-tape-override",
        periodMs: gate ? PERIOD.rapidPulse : PERIOD.blink,
        floor: bank.latched ? 32 : 0,
        phaseAnchor: "app-clock",
      });
    }
  }


  if (s.heads.active && index === 7)
    out.push({ mode: "breathe", owner: "heads mode active", key: "heads", provenance: "stem-tape-override", periodMs: PERIOD.breathe });

  if ((s.scrub.direction !== 0 || s.scrub.latched) && index === PLAY_SIDE_INDEX)
    out.push({
      mode: "blink",
      owner: `shuttle ${s.scrub.direction >= 0 ? "forward" : "reverse"} ×${SCRUB_MULTIPLIERS[s.scrub.speedIndex]!.toFixed(2)}`,
      key: "scrub",
      provenance: "stock-hardware-confirmed",
      periodMs: SCRUB_LED_PERIODS_MS[s.scrub.speedIndex]!,
      floor: s.scrub.latched ? 32 : 0,
      phaseAnchor: "gesture-start",
    });

  return out;
}

/**
 * The single authoritative resolver. Always returns exactly eight LEDs.
 */
export function resolveSp1LedFrame(
  state: AuthoritativeSp1LedState,
  animationTimeMs: number,
): ResolvedPhysicalLedFrame {
  const leds: ResolvedSp1Led[] = PHYSICAL_LED_MAP.map((slot) => {
    const cands = slot.index < 4 ? trackCandidates(state, slot.index) : sideCandidates(state, slot.index);
    const { win, lost, restore } = pick(cands);
    return {
      index: slot.index,
      id: slot.id,
      name: slot.name,
      mode: win.mode,
      brightness: Math.max(0, Math.min(127, Math.round(sampleBrightness(win, animationTimeMs, slot.index)))),
      floor: win.floor ?? 0,
      owner: win.owner,
      precedenceKey: win.key,
      precedence: PRECEDENCE[win.key],
      phaseAnchor: win.phaseAnchor ?? (isAnimatedMode(win.mode) ? "app-clock" : "none"),
      periodMs: win.periodMs ?? null,
      direction: win.direction ?? "none",
      provenance: win.provenance,
      lostTo: lost ? lost.owner : null,
      restoreTo: restore ? restore.owner : null,
    };
  });

  return {
    leds,
    values: leds.map((l) => l.brightness),
    signature: leds
      .map((l) => `${l.index}:${l.mode}:${l.owner}:${l.precedence}:${l.periodMs ?? "-"}:${l.direction}:${l.floor}`)
      .join("|"),
    animated: leds.some((l) => isAnimatedMode(l.mode)),
    animationTimeMs,
  };
}

/** `[T1 0, T2 0, T3 127, T4 0 | S1 127, S2 0, S3 0, S4 0] owner=…` */
export function formatSp1Frame(frame: ResolvedPhysicalLedFrame): string {
  const short = ["T1", "T2", "T3", "T4", "S1", "S2", "S3", "S4"];
  const cell = (i: number) => `${short[i]} ${frame.values[i] ?? 0}`;
  const top = [...frame.leds].sort((a, b) => b.precedence - a.precedence)[0];
  return `[${[0, 1, 2, 3].map(cell).join(", ")} | ${[4, 5, 6, 7].map(cell).join(", ")}] owner=${top?.owner ?? "none"}`;
}
