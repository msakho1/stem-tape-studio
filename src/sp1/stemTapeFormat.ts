/**
 * Stem Tape storage format — THE single source of numeric offsets.
 *
 * Layered on top of the unchanged Tape Looper transport (512-byte physical
 * blocks). The 512-byte block is the transfer/eMMC access unit; it is NOT the
 * audio format. Stem Tape groups sixteen physical blocks into one 8,192-byte
 * logical song sector, matching the 8 KB internal page size documented in
 * firmware/src/main.c ("TE's own format writes 8 KB sectors"), so every sector
 * write lands on one internal page without a read-modify-write.
 *
 * Provenance note (honest): the repository documents the 8 KB page grouping and
 * the Tape Looper index (firmware/src/main.c `struct meta_blk`, mirrored in
 * firmware/web/index.html), but it contains NO stereo/24-bit Stem Tape song
 * sector or index-extension definition. Those are defined here, once, and every
 * other module imports these constants — no conflicting offsets elsewhere.
 */

/* ---------- transport / sector geometry ---------- */

export const PHYSICAL_BLOCK_BYTES = 512;
export const BLOCKS_PER_SECTOR = 16;
export const SECTOR_BYTES = PHYSICAL_BLOCK_BYTES * BLOCKS_PER_SECTOR; // 8192

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

/* ---------- capability reply ('Q' extension) ---------- */

export const CMD_CAPS = 0x51; // 'Q'
export const CAPS_TAG = "STCP";
export const CAPS_BYTES = 40;
export const STEM_TAPE_FIRMWARE_ID = 0x53544657; // 'STFW'
export const PROTOCOL_MAJOR = 1;
export const FORMAT_MAJOR = 1;

export const CAPS_OFF = {
  firmwareId: 0,
  protoMajor: 4,
  protoMinor: 6,
  formatMajor: 8,
  formatMinor: 10,
  flags: 12,
  sampleRate: 16,
  songSlots: 20,
  indexBlocks: 24,
  libraryBase: 28,
  sectorsPerSong: 32,
  generation: 36,
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
} as const;

export const REQUIRED_CAP_FLAGS =
  CAP_FLAG.FOUR_STEMS |
  CAP_FLAG.STEREO |
  CAP_FLAG.RATE_48K |
  CAP_FLAG.DEPTH_24 |
  CAP_FLAG.INDEX_EXTENSION |
  CAP_FLAG.BPM_DOWNBEAT |
  CAP_FLAG.EXPLICIT_INIT;

/* ---------- versioned index extension ---------- */

export const INDEX_MAGIC = 0x53544958; // 'STIX' — authoritative, written last
export const INDEX_HEADER_BYTES = 32;
export const INDEX_ENTRY_BYTES = 192;
export const TEXT_BYTES = 60;

export const INDEX_OFF = {
  magic: 0,
  formatMajor: 4,
  formatMinor: 6,
  indexBytes: 8,
  generation: 12,
  slotCount: 16,
  sectorsPerSong: 20,
  currentSong: 24,
  reserved: 28,
} as const;

export const ENTRY_OFF = {
  committed: 0,
  startSector: 4,
  sectorCount: 8,
  sampleRate: 12,
  channels: 16,
  bitDepth: 18,
  frames: 20,
  originalFrames: 24, // u32 * 4
  stemChecksums: 40, // u32 * 4
  songChecksum: 56,
  bpmQ8: 60,
  downbeatFrame: 64,
  reserved: 68,
  title: 72, // 60 bytes
  artist: 132, // 60 bytes
} as const;

export function indexByteLength(slotCount: number): number {
  return INDEX_HEADER_BYTES + slotCount * INDEX_ENTRY_BYTES;
}

export function indexBlockCount(slotCount: number): number {
  return Math.ceil(indexByteLength(slotCount) / PHYSICAL_BLOCK_BYTES);
}

export function sectorsForFrames(frames: number): number {
  return Math.ceil(frames / FRAMES_PER_SECTOR);
}
