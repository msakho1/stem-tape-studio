/**
 * Outcome wording — the ONLY place that decides whether a sentence is allowed
 * to claim something about physical hardware.
 *
 * Rule: device-specific success language ("the device index now points at this
 * song", "a write has occurred on this device", "song verified on SP-1") is
 * reserved for mode === "physical", i.e. a real Web Serial port followed by a
 * successful device read-back. A mock/simulated run must always say so.
 */

import type { UploadOutcome, UploadResult } from "./transport";

export type TransportKind = "mock" | "physical";

/** Stage banner shown next to the stage list. */
export function writeStateWording(kind: TransportKind, anyWriteOccurred: boolean): string {
  if (!anyWriteOccurred) return kind === "mock" ? "no data has been written (simulated device)" : "no data has been written";
  return kind === "mock"
    ? "a simulated write occurred; no hardware was written"
    : "a write has occurred on this device";
}

/** The one-line verdict in stage 5. */
export function outcomeWording(kind: TransportKind, result: Pick<UploadResult, "outcome" | "detail">): string {
  const outcome: UploadOutcome = result.outcome;
  const detail = result.detail.replace(/\.$/, "");
  if (outcome === "committed") {
    return kind === "physical"
      ? "Committed. The device index now points at this song."
      : "Simulated commit completed. Nothing was written to hardware.";
  }
  if (outcome === "failed") {
    return kind === "physical"
      ? `Not committed — ${detail}. The slot still holds whatever it held before, so retrying is safe.`
      : `Simulated run stopped — ${detail}. No hardware was involved.`;
  }
  return kind === "physical"
    ? `Outcome unknown — ${detail}. Reconnect the SP-1 and resolve it below before assuming anything.`
    : `Simulated run left an unknown outcome — ${detail}. No hardware was involved.`;
}

/** Activity-log line emitted when an upload finishes successfully. */
export function successLogWording(kind: TransportKind): string {
  return kind === "physical"
    ? "Committed index re-read from the SP-1 and matched. Physical playback is still unconfirmed."
    : "Mock protocol verification passed. No physical SP-1 was written.";
}

/** Label for the first verification row. */
export function simulatedRowWording(kind: TransportKind): string {
  return kind === "mock"
    ? "mock protocol verification (simulated device, no hardware)"
    : "simulated verification (not applicable to a physical device)";
}

/** Wording after a reconnect-and-resolve pass. */
export function resolveWording(kind: TransportKind, outcome: UploadOutcome): string {
  if (kind === "mock") {
    return outcome === "committed"
      ? "Simulated reconnect: the simulated index matches this song. No hardware was read."
      : outcome === "failed"
        ? "Simulated reconnect: the song was NOT committed in the simulation. No hardware was read."
        : "Simulated reconnect: still unresolved in the simulation. No hardware was read.";
  }
  return outcome === "committed"
    ? "Reconnect check: the committed index matches this song. It is stored on the device."
    : outcome === "failed"
      ? "Reconnect check: the song was NOT committed. The previous song is still active."
      : "Reconnect check: still unresolved. The index could not be read.";
}

/** Phrases that may never appear for a simulated run. */
export const DEVICE_ONLY_PHRASES = [
  "The device index now points at this song.",
  "a write has occurred on this device",
  "Song verified on SP-1",
  "re-read from the SP-1",
] as const;
