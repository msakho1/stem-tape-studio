/**
 * Phase 5C — stem performance state.
 *
 * This is the stem half of the instrument: which stem is active, which stems
 * are soloed / linked, whether the FX overlay is open, and the four FX slots
 * per stem. It is serializable and holds NO audio objects; the AudioEngine is
 * still the only authority for what is audible.
 *
 * Persistence rule (correction 8): link, solo, variation and latch persist per
 * song. `momentary` and `fxOverlay` NEVER persist — a saved project must not
 * reload with a finger held down or an overlay open.
 */

import {
  BANKS,
  FX12_SCHEMA_VERSION,
  clearBankLatches,
  cycleAlgorithm,
  deserializeStemFx,
  initialStemFx,
  isBankActive,
  migrateLegacyStemFx,
  nudgeMacro,
  serializeStemFx,
  type AlgorithmIndex,
  type BankIndex,
  type StemFxState,
} from "./fx12";

export const FX_FAMILIES = ["filter", "echo", "reverb"] as const;
export type FxFamily = (typeof FX_FAMILIES)[number];

/**
 * Legacy bridge only. Selection now lives in `fx12` (four banks × three
 * algorithms); these four families remain because the audio rack's existing
 * Phase 5C processors are still the implementations behind algorithm 0 of each
 * bank. There is exactly ONE selection system — this map is derived from it.
 */
export const FX_FAMILY_BY_TRACK: readonly FxFamily[] = FX_FAMILIES;


export type StemIndex = 0 | 1 | 2 | 3;

export interface FxSlotState {
  /** Held-finger activation. Never persisted. */
  momentary: boolean;
  /** Latched activation (FX track + FUNCTION). Persisted. */
  latched: boolean;
  /** 1..4 — the selected preset within the family. Persisted. */
  variation: 1 | 2 | 3 | 4;
  /** Set when the engine refused the activation (e.g. no AudioWorklet). */
  rejected: string | null;
  /** Retained for schema compatibility; always false. */
  arming: boolean;
}

export interface StemTrackState {
  soloed: boolean;
  linked: boolean;
  /**
   * Compatibility echo of the values found in a stored project. The runtime
   * `soloed` / `linked` above are normalised on load (solo off, unlinked)
   * because PLAY + Track is retired and nothing can clear them; these two keep
   * the saved file byte-compatible when it is written back.
   */
  storedSoloed?: boolean;
  storedLinked?: boolean;
  /** Derived bridge to the existing rack processors. Never the source of truth. */
  fx: Record<FxFamily, FxSlotState>;
  /** Authoritative twelve-FX state: four banks × three algorithms. */
  fx12: StemFxState;
}

/** FX target: one lane, or the single post-sum rack on the whole mix. */
export type FxTarget = StemIndex | "global";
export type FxScope = "stem" | "global";

export interface StemPerformanceState {
  activeStem: StemIndex;
  fxOverlay: boolean;
  /**
   * Scope of the open overlay. Runtime only — never persisted, because a
   * reload always comes up with the overlay closed.
   */
  fxScope: FxScope;
  /** The one post-sum rack, driven when `fxScope === "global"`. */
  globalFx: StemFxState;
  tracks: [StemTrackState, StemTrackState, StemTrackState, StemTrackState];
  /** Diagnostics only: the last rejected activation reason. */
  lastRejection: string | null;
}

/** Saved-project schema version. 3 = Phase 5C, 4 = twelve-FX banks. */
export const STEM_TAPE_SCHEMA_VERSION = FX12_SCHEMA_VERSION;

export function initialFxSlot(): FxSlotState {
  return { momentary: false, latched: false, variation: 1, rejected: null, arming: false };
}

export function initialStemTrack(): StemTrackState {
  return {
    soloed: false,
    linked: true,
    fx: {
      filter: initialFxSlot(),
      echo: initialFxSlot(),
      reverb: initialFxSlot(),
    },
    fx12: initialStemFx(),
  };
}

/**
 * Project the authoritative bank state onto the legacy family slots the audio
 * rack still consumes. Bank i runs family `FX_FAMILIES[i]` only while algorithm
 * 0 (the legacy processor) is selected; algorithms 1 and 2 are the new
 * processors and never activate the legacy family.
 */
/**
 * Bank 2 (MOD) has NO legacy processor: Reel Flange, Formant Shift and
 * Rhythmic Gate are all bank-native, so bank 2 maps to no family.
 */
export const BANK_FAMILY: readonly (FxFamily | null)[] = ["filter", null, "echo", "reverb"];

export function syncLegacySlots(t: StemTrackState): StemTrackState {
  const fx = { ...t.fx };
  BANKS.forEach((_def, i) => {
    const bank = t.fx12.banks[i as BankIndex]!;
    const family = BANK_FAMILY[i];
    if (!family) return;
    const legacySelected = bank.selectedAlgorithm === 0;
    const alg = bank.algorithms[bank.selectedAlgorithm]!;
    fx[family] = {
      ...fx[family],
      momentary: legacySelected && bank.momentary,
      latched: legacySelected && bank.latched,
      // Macro 0..1 drives the legacy 1..4 preset table with no second selector.
      variation: (Math.min(4, Math.max(1, Math.round(alg.macroAmount * 3) + 1)) as FxSlotState["variation"]),
      rejected: alg.rejected,
      arming: alg.arming,
    };
  });
  return { ...t, fx };
}


export function initialStemPerformance(): StemPerformanceState {
  return {
    activeStem: 0,
    fxOverlay: false,
    fxScope: "stem",
    globalFx: initialStemFx(),
    tracks: [initialStemTrack(), initialStemTrack(), initialStemTrack(), initialStemTrack()],
    lastRejection: null,
  };
}

export function isFxActive(slot: FxSlotState): boolean {
  return slot.momentary || slot.latched;
}

export function anySolo(s: StemPerformanceState): boolean {
  return s.tracks.some((t) => t.soloed);
}

/**
 * Audible-by-solo / input-open rule (correction 3). Solo NEVER mutates the
 * saved mute state: a soloed track is audible even if the user muted it, and
 * un-soloing restores exactly the mute the user set.
 */
export function inputOpen(s: StemPerformanceState, index: number, muted: boolean): boolean {
  const track = s.tracks[index];
  if (!track) return !muted;
  const audibleBySolo = anySolo(s) ? track.soloed : true;
  return audibleBySolo && (!muted || track.soloed);
}

/** Which stems a tape operation targets: the linked group, or just this stem. */
export function tapeTarget(s: StemPerformanceState, stem: StemIndex): StemIndex[] {
  const active = s.tracks[stem];
  if (active?.linked) {
    return s.tracks.flatMap((t, i) => (t.linked ? [i as StemIndex] : []));
  }
  return [stem];
}

export type PerfPatch = (s: StemPerformanceState) => StemPerformanceState;

function patchTrack(
  s: StemPerformanceState,
  index: number,
  fn: (t: StemTrackState) => StemTrackState,
): StemPerformanceState {
  const tracks = [...s.tracks] as StemPerformanceState["tracks"];
  const t = tracks[index];
  if (!t) return s;
  tracks[index] = fn(t);
  return { ...s, tracks };
}

export function patchSlot(
  s: StemPerformanceState,
  index: number,
  family: FxFamily,
  fn: (slot: FxSlotState) => FxSlotState,
): StemPerformanceState {
  return patchTrack(s, index, (t) => ({ ...t, fx: { ...t.fx, [family]: fn(t.fx[family]) } }));
}

export function selectStem(s: StemPerformanceState, dir: 1 | -1): StemPerformanceState {
  const next = (((s.activeStem + dir) % 4) + 4) % 4;
  return { ...s, activeStem: next as StemIndex };
}

export function toggleSolo(s: StemPerformanceState, index: number): StemPerformanceState {
  return patchTrack(s, index, (t) => ({ ...t, soloed: !t.soloed }));
}

export function toggleLink(s: StemPerformanceState, index: number): StemPerformanceState {
  return patchTrack(s, index, (t) => ({ ...t, linked: !t.linked }));
}

// ------------------------------------------------------------ twelve-FX ops

/**
 * Every twelve-FX op below routes on its target: a lane index patches that
 * lane's rack, "global" patches the single post-sum rack. The bank/algorithm/
 * macro/latch semantics are IDENTICAL for both — one code path, two racks.
 */
function patchFx(
  s: StemPerformanceState,
  target: FxTarget,
  fn: (fx: StemFxState) => StemFxState,
): StemPerformanceState {
  if (target === "global") return { ...s, globalFx: fn(s.globalFx) };
  return patchTrack(s, target, (t) => syncLegacySlots({ ...t, fx12: fn(t.fx12) }));
}

/** Read the rack a target owns (overlay rendering + engine dispatch). */
export function fxStateOf(s: StemPerformanceState, target: FxTarget): StemFxState {
  return target === "global" ? s.globalFx : (s.tracks[target]?.fx12 ?? s.globalFx);
}

/** The target the open overlay is driving right now. */
export function fxTargetOf(s: StemPerformanceState): FxTarget {
  return s.fxScope === "global" ? "global" : s.activeStem;
}

function patchBank(
  s: StemPerformanceState,
  index: FxTarget,
  bank: BankIndex,
  fn: (b: StemFxState["banks"][number]) => StemFxState["banks"][number],
): StemPerformanceState {
  return patchFx(s, index, (fx) => {
    const banks = [...fx.banks] as StemFxState["banks"];
    banks[bank] = fn(banks[bank]!);
    return { ...fx, banks };
  });
}

export function selectBank(s: StemPerformanceState, index: FxTarget, bank: BankIndex): StemPerformanceState {
  return patchFx(s, index, (fx) => ({ ...fx, selectedBank: bank }));
}

export function setBankMomentary(
  s: StemPerformanceState,
  index: FxTarget,
  bank: BankIndex,
  on: boolean,
): StemPerformanceState {
  return patchBank(s, index, bank, (b) => ({ ...b, momentary: on }));
}

export function toggleBankLatch(s: StemPerformanceState, index: FxTarget, bank: BankIndex): StemPerformanceState {
  return patchBank(s, index, bank, (b) => ({ ...b, latched: !b.latched }));
}

export function cycleBankAlgorithm(
  s: StemPerformanceState,
  index: FxTarget,
  bank: BankIndex,
  dir: 1 | -1,
): StemPerformanceState {
  return patchBank(s, index, bank, (b) => cycleAlgorithm(b, dir));
}

export function nudgeBankMacro(
  s: StemPerformanceState,
  index: FxTarget,
  bank: BankIndex,
  dir: 1 | -1,
): StemPerformanceState {
  return patchBank(s, index, bank, (b) => nudgeMacro(bank, b, dir));
}

export function rejectBankAlgorithm(
  s: StemPerformanceState,
  index: FxTarget,
  bank: BankIndex,
  algorithm: AlgorithmIndex,
  reason: string,
): StemPerformanceState {
  const next = patchBank(s, index, bank, (b) => {
    const algorithms = [...b.algorithms] as StemFxState["banks"][number]["algorithms"];
    algorithms[algorithm] = { ...algorithms[algorithm]!, rejected: reason, arming: false };
    // Only this algorithm is refused; the bank stays usable and can cycle away.
    return { ...b, algorithms, momentary: false, latched: false };
  });
  return { ...next, lastRejection: reason };
}

export function activeBankCount(t: StemTrackState): number {
  return t.fx12.banks.filter(isBankActive).length;
}

export function clearLatches(s: StemPerformanceState, index: FxTarget): StemPerformanceState {
  return patchFx(s, index, (fx) => clearBankLatches(fx));
}

/**
 * Strip the never-persisted fields before saving.
 *
 * Compatibility: the ORIGINAL stored `soloed` / `linked` values are echoed back
 * verbatim from `storedSoloed` / `storedLinked` when they exist, so normalising
 * the runtime state (below) never rewrites an older project's saved values.
 */
export function serializePerformance(s: StemPerformanceState) {
  return {
    version: STEM_TAPE_SCHEMA_VERSION,
    activeStem: s.activeStem,
    tracks: s.tracks.map((t) => ({
      soloed: t.storedSoloed ?? t.soloed,
      linked: t.storedLinked ?? t.linked,
      fx12: serializeStemFx(t.fx12),
    })),
  };
}

type SerializedPerformance = ReturnType<typeof serializePerformance>;

/**
 * Versioned migration.
 *  - v4+  : read `fx12` directly.
 *  - v3   : Phase 5C families → bank algorithm 0, latches preserved, retired
 *           `variation` folded into that algorithm's macro amount.
 *  - older: defaults (no solos, no latches, Filter / Tempo Echo / Reverb /
 *           Beat Repeat selected).
 *
 * SOLO / LINK NORMALISER. PLAY + Track is retired, so nothing on the surface
 * can clear a persistent solo or re-link a lane. Loading a project that stored
 * either would strand the user in an unreachable state. The stored values are
 * therefore preserved for compatibility (`storedSoloed` / `storedLinked`, which
 * `serializePerformance` writes back unchanged) while the RUNTIME state is
 * forced to: persistent solo OFF, every lane UNLINKED.
 */
export function deserializePerformance(raw: unknown): StemPerformanceState {
  const base = normalizeSoloLink(initialStemPerformance());
  if (!raw || typeof raw !== "object") return base;
  const data = raw as Partial<SerializedPerformance> & {
    tracks?: { soloed?: boolean; linked?: boolean; fx?: unknown; fx12?: unknown }[];
  };
  const version = typeof data.version === "number" ? data.version : 0;
  if (version < 3) return base;
  const tracks = base.tracks.map((t, i) => {
    const saved = data.tracks?.[i];
    if (!saved) return t;
    const fx12 =
      version >= FX12_SCHEMA_VERSION
        ? deserializeStemFx(saved.fx12)
        : migrateLegacyStemFx(saved.fx as Record<string, { latched?: boolean; variation?: number }> | undefined);
    return syncLegacySlots({
      ...t,
      // Stored values kept verbatim for round-trip compatibility …
      storedSoloed: Boolean(saved.soloed),
      storedLinked: saved.linked !== false,
      // … runtime state normalised: no unreachable solo, no unreachable link.
      soloed: false,
      linked: false,
      fx12,
    });
  }) as StemPerformanceState["tracks"];
  return { ...base, activeStem: (data.activeStem ?? 0) as StemIndex, tracks };
}

/** Force every lane to solo-off / unlinked without touching the stored echo. */
export function normalizeSoloLink(s: StemPerformanceState): StemPerformanceState {
  return {
    ...s,
    tracks: s.tracks.map((t) => ({ ...t, soloed: false, linked: false })) as StemPerformanceState["tracks"],
  };
}

