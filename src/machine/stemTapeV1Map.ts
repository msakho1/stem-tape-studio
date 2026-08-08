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
  /** Rollback strategy when the row loses to a longer chord. */
  rollback: "none" | "txn-snapshot";
  led: string;
  provenance: Provenance;
  family?: FxFamily;
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

export const STEM_TAPE_V1_MAP: StemTapeRow[] = [
  ...V26_ROWS_AS_REGISTRY,
  ...STEM_ROWS,
  ...SYSTEM_ROWS,
  ...FX_ROWS,
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
