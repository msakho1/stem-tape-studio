/**
 * Phase 5B worklet migration budget (binding correction #3).
 *
 * Sequential migration does NOT guarantee that the node-graph AudioBuffer of a
 * migrated track is collected before the next track's PCM is transferred: JS
 * garbage collection is not deterministic, and a detached ArrayBuffer's backing
 * store may still be reachable from a pending message or a live source node.
 *
 * The gate therefore uses the CONSERVATIVE WORST CASE — the whole project's PCM
 * duplicated — not merely twice the current track. A 199 MiB project must be
 * checked against ~398 MiB before migration is offered at all.
 */

import { allowed, describeVerdict, formatMiB, judge, type MemoryBudget } from "./memory";

export interface MigrationEstimate {
  projectBytes: number;
  /** Conservative worst-case peak: every track's PCM duplicated at once. */
  worstCasePeakBytes: number;
  /** Optimistic figure, reported for transparency only — never gated on. */
  optimisticPeakBytes: number;
  allowed: boolean;
  statement: string;
}

export function estimateMigration(
  trackBytes: number[],
  budget: MemoryBudget,
  highMemoryMode = false,
): MigrationEstimate {
  const projectBytes = trackBytes.reduce((a, b) => a + b, 0);
  const largest = trackBytes.reduce((a, b) => Math.max(a, b), 0);
  const worstCasePeakBytes = projectBytes * 2;
  const optimisticPeakBytes = projectBytes + largest;
  const verdict = judge(worstCasePeakBytes, budget, highMemoryMode);
  const ok = allowed(verdict);
  return {
    projectBytes,
    worstCasePeakBytes,
    optimisticPeakBytes,
    allowed: ok,
    statement: ok
      ? `Worklet migration gated at the conservative worst case ${formatMiB(worstCasePeakBytes)} (project ${formatMiB(
          projectBytes,
        )} duplicated). Allowed. Optimistic peak if collection keeps up: ${formatMiB(optimisticPeakBytes)}.`
      : `Worklet migration refused: worst-case peak ${formatMiB(worstCasePeakBytes)} — the whole ${formatMiB(
          projectBytes,
        )} project duplicated — is not affordable. ${describeVerdict(worstCasePeakBytes, budget, highMemoryMode)}`,
  };
}

/**
 * Handoff contract (binding correction #4).
 *
 * "First successful render" proves only that the processor runs. It does not
 * prove phase alignment or a click-free switch. A track is handed over only
 * when ALL of these hold:
 */
export interface HandoffContract {
  /** Shared transport frame, identical for every track in the switch. */
  switchFrame: number;
  /** Crossfade length across the switch, seconds (equal-power, A↔B). */
  fadeS: number;
  /** Worklet reported the same integrated media position at switchFrame. */
  phaseToleranceSamples: number;
}

export const HANDOFF: HandoffContract = {
  switchFrame: 0,
  fadeS: 0.012,
  phaseToleranceSamples: 2,
};

export function handoffAccepted(
  nodePositionSamples: number,
  workletPositionSamples: number,
  renderedOk: boolean,
  contract: HandoffContract = HANDOFF,
): { ok: boolean; detail: string } {
  if (!renderedOk) return { ok: false, detail: "worklet produced no render — staying on the node graph" };
  const drift = Math.abs(nodePositionSamples - workletPositionSamples);
  if (drift > contract.phaseToleranceSamples) {
    return {
      ok: false,
      detail: `phase drift ${drift} samples exceeds ±${contract.phaseToleranceSamples} at the shared switch frame — handoff refused`,
    };
  }
  return { ok: true, detail: `phase-aligned within ${drift} samples; crossfading over ${contract.fadeS * 1000} ms` };
}
