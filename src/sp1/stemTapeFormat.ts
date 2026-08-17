/**
 * Stem Tape storage format v1.1 — THE single source of numeric offsets.
 *
 * Layered on top of the unchanged Tape Looper transport (512-byte physical
 * blocks, SP1XFER! + P/R/W/F/X). Nothing in this file changes the inherited
 * command encoding, response parsing or framing; it only defines what the
 * companion places INSIDE those 512-byte blocks.
 *
 * v1.1 replaces the unsafe single-index layout of v1.0 with true A/B storage:
 *
 *   song slot A   |  song slot B      two independent song-data regions
 *   index slot A  |  index slot B     two independent STIX v2 index regions
 *
 * A replacement upload always writes the INACTIVE song slot and the INACTIVE
 * index slot; the active pair is never touched. At every interruption point
 * either the previous generation or the new generation is a complete, valid,
 * CRC-checked record, so the library can never require reinitialization
 * because of an interrupted upload.
 *
 * All multi-byte fields in this file are LITTLE-ENDIAN.
 *
 * Provenance note (honest): the repository documents the 8 KB page grouping and
 * the Tape Looper index (firmware/src/main.c `struct meta_blk`, mirrored in
 * firmware/web/index.html). The STCP capability structure, the STIX v2 index
 * and the A/B layout below are the companion's own contract and are NOT
 * implemented by any firmware in this repository.
 */

/* ---------- transport / sector geometry (unchanged from v1.0) ---------- */

export const PHYSICAL_BLOCK_BYTES = 512;
export const BLOCKS_PER_SECTOR = 16;
export const SECTOR_BYTES = PHYSICAL_BLOCK_BYTES * BLOCKS_PER_SECTOR; // 8192
/** Every A/B region start and length is expressed in whole 512-byte blocks. */
export const REQUIRED_ALIGNMENT = PHYSICAL_BLOCK_BYTES;

/** Audio format — fixed by the Stem Tape contract. */
export const STEM_COUNT = 4;
export const CHANNELS = 2;
export const PCM_BIT_DEPTH = 24;
export const BYTES_PER_SAMPLE = PCM_BIT_DEPTH / 8;
export const SAMPLE_RATE = 48000;
/** One frame carries all four stems, both channels: 4*2*3 = 24 bytes. */
export const BYTES_PER_FRAME = STEM_COUNT * CHANNELS * BYTES_PER_SAMPLE;

/** Sector header: reserved timing / tempo / LED bytes, then the frame payload. */
export const SECTOR_HEADER_BYTES = 32;
export const SECTOR_PAYLOAD_BYTES = SECTOR_BYTES - SECTOR_HEADER_BYTES; // 8160
export const FRAMES_PER_SECTOR = SECTOR_PAYLOAD_BYTES / BYTES_PER_FRAME; // 340

export const SECTOR_MAGIC = 0x53545343; // 'STSC'
export const SECTOR_OFF = {
  magic: 0,
  sectorIndex: 4,
  firstFrame: 8,
  frameCount: 12,
  bpmQ8: 16,
  downbeatFrame: 20,
  ledReserved: 24, // 4 reserved LED bytes owned by the firmware
  reserved: 28,
} as const;

export function sectorsForFrames(frames: number): number {
  return Math.ceil(frames / FRAMES_PER_SECTOR);
}

/* ---------- versions ---------- */

export const PROTOCOL_MAJOR = 1;
/** Minimum protocol minor that carries the A/B capability structure. */
export const PROTOCOL_MINOR = 1;
export const FORMAT_MAJOR = 1;
/** 1.1 = A/B song + index storage. 1.0 (single index) is refused. */
export const FORMAT_MINOR = 1;
/** STIX index record version. v1 (the unsafe array index) is refused. */
export const STIX_VERSION = 2;

/* ---------- capability reply ('Q' extension, tag "STCP") ---------- */

export const CMD_CAPS = 0x51; // 'Q'
export const CAPS_TAG = "STCP";
export const CAPS_BYTES = 96;
export const STEM_TAPE_FIRMWARE_ID = 0x53544657; // 'STFW'

export const CAPS_OFF = {
  firmwareId: 0, // u32
  protoMajor: 4, // u16
  protoMinor: 6, // u16
  formatMajor: 8, // u16
  formatMinor: 10, // u16
  flags: 12, // u32
  sampleRate: 16, // u32
  blockSize: 20, // u32
  sectorBytes: 24, // u32
  alignment: 28, // u32
  deviceBlocks: 32, // u32 — total addressable device capacity in blocks
  songAStart: 36, // u32
  songABlocks: 40, // u32
  songBStart: 44, // u32
  songBBlocks: 48, // u32
  indexAStart: 52, // u32
  indexABlocks: 56, // u32
  indexBStart: 60, // u32
  indexBBlocks: 64, // u32
  activeIndexSlot: 68, // u32 — 0=A, 1=B, 0xffffffff = none
  activeSongSlot: 72, // u32 — 0=A, 1=B, 0xffffffff = none
  activeGenerationLo: 76, // u32
  activeGenerationHi: 80, // u32
  stixVersion: 84, // u16
  reserved: 86, // 10 bytes, must be zero
} as const;

export const CAP_FLAG = {
  FOUR_STEMS: 1 << 0,
  STEREO: 1 << 1,
  RATE_48K: 1 << 2,
  DEPTH_24: 1 << 3,
  INDEX_EXTENSION: 1 << 4,
  BPM_DOWNBEAT: 1 << 5,
  STAGING_COW: 1 << 6,
  EXPLICIT_INIT: 1 << 7,
  /** Two independently addressed song-data regions. */
  DUAL_SONG_SLOTS: 1 << 8,
  /** Two independently addressed STIX index regions. */
  DUAL_INDEX_SLOTS: 1 << 9,
  /** Active record chosen by generation number, not by a mutable pointer. */
  GENERATION_COMMIT: 1 << 10,
  /** Firmware guarantees an interrupted replacement leaves the old song valid. */
  CRASH_SAFE_REPLACE: 1 << 11,
} as const;

export const REQUIRED_CAP_FLAGS =
  CAP_FLAG.FOUR_STEMS |
  CAP_FLAG.STEREO |
  CAP_FLAG.RATE_48K |
  CAP_FLAG.DEPTH_24 |
  CAP_FLAG.INDEX_EXTENSION |
  CAP_FLAG.BPM_DOWNBEAT |
  CAP_FLAG.EXPLICIT_INIT |
  CAP_FLAG.DUAL_SONG_SLOTS |
  CAP_FLAG.DUAL_INDEX_SLOTS |
  CAP_FLAG.GENERATION_COMMIT |
  CAP_FLAG.CRASH_SAFE_REPLACE;

/* ---------- STIX v2 index record ---------- */

export const INDEX_MAGIC = 0x53544958; // 'STIX' — validity magic, written LAST
/** One index record. Fits inside a single 512-byte block so the magic-last
 *  write is one atomic block write. */
export const INDEX_RECORD_BYTES = 256;
export const TEXT_BYTES = 60;

export const IX_OFF = {
  magic: 0, // u32 — 0 while uncommitted, 'STIX' once committed
  indexVersion: 4, // u16
  formatMajor: 6, // u16
  formatMinor: 8, // u16
  slotIdentity: 10, // u8 — 0=A, 1=B; must equal the region it was read from
  songSlot: 11, // u8 — 0=A, 1=B; the song-data region this index describes
  flags: 12, // u16 — bit0 SONG_PRESENT
  reserved0: 14, // u16
  generationLo: 16, // u32
  generationHi: 20, // u32
  songStartBlock: 24, // u32
  songBlockCount: 28, // u32
  frames: 32, // u32
  sectorCount: 36, // u32
  sampleRate: 40, // u32
  channels: 44, // u16
  bitDepth: 46, // u16
  bpmQ8: 48, // u32
  downbeatFrame: 52, // u32
  originalFrames: 56, // u32 * 4
  stemChecksums: 72, // u32 * 4
  songChecksum: 88, // u32
  title: 92, // 60 bytes, UTF-8, NUL-padded
  artist: 152, // 60 bytes, UTF-8, NUL-padded
  reserved1: 212, // 40 bytes, must be zero
  crc32: 252, // u32 — last field
} as const;

export const IX_FLAG = { SONG_PRESENT: 1 << 0 } as const;

/**
 * CRC coverage, stated explicitly so no implementation has to guess:
 *   crc32 = CRC-32(record[0 .. 252)) with record[0 .. 4) — the validity magic —
 *   NORMALIZED TO 0x00000000 during the calculation.
 * The CRC field itself (252..256) is excluded. Bytes beyond the 256-byte record
 * inside the index block are not covered and must be zero.
 */
export const CRC_RANGE = { from: 0, to: IX_OFF.crc32 } as const;
export const CRC_ZEROED = { from: IX_OFF.magic, to: IX_OFF.magic + 4 } as const;

/** Generation is an unsigned 64-bit counter (lo/hi u32 pair), compared purely
 *  numerically: strictly greater wins. It starts at 1 and increments by exactly
 *  one per commit, so at one commit per second it cannot wrap for >5e11 years.
 *  There is no modular/wrapping comparison. */
export const GENERATION_MAX = Number.MAX_SAFE_INTEGER;

/* ---------- A/B regions ---------- */

export type AbSlot = 0 | 1;
export const SLOT_A: AbSlot = 0;
export const SLOT_B: AbSlot = 1;
export const NO_SLOT = 0xffffffff;
export function otherSlot(s: AbSlot): AbSlot {
  return s === SLOT_A ? SLOT_B : SLOT_A;
}
export function slotName(s: AbSlot): "A" | "B" {
  return s === SLOT_A ? "A" : "B";
}

export interface BlockRegion {
  /** First physical 512-byte block, device-reported. */
  start: number;
  /** Length in whole 512-byte blocks, device-reported. */
  blocks: number;
}

export function regionEnd(r: BlockRegion): number {
  return r.start + r.blocks;
}

export function regionsOverlap(a: BlockRegion, b: BlockRegion): boolean {
  return a.start < regionEnd(b) && b.start < regionEnd(a);
}

/** Sectors a song region can hold (whole 8 KiB logical sectors only). */
export function sectorsInRegion(r: BlockRegion): number {
  return Math.floor(r.blocks / BLOCKS_PER_SECTOR);
}
