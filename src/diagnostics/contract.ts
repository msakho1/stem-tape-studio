/**
 * Authoritative, VERSIONED SP-1 behaviour contract.
 *
 * Each entry states the required control sequence, the expected command, the
 * expected LED result, precedence, provenance and implementation status.
 * Provenance is honest: where no stock SP-1 document exists, the entry is
 * marked `unverified` rather than inventing stock behaviour.
 *
 * This module is DECLARATIVE ONLY. It never changes behaviour; the drawer
 * evaluates observed state against it.
 */

import type { SurfaceState } from "@/machine/surface";

export const BEHAVIOR_CONTRACT_VERSION = "sp1-behavior-contract/1.0.0";

export type Provenance =
  | "stock SP-1"
  | "Tape Looper baseline"
  | "Stem Tape override"
  | "Stem Tape addendum"
  | "undocumented";

export type ImplStatus = "implemented" | "partial" | "missing" | "conflicting" | "unverified";

export interface ContractEntry {
  id: string;
  name: string;
  sequence: string;
  expectedCommand: string;
  /** Expected result across the eight panel LEDs (tracks 1-4, side 1-4). */
  expectedLeds: string;
  precedence: number;
  provenance: Provenance;
  /** Source document / version / code reference. */
  reference: string;
  status: ImplStatus;
  notes?: string;
  /** Observed evaluation from live reducer state, when one is derivable. */
  observe?: (s: SurfaceState) => string;
}

export const BEHAVIOR_CONTRACT: ContractEntry[] = [
  {
    id: "track.mute",
    name: "Track mute",
    sequence: "Track n tap (no modifier, FX overlay closed)",
    expectedCommand: "stem.mute / stem.unmute",
    expectedLeds: "Track n → dim when muted, pulse when playing; side row unchanged",
    precedence: 20,
    provenance: "Tape Looper baseline",
    reference: "Tape Looper v2.6 mapping · src/machine/surface.ts",
    status: "implemented",
    observe: (s) => s.tracks.map((t, i) => `${i + 1}:${t.content}`).join(" "),
  },
  {
    id: "play.track.solo",
    name: "PLAY + Track solo / link (FX closed)",
    sequence: "PLAY held < 450 ms, then Track n; overlap < 700 ms = solo, ≥ = link/unlink",
    expectedCommand: "stem.solo / stem.link",
    expectedLeds: "Soloed Track solid, others dim",
    precedence: 74,
    provenance: "Stem Tape override",
    reference: "src/machine/chordArbiter.ts (soloLinkMs 700, globalLoopClaimMs 450)",
    status: "partial",
    notes: "Solo LED indication is rendered only while the FX overlay is open (deriveLeds row 5).",
    observe: (s) => s.perf.tracks.map((t, i) => `${i + 1}${t.soloed ? "S" : ""}${t.linked ? "L" : ""}`).join(" "),
  },
  {
    id: "stem.active",
    name: "Active-stem indication",
    sequence: "FUNCTION tap arms selection, then Track n",
    expectedCommand: "stem.select",
    expectedLeds: "Active Track breathe",
    precedence: 66,
    provenance: "Stem Tape override",
    reference: "src/machine/surface.ts trackSelectArmedAt",
    status: "partial",
    notes: "Breathe is emitted only inside the FX overlay; outside it the active stem has no LED.",
    observe: (s) => `activeStem=${s.perf.activeStem + 1}`,
  },
  {
    id: "function.volume",
    name: "Contextual FUNCTION + Volume ownership",
    sequence: "FUNCTION held + Volume ±",
    expectedCommand: "loop.division / fx.macro depending on context",
    expectedLeds: "Function LED 1 solid while held",
    precedence: 70,
    provenance: "Stem Tape addendum",
    reference: "stock-SP-1 behaviour addendum §1",
    status: "implemented",
    observe: (s) => `division=1/${s.globalLoop.division}`,
  },
  {
    id: "loop.momentary",
    name: "Momentary global loop",
    sequence: "PLAY held ≥ 450 ms",
    expectedCommand: "loop.global.enter → loop.global.exit on release",
    expectedLeds: "All four Track LEDs pulse in the loop division",
    precedence: 60,
    provenance: "Tape Looper baseline",
    reference: "src/machine/surface.ts globalLoop.active",
    status: "conflicting",
    notes: "globalLoop.active is never read by deriveLeds — no LED indicates a held loop.",
    observe: (s) => `active=${s.globalLoop.active} latched=${s.globalLoop.latched}`,
  },
  {
    id: "loop.latch",
    name: "FUNCTION loop latch",
    sequence: "During a held global loop, tap FUNCTION",
    expectedCommand: "loop.global.latch",
    expectedLeds: "Track LEDs stay pulsing after PLAY release",
    precedence: 61,
    provenance: "Stem Tape addendum",
    reference: "addendum §2",
    status: "conflicting",
    notes: "State latches correctly; no LED distinguishes latched from momentary.",
    observe: (s) => `latched=${s.globalLoop.latched}`,
  },
  {
    id: "loop.exit",
    name: "PLAY exits a latched loop",
    sequence: "PLAY tap while globalLoop.latched",
    expectedCommand: "loop.global.exit",
    expectedLeds: "Track LEDs return to per-track content state",
    precedence: 62,
    provenance: "Stem Tape addendum",
    reference: "addendum §2",
    status: "partial",
    observe: (s) => (s.globalLoop.latched ? "latched — PLAY will exit" : "no latched loop"),
  },
  {
    id: "scrub.speeds",
    name: "Four persistent scrub speeds",
    sequence: "FUNCTION + Volume ± while shuttling; setting survives release",
    expectedCommand: "scrub.speed",
    expectedLeds: "No dedicated LED defined by any source",
    precedence: 40,
    provenance: "Stem Tape addendum",
    reference: "addendum §3 · GLOBAL_SCRUB_SPEEDS [1.25,1.6,2.5,4]",
    status: "partial",
    notes: "Speed index is persistent, but no LED shows which of the four levels is selected.",
    observe: (s) => `level=${s.scrubSpeed + 1}`,
  },
  {
    id: "scrub.momentary",
    name: "FUNCTION + rocker momentary scrub",
    sequence: "FUNCTION held + rocker FWD or RWD",
    expectedCommand: "scrub.start / scrub.end",
    expectedLeds: "Function LED 2 blink while a rocker is down",
    precedence: 45,
    provenance: "Stem Tape addendum",
    reference: "addendum §3-4",
    status: "partial",
    notes: "Blink does not distinguish forward from reverse.",
    observe: (s) => `globalScrub=${s.globalScrub}`,
  },
  {
    id: "scrub.latch",
    name: "Scrub latch and bare-FUNCTION unlatch",
    sequence: "FUNCTION tap during a shuttle latches; bare FUNCTION tap unlatches",
    expectedCommand: "scrub.latch / scrub.unlatch",
    expectedLeds: "Function LED 2 chase while latched",
    precedence: 46,
    provenance: "Stem Tape addendum",
    reference: "addendum §4",
    status: "partial",
    notes: "Chase animates identically forward and reverse, and ignores the speed level.",
    observe: (s) => `latched=${s.scrubLatched} dir=${s.globalScrub}`,
  },
  {
    id: "scrub.inertia",
    name: "Forward / reverse inertia handoff",
    sequence: "Release a shuttle at speed; tape decelerates back to 1×",
    expectedCommand: "scrub.release (inertia curve)",
    expectedLeds: "No LED defined",
    precedence: 44,
    provenance: "Stem Tape addendum",
    reference: "addendum §6 · src/audio/inertia.ts",
    status: "unverified",
    notes: "Audible behaviour cannot be verified from reducer state alone.",
  },
  {
    id: "fx.scope",
    name: "STEM vs GLOBAL FX scope",
    sequence: "Volume− + Volume+ short chord opens the overlay; scope follows selection",
    expectedCommand: "fx.scope",
    expectedLeds: "No LED distinguishes scope",
    precedence: 72,
    provenance: "Stem Tape override",
    reference: "src/machine/stemPerformance.ts FxScope",
    status: "partial",
    observe: (s) => (s.perf.fxOverlay ? `overlay open · ${s.perf.fxScope}` : "overlay closed"),
  },
  {
    id: "fx.loop.precedence",
    name: "FX precedence while loops are active",
    sequence: "Track press with the FX overlay open during a momentary or latched loop",
    expectedCommand: "fx.momentary (loop must survive)",
    expectedLeds: "FX bank LED breathe; loop indication must not be lost",
    precedence: 74,
    provenance: "Stem Tape override",
    reference: "src/machine/chordArbiter.ts precedence 4-6",
    status: "conflicting",
    notes: "The FX overlay branch runs before every loop/heads branch, so loop LED state is hidden.",
  },
  {
    id: "fx.momentary",
    name: "Momentary FX",
    sequence: "FX overlay open + Track n held",
    expectedCommand: "fx.momentary",
    expectedLeds: "Side LED for the bank breathes while held",
    precedence: 66,
    provenance: "Stem Tape override",
    reference: "src/audio/fx/banks.ts",
    status: "implemented",
  },
  {
    id: "fx.latch",
    name: "FX latch / unlatch",
    sequence: "FUNCTION + Track n inside the FX overlay",
    expectedCommand: "fx.latch",
    expectedLeds: "Side LED solid while latched",
    precedence: 67,
    provenance: "Stem Tape override",
    reference: "src/machine/fx12.ts",
    status: "implemented",
  },
  {
    id: "fx.flash",
    name: "All-four-Track LED confirmation flash",
    sequence: "Any FX latch toggle",
    expectedCommand: "(LED only) fxFlashAt = now",
    expectedLeds: "All four Track LEDs flash once for 220 ms",
    precedence: 83,
    provenance: "Stem Tape addendum",
    reference: "addendum §5 · deriveLeds FX_LATCH_FLASH_MS",
    status: "conflicting",
    notes: "Rendered as an INFINITE blink and never re-derived, so the flash does not expire.",
    observe: (s) => (s.fxFlashAt == null ? "no flash armed" : `armed at ${s.fxFlashAt.toFixed(0)}`),
  },
  {
    id: "fx.flash.restore",
    name: "Normal post-flash LED restoration",
    sequence: "220 ms after an FX latch toggle",
    expectedCommand: "(LED only)",
    expectedLeds: "Track LEDs return to their pre-flash state",
    precedence: 82,
    provenance: "Stem Tape addendum",
    reference: "addendum §5",
    status: "missing",
    notes: "No ticker re-derives the frame, so restoration waits for an unrelated dispatch.",
  },
  {
    id: "fx.clearLatches",
    name: "Clear all FX latches",
    sequence: "FUNCTION + Volume− + Volume+ inside the FX overlay",
    expectedCommand: "fx.clearLatches",
    expectedLeds: "Confirmation flash then all side LEDs dark",
    precedence: 84,
    provenance: "Stem Tape override",
    reference: "src/machine/surface.ts fx.clearLatches",
    status: "partial",
    notes: "Command exists; it arms no confirmation flash.",
  },
  {
    id: "heads.mode",
    name: "Heads mode (retained)",
    sequence: "FUNCTION held + PLAY (Vocal stem decoded)",
    expectedCommand: "heads.enter / heads.exit",
    expectedLeds: "Track LEDs chase per head; Function LED 1 breathes; rejection = blink",
    precedence: 76,
    provenance: "Stem Tape override",
    reference: "src/audio/heads.ts · Heads addendum",
    status: "implemented",
    observe: (s) => `headsMode=${s.headsMode} source=${s.headsSource ?? "none"}`,
  },
  {
    id: "stock.reference",
    name: "Stock SP-1 reference behaviour",
    sequence: "n/a",
    expectedCommand: "n/a",
    expectedLeds: "n/a",
    precedence: 0,
    provenance: "undocumented",
    reference: "no public Teenage Engineering × Kanye West SP-1 firmware document is available",
    status: "unverified",
    notes: "Any stock behaviour not present in the Tape Looper baseline is explicitly unverified.",
  },
];

export interface ContractResult extends ContractEntry {
  observed: string | null;
}

export function evaluateContract(state: SurfaceState | null): ContractResult[] {
  return BEHAVIOR_CONTRACT.map((entry) => ({
    ...entry,
    observed: state && entry.observe ? entry.observe(state) : null,
  }));
}
