/**
 * Device-management model for the SP-1 screen.
 *
 * The shape here is deliberately PLURAL — `songs: Song[]`, a per-song `id`,
 * a capability record — while today's firmware is deliberately SINGULAR: it
 * holds exactly one song and can neither delete nor reorder. The UI branches
 * on `DeviceInfo.capabilities`, never on the format version, so a firmware
 * that gains multi-song support turns those controls on without a rewrite.
 *
 * This module constructs no protocol bytes. It only maps what the transport
 * already read (capability reply + the two STIX index records) into the state
 * the screen renders.
 *
 * Vocabulary rule: the words "slot", "generation" and "rollback" belong to the
 * diagnostics section only. Nothing in the primary strings below uses them.
 */

import type { StemTapeCapabilities } from "@/sp1/compatibility";
import type { LibraryState } from "@/sp1/activeIndex";
import { FORMAT_MAJOR, FORMAT_MINOR, PHYSICAL_BLOCK_BYTES as BLOCK_BYTES } from "@/sp1/stemTapeFormat";

/* ------------------------------------------------------------------ types */

export interface DeviceCapabilities {
  /** More than one song may be resident at a time. */
  multiSong: boolean;
  /** A resident song can be removed without replacing it. */
  deleteSong: boolean;
  /** Songs have a user-controlled order. */
  reorderSongs: boolean;
}

export interface DeviceInfo {
  firmwareId: number;
  protocol: { major: number; minor: number };
  format: { major: number; minor: number };
  sampleRate: number;
  capabilities: DeviceCapabilities;
}

export type DeviceConnection =
  | { status: "disconnected" }
  | { status: "connecting" }
  | { status: "incompatible"; deviceFormat: string; appFormat: string }
  | { status: "ready"; device: DeviceInfo };

export interface Song {
  id: string;
  title: string;
  artist: string;
  durationSeconds: number;
  sizeBytes: number;
  isActive: boolean;
}

export interface Storage {
  /** Total space songs may occupy. One song region — never the sum of both. */
  capacityBytes: number;
  usedBytes: number;
  freeBytes: number;
  /**
   * The largest single song that will actually fit. Deliberately NOT the same
   * field as freeBytes: on this firmware a new song replaces the resident one,
   * so the whole region is available to it even while a song is stored.
   */
  maxSongBytes: number;
}

export type UploadFailure =
  | { kind: "version-mismatch"; deviceFormat: string; appFormat: string }
  | { kind: "song-too-large"; songBytes: number; maxSongBytes: number }
  | { kind: "connection-lost"; sectorsSent: number; sectorsTotal: number }
  | { kind: "verification-failed"; detail: string }
  | { kind: "device-busy"; detail: string }
  | { kind: "outcome-unknown"; detail: string }
  | { kind: "other"; detail: string };

export type UploadState =
  | { phase: "idle" }
  | { phase: "preparing" }
  | { phase: "sending"; sectorsSent: number; sectorsTotal: number }
  | { phase: "verifying" }
  | { phase: "committing" }
  | { phase: "done" }
  | { phase: "failed"; reason: UploadFailure; existingSongIntact: boolean };

export interface DeviceState {
  connection: DeviceConnection;
  /** 0 or 1 entries on this firmware; many on a multi-song firmware. */
  songs: Song[];
  activeSongId: string | null;
  storage: Storage;
  upload: UploadState;
}

/* ------------------------------------------------------------- derivation */

export const EMPTY_STORAGE: Storage = {
  capacityBytes: 0,
  usedBytes: 0,
  freeBytes: 0,
  maxSongBytes: 0,
};

export const APP_FORMAT = `${FORMAT_MAJOR}.${FORMAT_MINOR}`;

export function formatVersion(major: number, minor: number): string {
  return `${major}.${minor}`;
}

/**
 * What THIS firmware supports. Everything multi-song is gated behind a format
 * minor the firmware does not have yet; when it ships, only this function
 * changes.
 */
export function capabilitiesFor(format: { major: number; minor: number }): DeviceCapabilities {
  const multiSong = format.major > 1 || (format.major === 1 && format.minor >= 3);
  return { multiSong, deleteSong: multiSong, reorderSongs: multiSong };
}

export function deviceInfoFrom(caps: StemTapeCapabilities): DeviceInfo {
  const format = { major: caps.formatMajor, minor: caps.formatMinor };
  return {
    firmwareId: caps.firmwareId,
    protocol: { major: caps.protoMajor, minor: caps.protoMinor },
    format,
    sampleRate: caps.sampleRate,
    capabilities: capabilitiesFor(format),
  };
}

/**
 * The resident song, if any. The second internal copy the device keeps so a
 * failed upload can fall back is NOT a song: it is never playable and never
 * chosen by the user, so it must never enter this array.
 */
export function songsFrom(library: LibraryState | null): Song[] {
  const rec = library?.active;
  if (!rec || !rec.songPresent) return [];
  const sampleRate = rec.sampleRate || 48000;
  return [
    {
      id: `song-${rec.generation}`,
      title: rec.title || "Untitled song",
      artist: rec.artist || "",
      durationSeconds: rec.frames / sampleRate,
      sizeBytes: rec.songBlockCount * BLOCK_BYTES,
      isActive: true,
    },
  ];
}

/**
 * A song cannot span the two regions, so the capacity is ONE region — summing
 * them would promise roughly twice what actually fits.
 */
export function storageFrom(caps: StemTapeCapabilities | null, songs: Song[]): Storage {
  if (!caps) return EMPTY_STORAGE;
  const capacityBytes = caps.song[0].blocks * BLOCK_BYTES;
  const usedBytes = songs.reduce((n, s) => n + s.sizeBytes, 0);
  return {
    capacityBytes,
    usedBytes,
    freeBytes: Math.max(0, capacityBytes - usedBytes),
    // A new song replaces the resident one, so it gets the whole region.
    maxSongBytes: capacityBytes,
  };
}

/* ----------------------------------------------------------------- upload */

/** Map the transport's fine-grained stages onto the user-visible phases. */
export function uploadStateFor(input: {
  stage: string;
  sectorsSent: number;
  sectorsTotal: number;
}): UploadState {
  switch (input.stage) {
    case "preparing":
    case "capacity":
      return { phase: "preparing" };
    case "writing":
      return {
        phase: "sending",
        sectorsSent: input.sectorsSent,
        sectorsTotal: input.sectorsTotal,
      };
    case "verifying":
    case "checksums":
      return { phase: "verifying" };
    case "metadata":
    case "committing":
    case "confirming":
      return { phase: "committing" };
    case "complete":
      return { phase: "done" };
    default:
      return { phase: "preparing" };
  }
}

export const PHASE_LABEL: Record<string, string> = {
  preparing: "Preparing the song",
  sending: "Sending to the SP-1",
  verifying: "Checking what arrived",
  committing: "Switching over",
  done: "Done",
};

export const PHASE_DETAIL: Record<string, string> = {
  preparing: "Encoding the four stems on this computer. Nothing has been sent yet.",
  sending: "Each chunk is written, read back off the SP-1 and checked before it counts.",
  verifying: "The SP-1 is re-reading everything it stored and comparing it with what was sent.",
  committing: "The moment the new song becomes the one that plays.",
  done: "The new song is on the SP-1.",
};

/** Ordered phases, for the phase stepper. */
export const UPLOAD_PHASES = ["preparing", "sending", "verifying", "committing", "done"] as const;

export function phaseIndex(phase: string): number {
  const i = (UPLOAD_PHASES as readonly string[]).indexOf(phase);
  return i < 0 ? -1 : i;
}

const fmtBytes = (n: number) => `${(n / 1048576).toFixed(1)} MiB`;
const fmtCount = (n: number) => n.toLocaleString("en-US");

export interface FailureCopy {
  /** Leads with the reassurance when the existing song survived. */
  headline: string;
  body: string;
  retryable: boolean;
}

export function failureCopy(reason: UploadFailure, existingSongIntact: boolean): FailureCopy {
  const intact = existingSongIntact
    ? "Upload failed — your existing song is untouched."
    : "Upload failed.";
  switch (reason.kind) {
    case "version-mismatch":
      return {
        headline: "This app and your SP-1 expect different song formats.",
        body: `Your SP-1 stores songs in format ${reason.deviceFormat}; this app writes format ${reason.appFormat}. ${
          reason.deviceFormat < reason.appFormat
            ? "Update the firmware on your SP-1 to continue."
            : "This app is older than your SP-1 — reload it to pick up the newer version."
        } Nothing was written and the song already on the SP-1 is untouched.`,
        retryable: false,
      };
    case "song-too-large":
      return {
        headline: intact,
        body: `This song is ${fmtBytes(reason.songBytes)} and the most your SP-1 can hold is ${fmtBytes(
          reason.maxSongBytes,
        )}. Nothing was written. Use a shorter song and try again.`,
        retryable: false,
      };
    case "connection-lost":
      return {
        headline: intact,
        body: `The connection dropped after ${fmtCount(reason.sectorsSent)} of ${fmtCount(
          reason.sectorsTotal,
        )} chunks. Nothing on the SP-1 was changed. Try again when you're ready.`,
        retryable: true,
      };
    case "verification-failed":
      return {
        headline: intact,
        body: `The data that arrived on the SP-1 didn't match what was sent, so the upload was rejected before anything changed. ${reason.detail}`,
        retryable: true,
      };
    case "device-busy":
      return {
        headline: intact,
        body: `The SP-1 is playing and can't take a song right now. Stop playback on the SP-1, then try again. ${reason.detail}`,
        retryable: true,
      };
    case "outcome-unknown":
      return {
        headline: "The SP-1 stopped answering at the last moment.",
        body: `Reconnect the SP-1 to see which song it is playing. Either the previous song is still there or the new one finished — the SP-1 is never left in between. ${reason.detail}`,
        retryable: true,
      };
    default:
      return { headline: intact, body: reason.detail, retryable: true };
  }
}

/** Classify a transport result/error into a user-facing failure. */
export function classifyFailure(input: {
  outcome: string;
  detail: string;
  sectorsSent: number;
  sectorsTotal: number;
}): UploadFailure {
  const d = input.detail.toLowerCase();
  if (input.outcome === "unknown") return { kind: "outcome-unknown", detail: input.detail };
  if (d.includes("capacity") || d.includes("too large") || d.includes("does not fit"))
    return { kind: "other", detail: input.detail };
  if (d.includes("checksum") || d.includes("crc") || d.includes("verif"))
    return { kind: "verification-failed", detail: input.detail };
  if (d.includes("busy") || d.includes("transfer mode") || d.includes("playing"))
    return { kind: "device-busy", detail: input.detail };
  if (
    d.includes("disconnect") ||
    d.includes("closed") ||
    d.includes("timed out") ||
    d.includes("timeout") ||
    d.includes("no response") ||
    d.includes("port")
  )
    return {
      kind: "connection-lost",
      sectorsSent: input.sectorsSent,
      sectorsTotal: input.sectorsTotal,
    };
  return { kind: "other", detail: input.detail };
}

/** Every pre-commit failure leaves the resident song playable. */
export function existingSongIntact(outcome: string): boolean {
  return outcome !== "unknown" && outcome !== "corrupt";
}
