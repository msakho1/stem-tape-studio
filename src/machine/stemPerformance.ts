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

export const FX_FAMILIES = ["filter", "echo", "reverb", "beatRepeat"] as const;
export type FxFamily = (typeof FX_FAMILIES)[number];

/** Track button 1..4 map to the four FX families while the overlay is open. */
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
  /** Beat Repeat only: buffer is filling, not yet repeating. */
  arming: boolean;
}

export interface StemTrackState {
  soloed: boolean;
  linked: boolean;
  fx: Record<FxFamily, FxSlotState>;
}

export interface StemPerformanceState {
  activeStem: StemIndex;
  fxOverlay: boolean;
  tracks: [StemTrackState, StemTrackState, StemTrackState, StemTrackState];
  /** Diagnostics only: the last rejected activation reason. */
  lastRejection: string | null;
}

/** Saved-project schema version. Bumped by Phase 5C. */
export const STEM_TAPE_SCHEMA_VERSION = 3;

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
      beatRepeat: initialFxSlot(),
    },
  };
}

export function initialStemPerformance(): StemPerformanceState {
  return {
    activeStem: 0,
    fxOverlay: false,
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

export function setVariation(
  s: StemPerformanceState,
  index: number,
  family: FxFamily,
  dir: 1 | -1,
): StemPerformanceState {
  return patchSlot(s, index, family, (slot) => {
    const v = (((slot.variation - 1 + dir) % 4) + 4) % 4;
    return { ...slot, variation: (v + 1) as FxSlotState["variation"] };
  });
}

export function clearLatches(s: StemPerformanceState, index: number): StemPerformanceState {
  return patchTrack(s, index, (t) => ({
    ...t,
    fx: {
      filter: { ...t.fx.filter, latched: false },
      echo: { ...t.fx.echo, latched: false },
      reverb: { ...t.fx.reverb, latched: false },
      beatRepeat: { ...t.fx.beatRepeat, latched: false },
    },
  }));
}

/** Strip the never-persisted fields before saving. */
export function serializePerformance(s: StemPerformanceState) {
  return {
    version: STEM_TAPE_SCHEMA_VERSION,
    activeStem: s.activeStem,
    tracks: s.tracks.map((t) => ({
      soloed: t.soloed,
      linked: t.linked,
      fx: Object.fromEntries(
        FX_FAMILIES.map((f) => [f, { latched: t.fx[f].latched, variation: t.fx[f].variation }]),
      ),
    })),
  };
}

type SerializedPerformance = ReturnType<typeof serializePerformance>;

/**
 * Migration defaults for projects saved before Phase 5C (correction 8):
 * all stems linked, no solos, no latches, variation 1 in every family.
 */
export function deserializePerformance(raw: unknown): StemPerformanceState {
  const base = initialStemPerformance();
  if (!raw || typeof raw !== "object") return base;
  const data = raw as Partial<SerializedPerformance>;
  if (typeof data.version !== "number" || data.version < STEM_TAPE_SCHEMA_VERSION) return base;
  const tracks = base.tracks.map((t, i) => {
    const saved = data.tracks?.[i];
    if (!saved) return t;
    const fx = { ...t.fx };
    for (const f of FX_FAMILIES) {
      const sf = (saved.fx as Record<string, { latched?: boolean; variation?: number }> | undefined)?.[f];
      fx[f] = {
        ...initialFxSlot(),
        latched: Boolean(sf?.latched),
        variation: ((sf?.variation ?? 1) as FxSlotState["variation"]) || 1,
        // A restored Beat Repeat latch must re-arm and refill; ring-buffer
        // contents are never persisted.
        arming: f === "beatRepeat" && Boolean(sf?.latched),
      };
    }
    return { soloed: Boolean(saved.soloed), linked: saved.linked !== false, fx };
  }) as StemPerformanceState["tracks"];
  return { ...base, activeStem: (data.activeStem ?? 0) as StemIndex, tracks };
}
