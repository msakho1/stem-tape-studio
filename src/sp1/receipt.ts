/**
 * Transfer receipt.
 *
 * The receipt records exactly what happened and, critically, keeps the three
 * verification claims separate. There is no aggregate "hardwareVerified" flag:
 *   simulatedVerification        — a mock device completed the protocol
 *   deviceReadbackVerification   — a real SP-1 returned matching committed bytes
 *   physicalPlaybackVerification — a human confirmed the song played back
 * The application never sets the third one on its own.
 */

import type { StemTapeCapabilities } from "./compatibility";
import type { CanonicalSong } from "./song";
import type { UploadResult } from "./transport";
import {
  BLOCKS_PER_SECTOR,
  CHANNELS,
  PCM_BIT_DEPTH,
  PHYSICAL_BLOCK_BYTES,
  SAMPLE_RATE,
  SECTOR_BYTES,
} from "./stemTapeFormat";

export interface TransferReceipt {
  schema: "stem-tape-transfer-receipt/1";
  generatedAt: string;
  mode: "mock" | "physical";
  outcome: UploadResult["outcome"];
  verification: UploadResult["verification"];
  song: {
    title: string;
    artist: string;
    bpm: number;
    downbeatSeconds: number;
    downbeatFrame: number;
    frames: number;
    durationSeconds: number;
    stems: { track: number; role: string; filename: string; padFrames: number; checksum: number }[];
    sha256: string;
    checksum: number;
    stemChecksums: number[];
  };
  format: {
    sampleRate: number;
    channels: number;
    bitDepth: number;
    bytesPerBlock: number;
    bytesPerSector: number;
    blocksPerSector: number;
  };
  transfer: {
    slot: number;
    sectorCount: number;
    blockCount: number;
    bytesWritten: number;
    retries: number;
    elapsedMs: number;
    indexSha256: string;
  };
  device: {
    firmwareId: string;
    protocolVersion: string;
    formatVersion: string;
    capabilityFlags: string;
    songSlots: number;
    libraryBase: number;
    sectorsPerSong: number;
    indexBlocks: number;
    generation: number;
  } | null;
}

export function buildReceipt(args: {
  song: CanonicalSong;
  result: UploadResult;
  caps: StemTapeCapabilities | null;
  slot: number;
  mode: "mock" | "physical";
  physicalPlaybackConfirmed?: boolean;
}): TransferReceipt {
  const { song, result, caps, slot, mode } = args;
  return {
    schema: "stem-tape-transfer-receipt/1",
    generatedAt: new Date().toISOString(),
    mode,
    outcome: result.outcome,
    verification: {
      // A mock run can never claim device or physical verification.
      simulatedVerification: mode === "mock" && result.outcome === "committed",
      deviceReadbackVerification: mode === "physical" && result.verification.deviceReadbackVerification,
      physicalPlaybackVerification:
        mode === "physical" &&
        result.verification.deviceReadbackVerification &&
        !!args.physicalPlaybackConfirmed,
    },
    song: {
      title: song.metadata.title,
      artist: song.metadata.artist,
      bpm: song.metadata.bpm,
      downbeatSeconds: song.metadata.downbeatSeconds,
      downbeatFrame: Math.round(song.metadata.downbeatSeconds * SAMPLE_RATE),
      frames: song.frames,
      durationSeconds: song.durationSeconds,
      stems: song.stems.map((s, i) => ({
        track: i + 1,
        role: s.name,
        filename: s.filename,
        padFrames: s.padFrames,
        checksum: s.checksum,
      })),
      sha256: result.songSha256,
      checksum: result.songChecksum,
      stemChecksums: result.stemChecksums,
    },
    format: {
      sampleRate: SAMPLE_RATE,
      channels: CHANNELS,
      bitDepth: PCM_BIT_DEPTH,
      bytesPerBlock: PHYSICAL_BLOCK_BYTES,
      bytesPerSector: SECTOR_BYTES,
      blocksPerSector: BLOCKS_PER_SECTOR,
    },
    transfer: {
      slot,
      sectorCount: result.sectorCount,
      blockCount: result.totalBlocks,
      bytesWritten: result.bytesWritten,
      retries: result.retries,
      elapsedMs: result.elapsedMs,
      indexSha256: result.indexSha256,
    },
    device: caps
      ? {
          firmwareId: `0x${(caps.firmwareId >>> 0).toString(16)}`,
          protocolVersion: `${caps.protoMajor}.${caps.protoMinor}`,
          formatVersion: `${caps.formatMajor}.${caps.formatMinor}`,
          capabilityFlags: `0x${(caps.flags >>> 0).toString(16)}`,
          songSlots: caps.songSlots,
          libraryBase: caps.libraryBase,
          sectorsPerSong: caps.sectorsPerSong,
          indexBlocks: caps.indexBlocks,
          generation: caps.generation,
        }
      : null,
  };
}
