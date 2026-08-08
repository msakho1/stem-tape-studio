/**
 * Decoded-memory budget.
 *
 * Phase 4 uses full decode, so decoded memory — not a blanket duration cap — is
 * the primary limit. Thresholds are tunable and the platform defaults are
 * finalised from /bench results, not from guesswork.
 */

export interface MemoryBudget {
  warnBytes: number;
  blockBytes: number;
  platform: string;
}

const MB = 1024 * 1024;

export function defaultBudget(): MemoryBudget {
  if (typeof navigator === "undefined") return { warnBytes: 180 * MB, blockBytes: 320 * MB, platform: "ssr" };
  const ua = navigator.userAgent;
  const iOS = /iPad|iPhone|iPod/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1);
  const android = /Android/.test(ua);
  if (iOS) return { warnBytes: 96 * MB, blockBytes: 192 * MB, platform: "ios" };
  if (android) return { warnBytes: 128 * MB, blockBytes: 256 * MB, platform: "android" };
  return { warnBytes: 180 * MB, blockBytes: 320 * MB, platform: "desktop" };
}

export type BudgetVerdict = "ok" | "warn" | "block";

export function judge(totalBytes: number, budget: MemoryBudget): BudgetVerdict {
  if (totalBytes > budget.blockBytes) return "block";
  if (totalBytes > budget.warnBytes) return "warn";
  return "ok";
}

/** Transient overhead: the original file bytes plus the decode scratch copy. */
export function transientBytes(fileBytes: number): number {
  return fileBytes * 2;
}

/** Reverse copies (Phase 5) double every decoded buffer that needs one. */
export function reverseCostBytes(decodedTotal: number): number {
  return decodedTotal;
}

/** Peak-cache for waveform drawing: ~2 floats per pixel column per track. */
export function waveformBytes(tracks: number, columns = 2048): number {
  return tracks * columns * 2 * 4;
}
