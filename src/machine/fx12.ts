/**
 * Workstream 3 — the twelve-effect model: four banks of three algorithms.
 *
 * Musical signal order (binding correction) rather than physical button order:
 *
 *   source → TONE → RHYTHM → MOTION → SPACE → fader → solo → master
 *
 * so Beat Repeat / Gate / Pump feed Echo and Reverb, and Echo / Reverb /
 * Shimmer tails stay natural downstream of them.
 *
 * Macro amounts are PER ALGORITHM, never shared per bank: Filter, Isolator and
 * Dirt each remember their own amount. A rejected algorithm marks only itself,
 * so a device that cannot run Spectral Freeze can still cycle back to Reverb
 * or Shimmer inside the SPACE bank.
 */

import type { StemIndex } from "./stemPerformance";

export type BankIndex = 0 | 1 | 2 | 3;
export type AlgorithmIndex = 0 | 1 | 2;

export type BankId = "tone" | "rhythm" | "motion" | "space";

export type AlgorithmId =
  // TONE
  | "filter"
  | "isolator"
  | "dirt"
  // RHYTHM
  | "beatRepeat"
  | "gate"
  | "pump"
  // MOTION
  | "echo"
  | "pitchEcho"
  | "scatter"
  // SPACE
  | "reverb"
  | "shimmer"
  | "freeze";

export interface AlgorithmDef {
  id: AlgorithmId;
  label: string;
  /** Macro amount used when a project has never touched this algorithm. */
  defaultMacro: number;
  /** Lazily constructed heavy processor that may be rejected on weak devices. */
  heavy: boolean;
  /** Existing Phase 5C implementation reused verbatim. */
  legacy: boolean;
}

export interface BankDef {
  id: BankId;
  label: string;
  /** Physical track button (0..3) that selects this bank. */
  button: BankIndex;
  algorithms: [AlgorithmDef, AlgorithmDef, AlgorithmDef];
}

/**
 * Declared in SIGNAL ORDER. `button` keeps the physical mapping, so
 * BANKS[i].button is NOT i for RHYTHM/MOTION.
 */
export const BANKS: [BankDef, BankDef, BankDef, BankDef] = [
  {
    id: "tone",
    label: "TONE",
    button: 0,
    algorithms: [
      { id: "filter", label: "Filter", defaultMacro: 0.5, heavy: false, legacy: true },
      { id: "isolator", label: "Isolator", defaultMacro: 0.5, heavy: false, legacy: false },
      { id: "dirt", label: "Dirt / Crusher", defaultMacro: 0.35, heavy: false, legacy: false },
    ],
  },
  {
    id: "rhythm",
    label: "RHYTHM",
    button: 3,
    algorithms: [
      { id: "beatRepeat", label: "Beat Repeat", defaultMacro: 0.5, heavy: false, legacy: true },
      { id: "gate", label: "Rhythmic Gate", defaultMacro: 0.5, heavy: false, legacy: false },
      { id: "pump", label: "Pump", defaultMacro: 0.45, heavy: false, legacy: false },
    ],
  },
  {
    id: "motion",
    label: "MOTION",
    button: 1,
    algorithms: [
      { id: "echo", label: "Tempo Echo", defaultMacro: 0.5, heavy: false, legacy: true },
      { id: "pitchEcho", label: "Pitch Echo", defaultMacro: 0.5, heavy: false, legacy: false },
      { id: "scatter", label: "Granular Scatter", defaultMacro: 0.4, heavy: true, legacy: false },
    ],
  },
  {
    id: "space",
    label: "SPACE",
    button: 2,
    algorithms: [
      { id: "reverb", label: "Reverb", defaultMacro: 0.45, heavy: false, legacy: true },
      { id: "shimmer", label: "Shimmer", defaultMacro: 0.45, heavy: true, legacy: false },
      { id: "freeze", label: "Spectral Freeze", defaultMacro: 0.5, heavy: true, legacy: false },
    ],
  },
];

/** Signal-chain order of the banks (index into BANKS). */
export const SIGNAL_ORDER: BankIndex[] = [0, 1, 2, 3];

/** Track button 1..4 → bank index in BANKS. */
export const BANK_BY_BUTTON: BankIndex[] = (() => {
  const out: BankIndex[] = [0, 0, 0, 0];
  BANKS.forEach((b, i) => {
    out[b.button] = i as BankIndex;
  });
  return out;
})();

export function bankOfButton(button: number): BankIndex {
  return BANK_BY_BUTTON[button] ?? 0;
}

export function algorithmDef(bank: BankIndex, algorithm: AlgorithmIndex): AlgorithmDef {
  return BANKS[bank]!.algorithms[algorithm];
}

export interface FxAlgorithmState {
  macroAmount: number;
  /** Set only on THIS algorithm when the engine refuses it. */
  rejected: string | null;
  /** Ring-buffer / capture still filling (Beat Repeat, Scatter, Freeze). */
  arming: boolean;
}

export interface FxBankState {
  selectedAlgorithm: AlgorithmIndex;
  algorithms: [FxAlgorithmState, FxAlgorithmState, FxAlgorithmState];
  /** Held-finger activation. Never persisted. */
  momentary: boolean;
  /** FUNCTION + bank button. Persisted. */
  latched: boolean;
}

export interface StemFxState {
  banks: [FxBankState, FxBankState, FxBankState, FxBankState];
  /** Never persisted — the overlay always reopens with nothing pre-selected. */
  selectedBank: BankIndex | null;
}

/** Bumped by the twelve-FX model. v3 projects migrate forward. */
export const FX12_SCHEMA_VERSION = 4;

export function initialAlgorithm(bank: BankIndex, algorithm: AlgorithmIndex): FxAlgorithmState {
  return { macroAmount: algorithmDef(bank, algorithm).defaultMacro, rejected: null, arming: false };
}

export function initialBank(bank: BankIndex): FxBankState {
  return {
    selectedAlgorithm: 0,
    algorithms: [initialAlgorithm(bank, 0), initialAlgorithm(bank, 1), initialAlgorithm(bank, 2)],
    momentary: false,
    latched: false,
  };
}

export function initialStemFx(): StemFxState {
  return {
    banks: [initialBank(0), initialBank(1), initialBank(2), initialBank(3)],
    selectedBank: null,
  };
}

export function isBankActive(b: FxBankState): boolean {
  return b.momentary || b.latched;
}

/** The algorithm a bank would run right now, or null when it is rejected. */
export function runningAlgorithm(bank: BankIndex, b: FxBankState): AlgorithmIndex | null {
  const a = b.algorithms[b.selectedAlgorithm];
  return a && a.rejected == null ? b.selectedAlgorithm : null;
}

/**
 * `+` cycles 0→1→2→0, `−` cycles 0→2→1→0. Wraps indefinitely. Cycling NEVER
 * activates an inactive bank; it only changes what will run on the next press.
 */
export function cycleAlgorithm(b: FxBankState, dir: 1 | -1): FxBankState {
  const next = (((b.selectedAlgorithm + dir) % 3) + 3) % 3;
  return { ...b, selectedAlgorithm: next as AlgorithmIndex };
}

export const MACRO_STEP = 0.05;

export function nudgeMacro(bank: BankIndex, b: FxBankState, dir: 1 | -1): FxBankState {
  const algorithms = [...b.algorithms] as FxBankState["algorithms"];
  const idx = b.selectedAlgorithm;
  const cur = algorithms[idx]!;
  const value = Math.min(1, Math.max(0, cur.macroAmount + dir * MACRO_STEP));
  algorithms[idx] = { ...cur, macroAmount: value };
  void bank;
  return { ...b, algorithms };
}

export function rejectAlgorithm(b: FxBankState, algorithm: AlgorithmIndex, reason: string): FxBankState {
  const algorithms = [...b.algorithms] as FxBankState["algorithms"];
  algorithms[algorithm] = { ...algorithms[algorithm]!, rejected: reason, arming: false };
  return { ...b, algorithms };
}

export function clearBankLatches(s: StemFxState): StemFxState {
  return { ...s, banks: s.banks.map((b) => ({ ...b, latched: false })) as StemFxState["banks"] };
}

// ---------------------------------------------------------------- persistence

export interface SerializedStemFx {
  version: number;
  banks: { selectedAlgorithm: number; latched: boolean; macros: number[] }[];
}

export function serializeStemFx(s: StemFxState): SerializedStemFx {
  return {
    version: FX12_SCHEMA_VERSION,
    banks: s.banks.map((b) => ({
      selectedAlgorithm: b.selectedAlgorithm,
      latched: b.latched,
      macros: b.algorithms.map((a) => a.macroAmount),
    })),
  };
}

/** v3 family key → { bank, algorithm } in the twelve-FX model. */
export const LEGACY_FAMILY_TO_BANK: Record<string, { bank: BankIndex; algorithm: AlgorithmIndex }> = {
  filter: { bank: 0, algorithm: 0 },
  beatRepeat: { bank: 1, algorithm: 0 },
  echo: { bank: 2, algorithm: 0 },
  reverb: { bank: 3, algorithm: 0 },
};

/**
 * Old (v3) per-stem FX record → twelve-FX state.
 *
 * Filter / Tempo Echo / Reverb / Beat Repeat become the selected algorithm of
 * their bank, latches are preserved, and the retired 1..4 `variation` maps onto
 * that algorithm's macro amount (variation N → (N-1)/3) so nothing the user set
 * is silently discarded. Every new algorithm gets its safe default macro.
 */
export function migrateLegacyStemFx(
  legacy: Record<string, { latched?: boolean; variation?: number }> | undefined,
): StemFxState {
  const state = initialStemFx();
  if (!legacy) return state;
  for (const [family, mapping] of Object.entries(LEGACY_FAMILY_TO_BANK)) {
    const saved = legacy[family];
    if (!saved) continue;
    const bank = state.banks[mapping.bank]!;
    bank.selectedAlgorithm = mapping.algorithm;
    bank.latched = Boolean(saved.latched);
    const variation = Math.min(4, Math.max(1, Number(saved.variation ?? 1)));
    bank.algorithms[mapping.algorithm] = {
      ...bank.algorithms[mapping.algorithm]!,
      macroAmount: (variation - 1) / 3,
      arming: mapping.bank === 1 && Boolean(saved.latched),
    };
  }
  return state;
}

export function deserializeStemFx(raw: unknown): StemFxState {
  const base = initialStemFx();
  if (!raw || typeof raw !== "object") return base;
  const data = raw as Partial<SerializedStemFx>;
  if (typeof data.version !== "number" || data.version < FX12_SCHEMA_VERSION) return base;
  data.banks?.forEach((saved, i) => {
    const bank = base.banks[i as BankIndex];
    if (!bank || !saved) return;
    bank.selectedAlgorithm = (Math.min(2, Math.max(0, saved.selectedAlgorithm ?? 0)) as AlgorithmIndex) ?? 0;
    bank.latched = Boolean(saved.latched);
    saved.macros?.forEach((m, a) => {
      const alg = bank.algorithms[a as AlgorithmIndex];
      if (alg && Number.isFinite(m)) alg.macroAmount = Math.min(1, Math.max(0, m));
    });
    // Beat Repeat restored latched must re-arm; buffers are never persisted.
    if (i === 1 && bank.latched && bank.selectedAlgorithm === 0) bank.algorithms[0]!.arming = true;
  });
  return base;
}

export type StemFxByStem = Record<StemIndex, StemFxState>;
