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
  if (outcome === "corrupt") {
    return kind === "physical"
      ? "Both index records are unreadable. This is corrupt storage, not an ordinary interrupted upload."
      : "Simulated: both index records are unreadable in the simulation. No hardware was involved.";
  }
  return kind === "physical"
    ? `Transfer outcome unknown. Reconnect to check which verified song is active. (${detail}.)`
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
    ? RECONNECT_COMMITTED
    : outcome === "failed"
      ? RECONNECT_NOT_COMMITTED
      : outcome === "corrupt"
        ? "Both index records are unreadable. This is corrupt storage, not an ordinary interrupted upload."
        : UNKNOWN_BEFORE_RECONNECT;
}

/* ---------- interrupted-transfer messaging (A/B contract v1.1) ---------- */

export const UNKNOWN_BEFORE_RECONNECT =
  "Transfer outcome unknown. Reconnect to check which verified song is active.";
export const RECONNECT_NOT_COMMITTED =
  "The replacement was not committed. Your previous song remains active.";
export const RECONNECT_COMMITTED =
  "The replacement was committed and verified on the connected device.";

/** Simulated variants — a mock run may never borrow device language. */
export function interruptedWording(kind: TransportKind, phase: "pending" | "old" | "new" | "corrupt"): string {
  const simulated = kind === "mock";
  switch (phase) {
    case "pending":
      return simulated ? `Simulated: ${UNKNOWN_BEFORE_RECONNECT} No hardware was involved.` : UNKNOWN_BEFORE_RECONNECT;
    case "old":
      return simulated ? `Simulated: ${RECONNECT_NOT_COMMITTED} No hardware was involved.` : RECONNECT_NOT_COMMITTED;
    case "new":
      return simulated
        ? "Simulated: the replacement was committed in the simulation only. No hardware was involved."
        : RECONNECT_COMMITTED;
    default:
      return simulated
        ? "Simulated: both index records are unreadable in the simulation. No hardware was involved."
        : "Both index records are unreadable. This is corrupt storage, not an ordinary interrupted upload.";
  }
}

/** Phrases that must never be shown for an ordinary interrupted upload. */
export const FORBIDDEN_INTERRUPTION_PHRASES = [
  "library must be reinitialized",
  "library must be reinitialised",
  "reinitialize the library",
  "reinitialise the library",
] as const;

/** Phrases that may never appear for a simulated run. */
export const DEVICE_ONLY_PHRASES = [
  "The device index now points at this song.",
  "a write has occurred on this device",
  "Song verified on SP-1",
  "re-read from the SP-1",
] as const;
