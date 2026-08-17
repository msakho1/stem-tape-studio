/**
 * Authoritative, VERSIONED SP-1 behaviour contract.
 *
 * Every entry carries an explicit provenance classification and citation. The
 * current website implementation and its CSS are NEVER evidence of physical
 * SP-1 behaviour; the M0 diagnostic firmware's temporary LED patterns are
 * NEVER stock behaviour. Anything without a source is `UNVERIFIED`.
 *
 * This module is DECLARATIVE ONLY. It never changes behaviour; the drawer
 * evaluates observed state against it.
 */

import type { SurfaceState } from "@/machine/surface";
import {
  expectedPhysicalFrame,
  led,
  validatePhysicalFrame,
  type DivergenceCategory,
  type ExpectedPhysicalLedFrame,
} from "./physical";

export const BEHAVIOR_CONTRACT_VERSION = "sp1-behavior-contract/4.0.0";

export type Provenance =
  | "STOCK_SP1_DOCUMENTED"
  | "TAPE_LOOPER_SOURCE"
  | "PHYSICAL_OBSERVATION"
  | "M0_DIAGNOSTIC_ONLY"
  | "STEM_TAPE_OVERRIDE"
  | "UNVERIFIED";

export type EvidenceKind = "documentary" | "pinned source" | "physical observation" | "inference";

export type Confidence = "high" | "medium" | "low" | "none";

import type { ObservationSource, ReproductionResult, ReproductionStatus } from "./segments";

export type ImplStatus = "implemented" | "partial" | "missing" | "conflicting" | "unverified";

export type ContractGroup =
  | "base surface"
  | "function+volume"
  | "global loop"
  | "global scrub"
  | "fx"
  | "heads"
  | "m0-only";

export interface Citation {
  title: string;
  /** Version, commit or date. */
  version: string;
  /** Page, section, line, function or code reference. */
  locator: string;
  evidence: EvidenceKind;
}

export interface ContractEntry {
  id: string;
  group: ContractGroup;
  name: string;
  /** State the surface must be in before the sequence begins. */
  initiatingState: string;
  sequence: string;
  timing: string;
  expectedOwner: string;
  expectedCommand: string;
  expectedEngineResult: string;
  /** COMPLETE eight-LED physical frame: 4 Track + 4 side/status. Never partial. */
  expectedLeds: ExpectedPhysicalLedFrame;
  /** Human summary of the expected frame, for the text report. */
  expectedLedSummary: string;
  precedence: number;
  competing: string[];
  provenance: Provenance;
  citation: Citation;
  confidence: Confidence;
  status: ImplStatus;
  notes?: string;
  firstDivergence?: DivergenceCategory;
  /** Observed evaluation from live reducer state, when one is derivable. */
  observe?: (s: SurfaceState) => string;
}

const LOOPER: Citation = {
  title: "Teenage Engineering Tape Looper (SP-1 community firmware)",
  version: "v2.6 · pinned commit a8dd127",
  locator: "control map + led driver",
  evidence: "pinned source",
};
const ADDENDUM = (section: string): Citation => ({
  title: "Stem Tape stock-SP-1 behaviour addendum",
  version: "2026-08 approved addendum",
  locator: section,
  evidence: "documentary",
});
const STEMTAPE = (locator: string): Citation => ({
  title: "Stem Tape implementation",
  version: "current working tree",
  locator,
  evidence: "pinned source",
});
const M0 = (locator: string): Citation => ({
  title: "Stem Tape M0 diagnostic firmware + bench observation",
  version: "v1.0.0 · source commit ea354f32c8c484c1d48e68804a4f1695a8a7b131",
  locator,
  evidence: "physical observation",
});
const NONE: Citation = {
  title: "no source located",
  version: "n/a",
  locator: "n/a",
  evidence: "inference",
};

const tracks = (l: ReturnType<typeof led>) => ({
  "track-led-1": l,
  "track-led-2": l,
  "track-led-3": l,
  "track-led-4": l,
});

export const BEHAVIOR_CONTRACT: ContractEntry[] = [
  // ---------------------------------------------------------------- base ---
  {
    id: "base.power.off",
    group: "base surface",
    name: "Power off",
    initiatingState: "device powered down",
    sequence: "n/a",
    timing: "n/a",
    expectedOwner: "none",
    expectedCommand: "none",
    expectedEngineResult: "no audio",
    expectedLeds: expectedPhysicalFrame({}),
    expectedLedSummary: "all eight physical LEDs dark (4 Track + 4 side/status)",
    precedence: 100,
    competing: [],
    provenance: "TAPE_LOOPER_SOURCE",
    citation: LOOPER,
    confidence: "high",
    status: "implemented",
    observe: (s) => `power=${s.power}`,
  },
  {
    id: "base.startup",
    group: "base surface",
    name: "Startup signature",
    initiatingState: "power applied",
    sequence: "boot",
    timing: "two flashes",
    expectedOwner: "firmware",
    expectedCommand: "none",
    expectedEngineResult: "n/a",
    expectedLeds: expectedPhysicalFrame(tracks(led("one-shot double flash", { durationMs: 400 }))),
    expectedLedSummary: "four Track LEDs flash twice (M0 diagnostic build only)",
    precedence: 99,
    competing: [],
    provenance: "M0_DIAGNOSTIC_ONLY",
    citation: M0("boot_signature()"),
    confidence: "high",
    status: "unverified",
    notes: "M0 diagnostic pattern; NOT evidence of stock SP-1 startup behaviour.",
  },
  {
    id: "base.idle.empty",
    group: "base surface",
    name: "Idle, no content",
    initiatingState: "powered on, no stems loaded",
    sequence: "n/a",
    timing: "n/a",
    expectedOwner: "base",
    expectedCommand: "none",
    expectedEngineResult: "silence",
    expectedLeds: expectedPhysicalFrame({}),
    expectedLedSummary: "Track LEDs dark = empty",
    precedence: 0,
    competing: [],
    provenance: "TAPE_LOOPER_SOURCE",
    citation: LOOPER,
    confidence: "medium",
    status: "implemented",
    observe: (s) => s.tracks.map((t, i) => `${i + 1}:${t.content}`).join(" "),
  },
  {
    id: "base.loaded.stopped",
    group: "base surface",
    name: "Loaded and stopped",
    initiatingState: "stems loaded, transport stopped",
    sequence: "n/a",
    timing: "n/a",
    expectedOwner: "base",
    expectedCommand: "none",
    expectedEngineResult: "silence",
    expectedLeds: expectedPhysicalFrame(tracks(led("dim"))),
    expectedLedSummary: "Track LEDs dim",
    precedence: 10,
    competing: [],
    provenance: "TAPE_LOOPER_SOURCE",
    citation: LOOPER,
    confidence: "medium",
    status: "implemented",
    observe: (s) => (s.playing ? "playing" : "stopped"),
  },
  {
    id: "base.playing",
    group: "base surface",
    name: "Playing",
    initiatingState: "stems loaded",
    sequence: "PLAY tap",
    timing: "tap < 450 ms",
    expectedOwner: "transport",
    expectedCommand: "transport.play",
    expectedEngineResult: "transport running from the shared timeline anchor",
    expectedLeds: expectedPhysicalFrame(
      tracks(led("pulse", { periodMs: 1200, phaseAnchor: "loop-wrap" })),
    ),
    expectedLedSummary: "Track LEDs pulse on loop wrap",
    precedence: 20,
    competing: ["loop.momentary"],
    provenance: "TAPE_LOOPER_SOURCE",
    citation: LOOPER,
    confidence: "medium",
    status: "partial",
    notes: "Pulse is a fixed CSS cycle, not anchored to loop wrap or beat phase.",
    firstDivergence: "DOM state correct but CSS timing wrong",
    observe: (s) => (s.playing ? "playing" : "stopped"),
  },
  {
    id: "track.mute",
    group: "base surface",
    name: "Track mute / unmute",
    initiatingState: "FX overlay closed, no modifier held",
    sequence: "Track n tap",
    timing: "tap",
    expectedOwner: "track",
    expectedCommand: "stem.mute / stem.unmute",
    expectedEngineResult: "stem gain to 0 / restored",
    expectedLeds: expectedPhysicalFrame({ "track-led-1": led("dim") }),
    expectedLedSummary: "muted Track dim, playing Track pulses; side row unchanged",
    precedence: 20,
    competing: ["fx.momentary", "play.track.solo"],
    provenance: "TAPE_LOOPER_SOURCE",
    citation: LOOPER,
    confidence: "high",
    status: "implemented",
    observe: (s) => s.tracks.map((t, i) => `${i + 1}:${t.content}`).join(" "),
  },
  {
    id: "play.track.solo",
    group: "base surface",
    name: "PLAY + Track solo / link (FX closed)",
    initiatingState: "FX overlay closed",
    sequence: "PLAY held, then Track n",
    timing: "PLAY < 450 ms before Track; overlap < 700 ms = solo, ≥ = link/unlink",
    expectedOwner: "solo/link chord",
    expectedCommand: "stem.solo / stem.link",
    expectedEngineResult: "non-soloed stems muted",
    expectedLeds: expectedPhysicalFrame({ "track-led-1": led("solid") }),
    expectedLedSummary: "soloed Track solid, others dim",
    precedence: 74,
    competing: ["loop.momentary", "track.mute"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: STEMTAPE("src/machine/chordArbiter.ts (soloLinkMs 700, globalLoopClaimMs 450)"),
    confidence: "high",
    status: "partial",
    notes: "Solo LED indication renders only while the FX overlay is open (deriveLeds row 5).",
    firstDivergence: "audio state changed but LED derivation was wrong",
    observe: (s) => s.perf.tracks.map((t, i) => `${i + 1}${t.soloed ? "S" : ""}${t.linked ? "L" : ""}`).join(" "),
  },
  {
    id: "stem.active",
    group: "base surface",
    name: "Active-stem indication",
    initiatingState: "any",
    sequence: "FUNCTION tap arms selection, then Track n",
    timing: "selection armed window",
    expectedOwner: "selection",
    expectedCommand: "stem.select",
    expectedEngineResult: "no audio change",
    expectedLeds: expectedPhysicalFrame({ "track-led-1": led("breathe", { periodMs: 2400 }) }),
    expectedLedSummary: "active Track breathes, in and out of the FX overlay",
    precedence: 66,
    competing: ["fx.momentary"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: STEMTAPE("src/machine/surface.ts trackSelectArmedAt"),
    confidence: "high",
    status: "partial",
    notes: "Breathe is emitted only inside the FX overlay; outside it the active stem has no LED.",
    firstDivergence: "expected contract missing",
    observe: (s) => `activeStem=${s.perf.activeStem + 1}`,
  },
  {
    id: "base.song.bank",
    group: "base surface",
    name: "Song / bank indication",
    initiatingState: "songs present",
    sequence: "n/a",
    timing: "n/a",
    expectedOwner: "base",
    expectedCommand: "none",
    expectedEngineResult: "n/a",
    expectedLeds: expectedPhysicalFrame({ "side-led-1": led("solid") }),
    expectedLedSummary: "side LED solid = song, blink = bank",
    precedence: 40,
    competing: ["fx.momentary"],
    provenance: "TAPE_LOOPER_SOURCE",
    citation: LOOPER,
    confidence: "medium",
    status: "implemented",
    observe: (s) => `bank=${s.bank + 1}`,
  },
  {
    id: "base.grid.reject",
    group: "base surface",
    name: "Grid rejection",
    initiatingState: "grid far from detected tempo",
    sequence: "any transport gesture",
    timing: "immediate",
    expectedOwner: "grid guard",
    expectedCommand: "(rejected)",
    expectedEngineResult: "nothing moves",
    expectedLeds: expectedPhysicalFrame(tracks(led("blink", { periodMs: 400 }))),
    expectedLedSummary: "all four Track LEDs blink, nothing moves",
    precedence: 98,
    competing: [],
    provenance: "TAPE_LOOPER_SOURCE",
    citation: LOOPER,
    confidence: "medium",
    status: "implemented",
  },
  {
    id: "base.heads.reject",
    group: "base surface",
    name: "Heads rejection",
    initiatingState: "Vocal stem not decoded",
    sequence: "FUNCTION + PLAY",
    timing: "immediate",
    expectedOwner: "heads guard",
    expectedCommand: "(rejected)",
    expectedEngineResult: "no head lanes",
    expectedLeds: expectedPhysicalFrame(tracks(led("one-shot double flash", { durationMs: 300 }))),
    expectedLedSummary: "rejection flash, then restoration",
    precedence: 97,
    competing: ["heads.mode"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: STEMTAPE("src/audio/heads.ts"),
    confidence: "medium",
    status: "conflicting",
    notes: "Rejection is rendered as an infinite CSS blink with no expiry ticker.",
    firstDivergence: "timing/clock missing",
  },
  {
    id: "base.loading",
    group: "base surface",
    name: "Loading / decode / engine / export failure",
    initiatingState: "ingest or export in flight",
    sequence: "n/a",
    timing: "n/a",
    expectedOwner: "engine",
    expectedCommand: "n/a",
    expectedEngineResult: "decode or export result",
    expectedLeds: expectedPhysicalFrame({}),
    expectedLedSummary: "no physical LED representation defined",
    precedence: 5,
    competing: [],
    provenance: "UNVERIFIED",
    citation: NONE,
    confidence: "none",
    status: "missing",
    notes: "Loading, decode failure, engine failure and export failure have no physical LED contract.",
    firstDivergence: "expected contract missing",
  },

  // ------------------------------------------------------ function+volume ---
  {
    id: "function.volume",
    group: "function+volume",
    name: "Contextual FUNCTION + Volume ownership",
    initiatingState: "any",
    sequence: "FUNCTION held + Volume ±",
    timing: "duration-independent while FUNCTION is held",
    expectedOwner: "FX target → loop division → active stem, in that order",
    expectedCommand: "fx.target / loop.division / stem.select — never master volume",
    expectedEngineResult: "context-appropriate change only",
    expectedLeds: expectedPhysicalFrame({}),
    expectedLedSummary: "no physical LED indicates FUNCTION hold — the `••` marks are printed artwork",
    precedence: 70,
    competing: ["fx.scope", "loop.momentary"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: ADDENDUM("§1 contextual FUNCTION + Volume"),
    confidence: "high",
    status: "partial",
    notes: "The web build lights a non-physical function indicator; the SP-1 has no FUNCTION LED. Any physical indication would have to use the shared side/status row, and no source establishes an index.",
    firstDivergence: "expected contract missing",
    observe: (s) => `division=1/${s.globalLoop.division} activeStem=${s.perf.activeStem + 1}`,
  },

  // ----------------------------------------------------------- global loop ---
  {
    id: "loop.momentary",
    group: "global loop",
    name: "Momentary global loop",
    initiatingState: "playing",
    sequence: "PLAY held ≥ 450 ms",
    timing: "claim at 450 ms",
    expectedOwner: "global loop",
    expectedCommand: "loop.global.enter → loop.global.exit on release",
    expectedEngineResult: "song timeline loops; release continues from the audible frame",
    expectedLeds: expectedPhysicalFrame(
      tracks(led("pulse", { periodMs: 500, phaseAnchor: "loop-wrap" })),
    ),
    expectedLedSummary: "all four Track LEDs pulse in the loop division",
    precedence: 60,
    competing: ["play.track.solo", "fx.momentary"],
    provenance: "TAPE_LOOPER_SOURCE",
    citation: LOOPER,
    confidence: "medium",
    status: "conflicting",
    notes: "globalLoop.active is never read by deriveLeds — no LED indicates a held loop.",
    firstDivergence: "expected contract missing",
    observe: (s) => `active=${s.globalLoop.active} latched=${s.globalLoop.latched}`,
  },
  {
    id: "loop.latch",
    group: "global loop",
    name: "FUNCTION loop latch",
    initiatingState: "momentary global loop held",
    sequence: "tap FUNCTION while PLAY remains held",
    timing: "tap",
    expectedOwner: "global loop",
    expectedCommand: "loop.global.latch",
    expectedEngineResult: "loop survives PLAY release",
    expectedLeds: expectedPhysicalFrame(tracks(led("solid"))),
    expectedLedSummary: "Track LEDs stay lit after PLAY release, distinct from momentary",
    precedence: 61,
    competing: ["loop.momentary"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: ADDENDUM("§2 global-loop latching"),
    confidence: "high",
    status: "conflicting",
    notes: "State latches correctly; no LED distinguishes latched from momentary.",
    firstDivergence: "expected contract missing",
    observe: (s) => `latched=${s.globalLoop.latched}`,
  },
  {
    id: "loop.exit",
    group: "global loop",
    name: "PLAY exits a latched loop",
    initiatingState: "latched global loop",
    sequence: "PLAY tap",
    timing: "tap",
    expectedOwner: "global loop",
    expectedCommand: "loop.global.exit",
    expectedEngineResult: "transport continues from the audible frame",
    expectedLeds: expectedPhysicalFrame(tracks(led("pulse", { periodMs: 1200, phaseAnchor: "loop-wrap" }))),
    expectedLedSummary: "Track LEDs return to per-track content state",
    precedence: 62,
    competing: ["track.mute"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: ADDENDUM("§2 global-loop latching"),
    confidence: "high",
    status: "partial",
    notes: "Observed on the bench to emit a track.mute for Track 1 before loop.global.exit.",
    firstDivergence: "gesture arbitration selected the wrong owner",
    observe: (s) => (s.globalLoop.latched ? "latched — PLAY will exit" : "no latched loop"),
  },
  {
    id: "loop.division",
    group: "global loop",
    name: "Loop-division indication",
    initiatingState: "global loop active",
    sequence: "FUNCTION + Volume ±",
    timing: "duration-independent",
    expectedOwner: "global loop",
    expectedCommand: "loop.division",
    expectedEngineResult: "loop length changes at the next wrap",
    expectedLeds: expectedPhysicalFrame({}),
    expectedLedSummary: "no physical indication defined",
    precedence: 59,
    competing: ["function.volume"],
    provenance: "UNVERIFIED",
    citation: NONE,
    confidence: "none",
    status: "missing",
    firstDivergence: "expected contract missing",
    observe: (s) => `1/${s.globalLoop.division}`,
  },

  // ---------------------------------------------------------- global scrub ---
  {
    id: "scrub.momentary",
    group: "global scrub",
    name: "FUNCTION + rocker momentary scrub",
    initiatingState: "playing",
    sequence: "FUNCTION held BEFORE rocker deflection",
    timing: "FUNCTION down first; ownership survives FUNCTION release",
    expectedOwner: "global scrub",
    expectedCommand: "scrub.start / scrub.end",
    expectedEngineResult: "audible shuttle at the selected multiplier",
    expectedLeds: expectedPhysicalFrame({
      "side-led-1": led("chase", { periodMs: 550, direction: "forward", indexUnverified: true }),
    }),
    expectedLedSummary:
      "shuttle indication belongs to the shared side/status row — physical index UNVERIFIED (shown on side-led-1 as a placeholder, not evidence)",
    precedence: 45,
    competing: ["fx.momentary", "loop.momentary"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: ADDENDUM("§3-4 scrub"),
    confidence: "high",
    status: "partial",
    notes: "Chase does not distinguish forward from reverse, and no source establishes which side/status LED stock firmware uses for shuttle.",
    firstDivergence: "physical LED index unverified",
    observe: (s) => `globalScrub=${s.globalScrub}`,
  },
  {
    id: "scrub.latch",
    group: "global scrub",
    name: "Scrub latch and bare-FUNCTION unlatch",
    initiatingState: "shuttling",
    sequence: "FUNCTION tap while rocker held latches; completed bare FUNCTION tap unlatches; rocker touch alone does not",
    timing: "tap",
    expectedOwner: "global scrub",
    expectedCommand: "scrub.latch / scrub.unlatch",
    expectedEngineResult: "shuttle survives rocker release",
    expectedLeds: expectedPhysicalFrame({
      "side-led-1": led("chase", { periodMs: 550, indexUnverified: true }),
    }),
    expectedLedSummary: "latched shuttle shown on the shared side/status row — physical index UNVERIFIED",
    precedence: 46,
    competing: ["fx.momentary"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: ADDENDUM("§4 scrub latching"),
    confidence: "high",
    status: "partial",
    notes: "Chase animates identically forward and reverse and ignores the speed level.",
    firstDivergence: "logical LED state correct but DOM rendering wrong",
    observe: (s) => `latched=${s.scrubLatched} dir=${s.globalScrub}`,
  },
  {
    id: "scrub.speeds",
    group: "global scrub",
    name: "Four persistent scrub speeds",
    initiatingState: "shuttling",
    sequence: "Volume −/+ during scrub — never master volume",
    timing: "persists between shuttles",
    expectedOwner: "global scrub",
    expectedCommand: "scrub.speed",
    expectedEngineResult: "rate 1.25 / 1.6 / 2.5 / 4.0×",
    expectedLeds: expectedPhysicalFrame({
      "track-led-1": led("solid"),
      "track-led-2": led("dim"),
      "track-led-3": led("dim"),
      "track-led-4": led("dim"),
    }),
    expectedLedSummary: "Track LEDs 1-4 indicate the four levels",
    precedence: 40,
    competing: ["function.volume"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: ADDENDUM("§3 · GLOBAL_SCRUB_SPEEDS [1.25,1.6,2.5,4]"),
    confidence: "high",
    status: "partial",
    notes: "Speed index persists, but no LED shows which of the four levels is selected.",
    firstDivergence: "expected contract missing",
    observe: (s) => `level=${s.scrubSpeed + 1}`,
  },
  {
    id: "scrub.inertia",
    group: "global scrub",
    name: "Forward / reverse inertia handoff",
    initiatingState: "shuttle release",
    sequence: "release a shuttle at speed",
    timing: "≤ 2 audio frames of seam discontinuity",
    expectedOwner: "inertia",
    expectedCommand: "scrub.release (inertia curve)",
    expectedEngineResult: "forward decays toward +1.0×; reverse passes through zero into +1.0×",
    expectedLeds: expectedPhysicalFrame({}),
    expectedLedSummary: "no LED defined",
    precedence: 44,
    competing: [],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: ADDENDUM("§6 · src/audio/inertia.ts"),
    confidence: "medium",
    status: "unverified",
    notes: "Audible behaviour cannot be verified from reducer state alone.",
    firstDivergence: "state not available",
  },

  // -------------------------------------------------------------------- fx ---
  {
    id: "fx.scope",
    group: "fx",
    name: "STEM vs GLOBAL FX scope",
    initiatingState: "any",
    sequence: "bare Volume− + Volume+ opens STEM; FUNCTION held first then the chord opens GLOBAL; bare chord closes",
    timing: "chord is atomic — no master volume, no division, no two stem selections",
    expectedOwner: "FX overlay",
    expectedCommand: "fx.scope",
    expectedEngineResult: "per-stem or post-sum insert",
    expectedLeds: expectedPhysicalFrame({}),
    expectedLedSummary: "no physical LED distinguishes STEM from GLOBAL scope",
    precedence: 72,
    competing: ["function.volume"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: STEMTAPE("src/machine/stemPerformance.ts FxScope"),
    confidence: "high",
    status: "partial",
    firstDivergence: "expected contract missing",
    observe: (s) => (s.perf.fxOverlay ? `overlay open · ${s.perf.fxScope}` : "overlay closed"),
  },
  {
    id: "fx.momentary",
    group: "fx",
    name: "Momentary FX",
    initiatingState: "FX overlay open",
    sequence: "Track n held",
    timing: "while held",
    expectedOwner: "FX overlay",
    expectedCommand: "fx.momentary",
    expectedEngineResult: "algorithm engaged for the hold",
    expectedLeds: expectedPhysicalFrame({ "side-led-1": led("breathe", { periodMs: 2400 }) }),
    expectedLedSummary: "side LED for the bank breathes while held",
    precedence: 66,
    competing: ["track.mute", "play.track.solo"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: STEMTAPE("src/audio/fx/banks.ts"),
    confidence: "high",
    status: "implemented",
  },
  {
    id: "fx.latch",
    group: "fx",
    name: "FX latch / unlatch",
    initiatingState: "FX overlay open",
    sequence: "FUNCTION + Track n",
    timing: "tap",
    expectedOwner: "FX overlay",
    expectedCommand: "fx.latch",
    expectedEngineResult: "algorithm stays engaged",
    expectedLeds: expectedPhysicalFrame({ "side-led-1": led("solid") }),
    expectedLedSummary: "side LED solid while latched",
    precedence: 67,
    competing: ["function.volume"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: STEMTAPE("src/machine/fx12.ts"),
    confidence: "high",
    status: "implemented",
  },
  {
    id: "fx.loop.precedence",
    group: "fx",
    name: "FX precedence while loops or scrub are active",
    initiatingState: "momentary or latched loop / scrub, FX overlay open",
    sequence: "Track press",
    timing: "immediate",
    expectedOwner: "FX overlay (loop and scrub must survive)",
    expectedCommand: "fx.momentary",
    expectedEngineResult: "FX engages; loop and scrub state unchanged",
    expectedLeds: expectedPhysicalFrame({
      ...tracks(led("pulse", { periodMs: 500, phaseAnchor: "loop-wrap" })),
      "side-led-1": led("breathe", { periodMs: 2400 }),
    }),
    expectedLedSummary: "FX bank LED breathes; loop indication must not be lost",
    precedence: 74,
    competing: ["loop.momentary", "loop.latch", "scrub.latch"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: STEMTAPE("src/machine/chordArbiter.ts precedence 4-6"),
    confidence: "high",
    status: "conflicting",
    notes: "The FX overlay branch runs before every loop/heads branch, so loop LED state is hidden.",
    firstDivergence: "priority arbitration wrong",
  },
  {
    id: "fx.flash",
    group: "fx",
    name: "All-four-Track LED confirmation flash",
    initiatingState: "FX overlay open",
    sequence: "any FX latch toggle",
    timing: "single 220 ms one-shot",
    expectedOwner: "FX overlay",
    expectedCommand: "(LED only) fxFlashAt = now",
    expectedEngineResult: "no audio change",
    expectedLeds: expectedPhysicalFrame(tracks(led("one-shot single flash", { durationMs: 220 }))),
    expectedLedSummary: "all four Track LEDs flash once for 220 ms",
    precedence: 83,
    competing: ["loop.momentary"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: ADDENDUM("§5 · deriveLeds FX_LATCH_FLASH_MS"),
    confidence: "high",
    status: "conflicting",
    notes: "Rendered as an INFINITE blink and never re-derived, so the flash does not expire.",
    firstDivergence: "timing/clock missing",
    observe: (s) => (s.fxFlashAt == null ? "no flash armed" : `armed at ${s.fxFlashAt.toFixed(0)}`),
  },
  {
    id: "fx.flash.restore",
    group: "fx",
    name: "Normal post-flash LED restoration",
    initiatingState: "220 ms after an FX latch toggle",
    sequence: "elapse",
    timing: "220 ms",
    expectedOwner: "base",
    expectedCommand: "(LED only)",
    expectedEngineResult: "no audio change",
    expectedLeds: expectedPhysicalFrame(tracks(led("pulse", { periodMs: 1200, phaseAnchor: "loop-wrap" }))),
    expectedLedSummary: "Track LEDs return to their pre-flash state",
    precedence: 82,
    competing: ["fx.flash"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: ADDENDUM("§5"),
    confidence: "high",
    status: "missing",
    notes: "No ticker re-derives the frame, so restoration waits for an unrelated dispatch.",
    firstDivergence: "timing/clock missing",
  },
  {
    id: "fx.clearLatches",
    group: "fx",
    name: "Clear all FX latches",
    initiatingState: "FX overlay open",
    sequence: "FUNCTION + Volume− + Volume+",
    timing: "atomic chord",
    expectedOwner: "FX overlay",
    expectedCommand: "fx.clearLatches",
    expectedEngineResult: "all inserts bypassed",
    expectedLeds: expectedPhysicalFrame(tracks(led("one-shot single flash", { durationMs: 220 }))),
    expectedLedSummary: "confirmation flash, then all side LEDs dark",
    precedence: 84,
    competing: ["function.volume"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: STEMTAPE("src/machine/surface.ts fx.clearLatches"),
    confidence: "high",
    status: "partial",
    notes: "Command exists; it arms no confirmation flash.",
    firstDivergence: "expected contract missing",
  },
  {
    id: "fx.bank",
    group: "fx",
    name: "Bank and algorithm selection indication",
    initiatingState: "FX overlay open",
    sequence: "FUNCTION + Volume ± cycles the target / bank",
    timing: "duration-independent",
    expectedOwner: "FX overlay",
    expectedCommand: "fx.cycle",
    expectedEngineResult: "selected algorithm changes",
    expectedLeds: expectedPhysicalFrame({ "side-led-1": led("blink", { periodMs: 400 }) }),
    expectedLedSummary: "side LED indicates the selected bank",
    precedence: 68,
    competing: ["function.volume"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: STEMTAPE("src/machine/fx12.ts"),
    confidence: "medium",
    status: "partial",
    observe: (s) => `bank=${s.bank + 1}`,
  },

  // ----------------------------------------------------------------- heads ---
  {
    id: "heads.mode",
    group: "heads",
    name: "Heads mode (retained)",
    initiatingState: "Vocal stem decoded",
    sequence: "FUNCTION held + PLAY",
    timing: "hold",
    expectedOwner: "heads",
    expectedCommand: "heads.enter / heads.exit",
    expectedEngineResult: "energy-normalised four-head sum on the heads bus",
    expectedLeds: expectedPhysicalFrame({
      ...tracks(led("chase", { periodMs: 550, direction: "forward" })),
    }),
    expectedLedSummary: "Track LEDs chase per head; rejection = flash. No physical FUNCTION LED exists to mark the mode.",
    precedence: 76,
    competing: ["loop.momentary", "fx.momentary"],
    provenance: "STEM_TAPE_OVERRIDE",
    citation: STEMTAPE("src/audio/heads.ts · Heads addendum"),
    confidence: "high",
    status: "implemented",
    observe: (s) => `headsMode=${s.headsMode} source=${s.headsSource ?? "none"}`,
  },

  // --------------------------------------------------------------- m0 only ---
  {
    id: "m0.boot",
    group: "m0-only",
    name: "M0 boot signature",
    initiatingState: "power / USB attach",
    sequence: "boot",
    timing: "two flashes, then dark",
    expectedOwner: "firmware",
    expectedCommand: "none",
    expectedEngineResult: "n/a",
    expectedLeds: expectedPhysicalFrame(tracks(led("one-shot double flash", { durationMs: 400 }))),
    expectedLedSummary: "four Track LEDs flash twice, then idle dark",
    precedence: 0,
    competing: [],
    provenance: "M0_DIAGNOSTIC_ONLY",
    citation: M0("boot_signature()"),
    confidence: "high",
    status: "implemented",
    notes: "Diagnostic-only. Must not be read as stock or Stem Tape transport behaviour.",
  },
  {
    id: "m0.function.hold",
    group: "m0-only",
    name: "M0 long-FUNCTION shutdown ramp",
    initiatingState: "running",
    sequence: "hold FUNCTION",
    timing: "progressive",
    expectedOwner: "firmware",
    expectedCommand: "none",
    expectedEngineResult: "power-off transition",
    expectedLeds: expectedPhysicalFrame({
      "side-led-1": led("solid"),
      "side-led-2": led("solid"),
      "side-led-3": led("solid"),
      "side-led-4": led("solid"),
      ...tracks(led("one-shot single flash", { durationMs: 200 })),
    }),
    expectedLedSummary: "side LEDs light one by one; at the end all four Track LEDs light once",
    precedence: 0,
    competing: [],
    provenance: "M0_DIAGNOSTIC_ONLY",
    citation: M0("power hold path"),
    confidence: "high",
    status: "implemented",
  },
  {
    id: "m0.dfu",
    group: "m0-only",
    name: "M0 Track 1 + Track 4 early DFU escape",
    initiatingState: "boot",
    sequence: "hold Track 1 + Track 4 for 1200 ms at boot",
    timing: "1200 ms, before boot_signature()",
    expectedOwner: "firmware",
    expectedCommand: "none",
    expectedEngineResult: "reboot into bootloader",
    expectedLeds: expectedPhysicalFrame(tracks(led("solid"))),
    expectedLedSummary: "all four Track LEDs light, then the device reboots to DFU",
    precedence: 0,
    competing: [],
    provenance: "M0_DIAGNOSTIC_ONLY",
    citation: M0("early_dfu_escape()"),
    confidence: "high",
    status: "implemented",
  },
  {
    id: "m0.led.coverage",
    group: "m0-only",
    name: "M0 physical LED coverage",
    initiatingState: "n/a",
    sequence: "n/a",
    timing: "n/a",
    expectedOwner: "firmware",
    expectedCommand: "none",
    expectedEngineResult: "n/a",
    expectedLeds: expectedPhysicalFrame({}),
    expectedLedSummary:
      "all eight physical outputs (4 Track + 4 side/status) are electrically driven by the audited M0 driver; Stem Tape behaviour mapping onto them is partial",
    precedence: 0,
    competing: [],
    provenance: "M0_DIAGNOSTIC_ONLY",
    citation: M0("led driver — 8 of 8 physical outputs"),
    confidence: "high",
    status: "partial",
    notes:
      "Electrical coverage 8/8. Host→device LED feedback is not implemented by the audited build; a later versioned capability handshake may advertise it.",
  },
  {
    id: "stock.reference",
    group: "base surface",
    name: "Stock SP-1 reference behaviour",
    initiatingState: "n/a",
    sequence: "n/a",
    timing: "n/a",
    expectedOwner: "n/a",
    expectedCommand: "n/a",
    expectedEngineResult: "n/a",
    expectedLeds: expectedPhysicalFrame({}),
    expectedLedSummary: "n/a",
    precedence: 0,
    competing: [],
    provenance: "UNVERIFIED",
    citation: NONE,
    confidence: "none",
    status: "unverified",
    notes:
      "No public Teenage Engineering × Kanye West SP-1 firmware document is available. Any behaviour absent from the pinned Tape Looper source is unverified, regardless of what this website renders.",
  },
];

export interface ContractResult extends ContractEntry {
  observed: string | null;
  /** Implementation status from SOURCE AUDIT. Never a test result. */
  implementationStatus: ImplStatus;
  /** Reproduction status. `not-run` until a segment actually executed. */
  reproductionStatus: ReproductionStatus;
  observationSource: ObservationSource;
  segmentId: string | null;
  /** Only present when a reproduction really ran and really diverged. */
  reproductionFirstDivergence: string | null;
  reproductionDetail: string | null;
}

/** Contract self-validation: every entry must carry an exact 8-LED frame. */
export function validateContract(entries: ContractEntry[] = BEHAVIOR_CONTRACT): string[] {
  const problems: string[] = [];
  for (const e of entries) {
    for (const p of validatePhysicalFrame(e.expectedLeds as unknown as Record<string, unknown>)) {
      problems.push(`${e.id}: ${p}`);
    }
  }
  return problems;
}

/**
 * Reference data + (optionally) the result of a reproduction that ACTUALLY
 * ran. A static audit finding is never reported as a failed live test: with no
 * segment for an entry, `reproductionStatus` stays `not-run`, the observation
 * source is `source-audit`, and no first-divergence stage is claimed.
 */
export function evaluateContract(
  state: SurfaceState | null,
  reproductions?: (contractId: string) => ReproductionResult | null,
): ContractResult[] {
  return BEHAVIOR_CONTRACT.map((entry) => {
    const rep = reproductions?.(entry.id) ?? null;
    return {
      ...entry,
      observed: state && entry.observe ? entry.observe(state) : null,
      implementationStatus: entry.status,
      reproductionStatus: rep?.status ?? "not-run",
      observationSource: rep?.observationSource ?? "source-audit",
      segmentId: rep?.segmentId ?? null,
      reproductionFirstDivergence: rep?.status === "failed" ? rep.firstMissingStage : null,
      reproductionDetail: rep?.detail ?? null,
    };
  });
}
