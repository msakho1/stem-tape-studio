/**
 * Phase 5C — versioned Stem Tape v1 mapping registry.
 *
 * Declarative rows for the whole instrument. The 37 stock Tape Looper v2.6 rows
 * are RE-EXPORTED unchanged from `v26map.ts` and referenced here, never
 * rewritten, so the v2.6 regression suite keeps a single source of truth.
 */

import { V26_MAP, type V26Row } from "./v26map";
import type { FxFamily } from "./stemPerformance";

export const STEM_TAPE_MAP_VERSION = "stem-tape-v1.0.0";

export type MapLayer = "tape" | "stem" | "fx-overlay" | "system";
export type Provenance = "stock" | "v2.6" | "reinterpreted" | "extension";

export interface StemTapeRow {
  id: string;
  layer: MapLayer;
  /** Controls in the order they must be pressed. */
  controls: string[];
  /** Tap / hold discriminator in ms, when the row has one. */
  thresholdMs?: number;
  /** The single semantic command this row emits. */
  command: string;
  /** Base commands the arbiter suppresses BEFORE dispatch when this row wins. */
  suppresses: string[];
  /** Documented v2.6 row ids this Stem Tape binding reinterprets. */
  supersedes?: string[];
  /** The original v2.6 behaviour, preserved in documentation only. */
  originalBehaviour?: string;
  /** Rollback strategy when the row loses to a longer chord. */
  rollback: "none" | "txn-snapshot";
  led: string;
  provenance: Provenance;
  family?: FxFamily;
  /** Desktop keyboard bindings, "KeyF+KeyQ" for chords. Panel is derived from these. */
  keys?: string[];
  /** Phase 6 tutorial metadata — drives the Guide without duplicating the map. */
  tutorial?: {
    plainLanguage: string;
    /** Ordered controls the Guide highlights on the twin. */
    highlight: string[];
    expected: string;
  };
}

/** The stock v2.6 rows, unchanged, projected into the registry shape. */
export const V26_ROWS_AS_REGISTRY: StemTapeRow[] = V26_MAP.map((r: V26Row) => ({
  id: r.id,
  layer: "tape" as const,
  controls: [r.input],
  command: r.command,
  suppresses: [],
  rollback: r.id.includes("double") || r.id.includes("semitone") ? ("txn-snapshot" as const) : ("none" as const),
  led: "v2.6 stock behaviour",
  provenance: "v2.6" as const,
}));

export const STEM_ROWS: StemTapeRow[] = [
  {
    id: "stem.select.next",
    layer: "stem",
    controls: ["play", "volume-plus"],
    command: "stem.select dir=+1 (wrap Vocals→Drums→Bass→Instruments)",
    suppresses: ["transport.play", "master.gain"],
    rollback: "none",
    led: "active stem LED brightens",
    provenance: "extension",
    tutorial: {
      plainLanguage: "Hold PLAY and tap volume up to move to the next stem: Vocals, Drums, Bass, Instruments.",
      highlight: ["play", "volume-plus"],
      expected: "The newly selected stem's LED brightens.",
    },
  },
  {
    id: "stem.select.prev",
    layer: "stem",
    controls: ["play", "volume-minus"],
    command: "stem.select dir=−1",
    suppresses: ["transport.play", "master.gain"],
    rollback: "none",
    led: "active stem LED brightens",
    provenance: "extension",
    tutorial: {
      plainLanguage: "Hold PLAY and tap volume down to step back to the previous stem.",
      highlight: ["play", "volume-minus"],
      expected: "The newly selected stem's LED brightens.",
    },
  },
  {
    id: "stem.solo",
    layer: "stem",
    controls: ["play", "track-button-n"],
    thresholdMs: 700,
    command: "stem.solo (overlap < 700 ms, measured from the two-control overlap)",
    suppresses: ["transport.play", "track.mute", "track.unmute"],
    rollback: "none",
    led: "soloed stem solid, non-solo stems faint",
    provenance: "extension",
  },
  {
    id: "stem.link",
    layer: "stem",
    controls: ["play", "track-button-n"],
    thresholdMs: 700,
    command: "stem.link toggle (overlap ≥ 700 ms) — phase-continuous, never restarts",
    suppresses: ["transport.play", "track.mute", "track.unmute"],
    rollback: "none",
    led: "unlinked stem double-pulses",
    provenance: "extension",
  },
];

export const TRANSPORT_OVERRIDE_ROWS: StemTapeRow[] = [
  {
    id: "play.cue",
    layer: "tape",
    controls: ["play"],
    thresholdMs: 450,
    command: "transport.cue frame=0 (8 ms anti-click fade, sources stopped, EXACT launch armed)",
    suppresses: ["transport.restart"],
    supersedes: ["play.restart"],
    originalBehaviour: "v2.6: hold PLAY restarts the loop from the top immediately.",
    rollback: "none",
    led: "PLAY indicator slow-blinks while cued",
    provenance: "reinterpreted",
    tutorial: {
      plainLanguage: "Hold PLAY to park the tape at the very start, ready for a clean launch.",
      highlight: ["play"],
      expected: "Sound stops within ~8 ms and the next PLAY tap starts everything exactly on frame 0.",
    },
  },
  {
    id: "rocker.scrub",
    layer: "tape",
    controls: ["function", "rocker-fwd"],
    command: "transport.scrub.start direction=±1 → transport.scrub.end (held audible four-stem shuttle, one shared playhead)",
    suppresses: ["rocker.chop", "rate.set", "transport.scrub"],
    supersedes: ["rocker.chop"],
    originalBehaviour: "v2.6: FUNCTION + rocker halves/doubles the chop division. Chop now lives on PLAY + rocker.",
    rollback: "none",
    led: "rocker LED flashes in the scrub direction while held",
    provenance: "reinterpreted",
    keys: ["KeyF+KeyQ", "KeyF+KeyA"],
    tutorial: {
      plainLanguage: "Hold FUNCTION and a rocker to shuttle all four stems together — you hear the tape move.",
      highlight: ["function", "rocker-fwd"],
      expected: "Audible tape-style shuttle; all four stems stay locked to one playhead and playback resumes on release.",
    },
  },
  {
    id: "rocker.chop.play",
    layer: "tape",
    controls: ["play", "rocker-fwd"],
    command: "loop.chop half / double (PLAY held + rocker deflection; PLAY is claimed before dispatch)",
    suppresses: ["transport.play", "transport.stop", "transport.cue", "rate.set"],
    supersedes: ["rocker.chop"],
    originalBehaviour: "v2.6: FUNCTION + rocker halved/doubled the chop. Stem Tape moves chop to PLAY + rocker so FUNCTION + rocker can shuttle.",
    rollback: "none",
    led: "chop division shown on the four track LEDs while PLAY is held",
    provenance: "extension",
    keys: ["Space+KeyQ", "Space+KeyA"],
    tutorial: {
      plainLanguage: "Hold PLAY and flick a rocker to halve or double the chop; flick twice to reset, hold to glide.",
      highlight: ["play", "rocker-fwd"],
      expected: "The loop subdivides or lengthens and the transport never starts or stops.",
    },
  },
];


export const SYSTEM_ROWS: StemTapeRow[] = [
  {
    id: "fx.overlay.toggle",
    layer: "system",
    controls: ["volume-minus", "volume-plus"],
    thresholdMs: 600,
    command: "fx.overlay toggle (second press within 120 ms, both released < 600 ms)",
    suppresses: ["master.gain"],
    rollback: "none",
    led: "both FUNCTION LEDs alternate-pulse while open",
    provenance: "reinterpreted",
    tutorial: {
      plainLanguage: "Press both volume keys together to open or close the effects layer.",
      highlight: ["volume-minus", "volume-plus"],
      expected: "Both FUNCTION LEDs alternate-pulse while the effects layer is open.",
    },
  },
  {
    id: "system.pairing",
    layer: "system",
    controls: ["volume-minus", "volume-plus"],
    thresholdMs: 2000,
    command: "existing Bluetooth pairing gesture (≈2 s hold)",
    suppresses: ["master.gain", "fx.overlay"],
    rollback: "none",
    led: "stock pairing pattern",
    provenance: "stock",
  },
  {
    id: "system.volumechord.ambiguous",
    layer: "system",
    controls: ["volume-minus", "volume-plus"],
    command: "no-op (600–2000 ms band, diagnostics only)",
    suppresses: ["master.gain", "fx.overlay"],
    rollback: "none",
    led: "no change",
    provenance: "extension",
  },
];

const FAMILY_ROWS = (family: FxFamily, track: number, label: string): StemTapeRow[] => [
  {
    id: `fx.${family}.momentary`,
    layer: "fx-overlay",
    controls: [`track-button-${track}`],
    command: `fx.momentary.start/end ${family} on the active stem (${label})`,
    suppresses: ["track.mute", "track.unmute"],
    rollback: "none",
    led: `side LED ${track} breathing`,
    provenance: "extension",
    family,
    tutorial: {
      plainLanguage: `Hold key ${track} to apply ${label} to the active stem for as long as you hold it.`,
      highlight: [`track-button-${track}`],
      expected: `Side LED ${track} breathes while the effect sounds.`,
    },
  },
  {
    id: `fx.${family}.variation`,
    layer: "fx-overlay",
    controls: [`track-button-${track}`, "volume-minus|volume-plus"],
    command: `fx.variation ${family} ±1 (4 presets)`,
    suppresses: ["master.gain", "track.mute", "track.unmute"],
    rollback: "none",
    led: `side LEDs show the variation number, then time out`,
    provenance: "extension",
    family,
    tutorial: {
      plainLanguage: `Hold key ${track} and tap a volume key to step through the four ${label} variations.`,
      highlight: [`track-button-${track}`, "volume-plus"],
      expected: "The side LEDs briefly show the variation number.",
    },
  },
  {
    id: `fx.${family}.latch`,
    layer: "fx-overlay",
    controls: [`track-button-${track}`, "function"],
    command: `fx.latch ${family} toggle`,
    suppresses: ["track.mute", "track.unmute", "function.hold"],
    rollback: "none",
    led: `side LED ${track} solid while latched`,
    provenance: "extension",
    family,
    tutorial: {
      plainLanguage: `Hold FUNCTION and press key ${track} to leave ${label} latched on until you toggle it off.`,
      highlight: ["function", `track-button-${track}`],
      expected: `Side LED ${track} stays solid while the effect is latched.`,
    },
  },
];

export const FX_ROWS: StemTapeRow[] = [
  ...FAMILY_ROWS("filter", 1, "bipolar DJ filter"),
  ...FAMILY_ROWS("echo", 2, "tempo-synced echo"),
  ...FAMILY_ROWS("reverb", 3, "algorithmic reverb"),
  ...FAMILY_ROWS("beatRepeat", 4, "beat repeat / stutter"),
  {
    id: "fx.clearLatches",
    layer: "fx-overlay",
    controls: ["track-button-1", "track-button-2", "track-button-3", "track-button-4", "function"],
    command: "fx.clearLatches on the active stem",
    suppresses: ["track.mute", "track.unmute", "function.hold"],
    rollback: "none",
    led: "all four side LEDs blink once, then dark",
    provenance: "extension",
  },
];

/** Phase 6 — recording, grid, heads/PRINT and export rows (binding map §M5). */
export const RECORDING_ROWS: StemTapeRow[] = [
  {
    id: "rec.arm.hold",
    layer: "stem",
    controls: ["track-n"],
    thresholdMs: 450,
    command: "rec.arm track=n (empty → arm; content → overdub-arm)",
    suppresses: ["take.stop", "stem.mute.toggle"],
    rollback: "txn-snapshot",
    led: "armed track pulses ~2 Hz while waiting for sound",
    provenance: "extension",
    tutorial: {
      plainLanguage: "Hold a track button for about half a second to arm it. Nothing records until it hears sound.",
      highlight: ["track-n"],
      expected: "the track light pulses; play or speak and it starts recording on the first note",
    },
  },
  {
    id: "rec.tap.stop",
    layer: "stem",
    controls: ["track-n"],
    thresholdMs: 450,
    command: "rec.tap track=n (recording → stop at seam; armed → cancel; else stop take / mute toggle)",
    suppresses: [],
    rollback: "none",
    led: "solid while recording, returns to stem colour when stopped",
    provenance: "reinterpreted",
    tutorial: {
      plainLanguage: "A short tap on the same track button stops the recording at the next loop point.",
      highlight: ["track-n"],
      expected: "recording ends cleanly on the loop seam, no click",
    },
  },
  {
    id: "rec.undo.doubleTap",
    layer: "stem",
    controls: ["track-n", "track-n"],
    thresholdMs: 300,
    command: "rec.undoLastPass track=n (non-destructive; redo available)",
    suppresses: ["rec.tap"],
    rollback: "txn-snapshot",
    led: "undone pass dims in the take list",
    provenance: "extension",
    tutorial: {
      plainLanguage: "Double-tap the track button to remove the last overdub pass. Nothing is deleted — you can bring it back.",
      highlight: ["track-n"],
      expected: "the newest pass mutes and the redo action becomes available",
    },
  },
  {
    id: "rec.input.drawer",
    layer: "system",
    controls: ["function", "record"],
    command: "rec.openInputDrawer (permission, device, monitoring, latency)",
    suppresses: [],
    rollback: "none",
    led: "input LED lit while the drawer is open",
    provenance: "extension",
    tutorial: {
      plainLanguage: "Opens the input panel where you allow the microphone and choose monitoring.",
      highlight: ["function", "record"],
      expected: "the input drawer opens; monitoring stays off until you turn it on",
    },
  },
  {
    id: "grid.tap.learn",
    layer: "system",
    controls: ["function", "volume-plus"],
    command: "grid.tapTempo (median-filtered, ≥3 taps, 1500 ms inactivity resets)",
    suppresses: ["master.gain"],
    rollback: "none",
    led: "grid LED flashes on the downbeat once locked",
    provenance: "extension",
    tutorial: {
      plainLanguage: "Tap this combination in time with your music to teach Stem Tape the tempo.",
      highlight: ["function", "volume-plus"],
      expected: "after three taps the tempo readout locks and the grid light flashes on beat one",
    },
  },
  {
    id: "grid.quantise.toggle",
    layer: "system",
    controls: ["function", "volume-minus"],
    command: "grid.toggleQuantise (punch-in/out snaps to the grid)",
    suppresses: ["master.gain"],
    rollback: "none",
    led: "grid LED solid when quantise is on",
    provenance: "extension",
    tutorial: {
      plainLanguage: "Turns grid snapping on or off for punch-in recording.",
      highlight: ["function", "volume-minus"],
      expected: "recording starts and stops exactly on the beat instead of the instant you press",
    },
  },
  {
    id: "print.reserved",
    layer: "system",
    controls: ["function", "play"],
    thresholdMs: 900,
    command: "heads.print (RESERVED — records the heads sum to a new take)",
    suppresses: ["transport.play"],
    rollback: "none",
    led: "print LED reserved, unlit in this build",
    provenance: "extension",
    tutorial: {
      plainLanguage: "Reserved for PRINT: committing the tape heads to a new recorded layer.",
      highlight: ["function", "play"],
      expected: "reserved in this build — the gesture is registered but inactive",
    },
  },
  {
    id: "export.wav",
    layer: "system",
    controls: ["function", "record"],
    thresholdMs: 900,
    command: "export.wav (streamed 16/24-bit WAV, on-device)",
    suppresses: ["rec.openInputDrawer"],
    rollback: "none",
    led: "export LED animates during assembly",
    provenance: "extension",
    tutorial: {
      plainLanguage: "Holds to export what you have recorded as a WAV file saved from your own device.",
      highlight: ["function", "record"],
      expected: "a WAV download appears; the audio never leaves your device before saving",
    },
  },
];

export const STEM_TAPE_V1_MAP: StemTapeRow[] = [
  ...V26_ROWS_AS_REGISTRY,
  ...STEM_ROWS,
  ...TRANSPORT_OVERRIDE_ROWS,
  ...SYSTEM_ROWS,
  ...FX_ROWS,
  ...RECORDING_ROWS,
];

export const STEM_TAPE_ROW_BY_ID: Record<string, StemTapeRow> = Object.fromEntries(
  STEM_TAPE_V1_MAP.map((r) => [r.id, r]),
);

/** JSON export for the Mapping Lab. */
export function exportMapJson(): string {
  return JSON.stringify(
    { version: STEM_TAPE_MAP_VERSION, generatedAt: new Date().toISOString(), rows: STEM_TAPE_V1_MAP },
    null,
    2,
  );
}
