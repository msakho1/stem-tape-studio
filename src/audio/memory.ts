/**
 * Decoded-memory budget (Phase 4.1).
 *
 * UNIT CONTRACT: every number in this module is BYTES, and every rendered
 * figure is MiB (1024²). The label is "MiB", never "MB" — the divisor has
 * always been 1024², so the old label was simply wrong.
 *
 * This module is about RAM (decoded PCM held by AudioBuffers). It is NOT about
 * navigator.storage.estimate(), which measures on-device storage quota and is
 * reported separately.
 */

export const MiB = 1024 * 1024;

export interface MemoryBudget {
  /** Above this: allowed, warned. */
  warnBytes: number;
  /** Above this: requires explicit High Memory Mode. */
  standardBlockBytes: number;
  /** Above this: full decode is refused outright. */
  highMemoryBlockBytes: number;
  platform: "ios" | "android" | "desktop" | "ssr";
}

/** Server-render default: neutral, and never used for a real decision. */
export const SSR_BUDGET: MemoryBudget = {
  warnBytes: 192 * MiB,
  standardBlockBytes: 384 * MiB,
  highMemoryBlockBytes: 512 * MiB,
  platform: "ssr",
};

export function defaultBudget(): MemoryBudget {
  if (typeof navigator === "undefined") return SSR_BUDGET;
  const ua = navigator.userAgent;
  const iOS = /iPad|iPhone|iPod/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1);
  const android = /Android/.test(ua);
  if (iOS) return { warnBytes: 192 * MiB, standardBlockBytes: 384 * MiB, highMemoryBlockBytes: 512 * MiB, platform: "ios" };
  if (android)
    return { warnBytes: 192 * MiB, standardBlockBytes: 384 * MiB, highMemoryBlockBytes: 512 * MiB, platform: "android" };
  return { warnBytes: 384 * MiB, standardBlockBytes: 768 * MiB, highMemoryBlockBytes: 1024 * MiB, platform: "desktop" };
}

export type BudgetVerdict = "ok" | "warn" | "needs-high-memory" | "block";

export function judge(totalBytes: number, budget: MemoryBudget, highMemoryMode = false): BudgetVerdict {
  if (totalBytes > budget.highMemoryBlockBytes) return "block";
  if (totalBytes > budget.standardBlockBytes) return highMemoryMode ? "warn" : "needs-high-memory";
  if (totalBytes > budget.warnBytes) return "warn";
  return "ok";
}

export function allowed(verdict: BudgetVerdict): boolean {
  return verdict === "ok" || verdict === "warn";
}

export function formatMiB(bytes: number): string {
  return `${(bytes / MiB).toFixed(bytes < MiB ? 2 : 0)} MiB`;
}

/**
 * The sentence shown to the user. It must be arithmetically true: a total is
 * only ever described against the threshold it actually crossed.
 */
export function describeVerdict(totalBytes: number, budget: MemoryBudget, highMemoryMode = false): string {
  const v = judge(totalBytes, budget, highMemoryMode);
  const total = formatMiB(totalBytes);
  const warn = formatMiB(budget.warnBytes);
  const std = formatMiB(budget.standardBlockBytes);
  const high = formatMiB(budget.highMemoryBlockBytes);
  switch (v) {
    case "ok":
      return `Project total: ${total}. This is below the ${warn} warning threshold. Loading is allowed.`;
    case "warn":
      return totalBytes > budget.standardBlockBytes
        ? `Project total: ${total}. This exceeds the ${std} standard limit and is allowed only because High Memory Mode is on (ceiling ${high}).`
        : `Project total: ${total}. This exceeds the ${warn} warning threshold but remains below the ${std} standard limit. Loading is allowed.`;
    case "needs-high-memory":
      return `Project total: ${total}. This exceeds the ${std} standard limit. Enable High Memory Mode to attempt loading up to ${high}.`;
    case "block":
      return `Project total: ${total}. This exceeds the ${high} High Memory ceiling. Full decoding is refused — use Memory Saver (mono working copies) or fewer tracks.`;
  }
}

/** Transient overhead during one decode: the encoded bytes still resident. */
export function transientBytes(encodedBytes: number): number {
  return encodedBytes;
}

/** Reverse copies (Phase 5) double every decoded buffer that needs one. */
export function reverseCostBytes(decodedTotal: number): number {
  return decodedTotal;
}

/** Peak-cache for waveform drawing: ~2 floats per pixel column per track. */
export function waveformBytes(tracks: number, columns = 2048): number {
  return tracks * columns * 2 * 4;
}
