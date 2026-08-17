/**
 * Stem Tape compatibility gate (contract v1.1, A/B storage).
 *
 * Answering the classic `SP1XFER!` magic proves only that an SP-1-class device
 * with the Tape Looper transfer build is listening. It authorizes NOTHING here.
 * Physical mutation requires the Stem Tape capability reply ('Q' -> "STCP") to
 * positively report firmware identity, protocol version, storage-format
 * version 1.1, STIX index version 2, four-stem/stereo/48 kHz/24-bit support,
 * BPM+downbeat metadata, explicit-init support AND the four crash-safety
 * capabilities: dual song slots, dual index slots, generation-based commit and
 * crash-safe replacement.
 *
 * Stock SP-1, ordinary Tape Looper firmware and any v1.0 single-index Stem Tape
 * firmware are detected and remain READ-ONLY. Storage addresses are never
 * guessed: every block address below comes from the device's own reply and is
 * validated for alignment, non-overlap and device bounds before any write.
 */

import {
  BLOCKS_PER_SECTOR,
  CAPS_BYTES,
  CAPS_OFF,
  CAP_FLAG,
  FORMAT_MAJOR,
  FORMAT_MINOR,
  INDEX_RECORD_BYTES,
  NO_SLOT,
  PHYSICAL_BLOCK_BYTES,
  PROTOCOL_MAJOR,
  PROTOCOL_MINOR,
  REQUIRED_ALIGNMENT,
  SECTOR_BYTES,
  STEM_TAPE_FIRMWARE_ID,
  STIX_VERSION,
  regionsOverlap,
  sectorsInRegion,
  type BlockRegion,
} from "./stemTapeFormat";
import { joinGeneration, splitGeneration } from "./stemIndex";

export const READ_ONLY_NOTICE =
  "This device answers the Tape Looper transfer protocol but does not report Stem Tape v1.1 A/B firmware. It stays read-only: no initialization, no writes, no delete.";

export interface StemTapeCapabilities {
  firmwareId: number;
  protoMajor: number;
  protoMinor: number;
  formatMajor: number;
  formatMinor: number;
  flags: number;
  sampleRate: number;
  blockSize: number;
  sectorBytes: number;
  alignment: number;
  /** Total addressable device capacity in 512-byte blocks. */
  deviceBlocks: number;
  /** [A, B] song-data regions, device-reported. */
  song: [BlockRegion, BlockRegion];
  /** [A, B] index regions, device-reported. */
  index: [BlockRegion, BlockRegion];
  /** Device's own belief; advisory only — the valid records decide. */
  activeIndexSlot: number;
  activeSongSlot: number;
  activeGeneration: number;
  stixVersion: number;
}

function get32(a: Uint8Array, o: number): number {
  return ((a[o]! | (a[o + 1]! << 8) | (a[o + 2]! << 16)) >>> 0) + a[o + 3]! * 0x1000000;
}
function get16(a: Uint8Array, o: number): number {
  return a[o]! | (a[o + 1]! << 8);
}
function put32(a: Uint8Array, o: number, v: number) {
  a[o] = v & 255;
  a[o + 1] = (v >>> 8) & 255;
  a[o + 2] = (v >>> 16) & 255;
  a[o + 3] = (v >>> 24) & 255;
}
function put16(a: Uint8Array, o: number, v: number) {
  a[o] = v & 255;
  a[o + 1] = (v >>> 8) & 255;
}

/** The one parser. Used by the companion, the mock device and the fixtures. */
export function parseCapabilities(bytes: Uint8Array): StemTapeCapabilities {
  return {
    firmwareId: get32(bytes, CAPS_OFF.firmwareId),
    protoMajor: get16(bytes, CAPS_OFF.protoMajor),
    protoMinor: get16(bytes, CAPS_OFF.protoMinor),
    formatMajor: get16(bytes, CAPS_OFF.formatMajor),
    formatMinor: get16(bytes, CAPS_OFF.formatMinor),
    flags: get32(bytes, CAPS_OFF.flags),
    sampleRate: get32(bytes, CAPS_OFF.sampleRate),
    blockSize: get32(bytes, CAPS_OFF.blockSize),
    sectorBytes: get32(bytes, CAPS_OFF.sectorBytes),
    alignment: get32(bytes, CAPS_OFF.alignment),
    deviceBlocks: get32(bytes, CAPS_OFF.deviceBlocks),
    song: [
      { start: get32(bytes, CAPS_OFF.songAStart), blocks: get32(bytes, CAPS_OFF.songABlocks) },
      { start: get32(bytes, CAPS_OFF.songBStart), blocks: get32(bytes, CAPS_OFF.songBBlocks) },
    ],
    index: [
      { start: get32(bytes, CAPS_OFF.indexAStart), blocks: get32(bytes, CAPS_OFF.indexABlocks) },
      { start: get32(bytes, CAPS_OFF.indexBStart), blocks: get32(bytes, CAPS_OFF.indexBBlocks) },
    ],
    activeIndexSlot: get32(bytes, CAPS_OFF.activeIndexSlot),
    activeSongSlot: get32(bytes, CAPS_OFF.activeSongSlot),
    activeGeneration: joinGeneration(
      get32(bytes, CAPS_OFF.activeGenerationLo),
      get32(bytes, CAPS_OFF.activeGenerationHi),
    ),
    stixVersion: get16(bytes, CAPS_OFF.stixVersion),
  };
}

/** The one serializer. Used by the mock device and by the binary fixtures. */
export function serializeCapabilities(c: StemTapeCapabilities): Uint8Array {
  const b = new Uint8Array(CAPS_BYTES);
  put32(b, CAPS_OFF.firmwareId, c.firmwareId);
  put16(b, CAPS_OFF.protoMajor, c.protoMajor);
  put16(b, CAPS_OFF.protoMinor, c.protoMinor);
  put16(b, CAPS_OFF.formatMajor, c.formatMajor);
  put16(b, CAPS_OFF.formatMinor, c.formatMinor);
  put32(b, CAPS_OFF.flags, c.flags);
  put32(b, CAPS_OFF.sampleRate, c.sampleRate);
  put32(b, CAPS_OFF.blockSize, c.blockSize);
  put32(b, CAPS_OFF.sectorBytes, c.sectorBytes);
  put32(b, CAPS_OFF.alignment, c.alignment);
  put32(b, CAPS_OFF.deviceBlocks, c.deviceBlocks);
  put32(b, CAPS_OFF.songAStart, c.song[0].start);
  put32(b, CAPS_OFF.songABlocks, c.song[0].blocks);
  put32(b, CAPS_OFF.songBStart, c.song[1].start);
  put32(b, CAPS_OFF.songBBlocks, c.song[1].blocks);
  put32(b, CAPS_OFF.indexAStart, c.index[0].start);
  put32(b, CAPS_OFF.indexABlocks, c.index[0].blocks);
  put32(b, CAPS_OFF.indexBStart, c.index[1].start);
  put32(b, CAPS_OFF.indexBBlocks, c.index[1].blocks);
  put32(b, CAPS_OFF.activeIndexSlot, c.activeIndexSlot);
  put32(b, CAPS_OFF.activeSongSlot, c.activeSongSlot);
  const g = splitGeneration(c.activeGeneration);
  put32(b, CAPS_OFF.activeGenerationLo, g.lo);
  put32(b, CAPS_OFF.activeGenerationHi, g.hi);
  put16(b, CAPS_OFF.stixVersion, c.stixVersion);
  return b;
}

/* ---------- region validation ---------- */

export interface RegionProblem {
  region: string;
  problem: string;
}

/** Every rejection rule for the four reported regions, in one place. */
export function validateRegions(c: StemTapeCapabilities): RegionProblem[] {
  const out: RegionProblem[] = [];
  const named: [string, BlockRegion, number][] = [
    ["song A", c.song[0], c.sectorBytes || SECTOR_BYTES],
    ["song B", c.song[1], c.sectorBytes || SECTOR_BYTES],
    ["index A", c.index[0], PHYSICAL_BLOCK_BYTES],
    ["index B", c.index[1], PHYSICAL_BLOCK_BYTES],
  ];
  if (c.blockSize !== PHYSICAL_BLOCK_BYTES) {
    out.push({ region: "device", problem: `block size ${c.blockSize} is not ${PHYSICAL_BLOCK_BYTES}` });
  }
  if (c.sectorBytes !== SECTOR_BYTES) {
    out.push({ region: "device", problem: `logical sector size ${c.sectorBytes} is not ${SECTOR_BYTES}` });
  }
  if (c.alignment !== REQUIRED_ALIGNMENT) {
    out.push({ region: "device", problem: `required alignment ${c.alignment} is not ${REQUIRED_ALIGNMENT}` });
  }
  if (c.deviceBlocks < 1) out.push({ region: "device", problem: "device reports zero capacity" });

  for (const [name, r] of named) {
    if (r.blocks === 0) {
      out.push({ region: name, problem: "missing or zero-sized region" });
      continue;
    }
    if (!Number.isSafeInteger(r.start) || !Number.isSafeInteger(r.blocks)) {
      out.push({ region: name, problem: "non-integer region bounds" });
      continue;
    }
    if (c.deviceBlocks && r.start + r.blocks > c.deviceBlocks) {
      out.push({ region: name, problem: `extends past reported device capacity (${c.deviceBlocks} blocks)` });
    }
  }
  // Alignment: every region is expressed in whole blocks, and each song region
  // must be a whole number of 8 KiB logical sectors.
  for (const [name, r] of named.slice(0, 2)) {
    if (r.blocks && r.blocks % BLOCKS_PER_SECTOR !== 0) {
      out.push({ region: name, problem: `length ${r.blocks} blocks is not a whole number of logical sectors` });
    }
  }
  for (const [name, r] of named.slice(2)) {
    if (r.blocks && r.blocks * PHYSICAL_BLOCK_BYTES < INDEX_RECORD_BYTES) {
      out.push({ region: name, problem: "too small to hold one STIX v2 record" });
    }
  }
  // Overlap: all four regions must be mutually disjoint.
  const all: [string, BlockRegion][] = [
    ["song A", c.song[0]],
    ["song B", c.song[1]],
    ["index A", c.index[0]],
    ["index B", c.index[1]],
  ];
  for (let i = 0; i < all.length; i++) {
    for (let j = i + 1; j < all.length; j++) {
      const [na, ra] = all[i]!;
      const [nb, rb] = all[j]!;
      if (ra.blocks && rb.blocks && regionsOverlap(ra, rb)) {
        out.push({ region: `${na} / ${nb}`, problem: "regions overlap" });
      }
    }
  }
  // Identical active and staging regions are a contradiction.
  if (c.song[0].start === c.song[1].start) out.push({ region: "song A / song B", problem: "identical regions" });
  if (c.index[0].start === c.index[1].start) out.push({ region: "index A / index B", problem: "identical regions" });
  // The device's own active pointers must at least name real slots.
  for (const [label, v] of [
    ["active index slot", c.activeIndexSlot],
    ["active song slot", c.activeSongSlot],
  ] as const) {
    if (v !== 0 && v !== 1 && v !== NO_SLOT) out.push({ region: "device", problem: `${label} ${v} is not A, B or none` });
  }
  return out;
}

export interface Requirement {
  id: string;
  label: string;
  satisfied: boolean;
  detail: string;
}

export interface CompatibilityVerdict {
  /** True only when every requirement below is satisfied. */
  writable: boolean;
  requirements: Requirement[];
  summary: string;
  /** Copy-on-write / staging support, reported by the device (informational:
   *  A/B replacement does not depend on it). */
  staging: boolean;
  regionProblems: RegionProblem[];
}

export interface CapacityDemand {
  /** Logical 8 KiB sectors the prepared song needs. */
  requiredSectors: number;
}

export function evaluate(caps: StemTapeCapabilities | null, demand?: CapacityDemand): CompatibilityVerdict {
  const f = caps?.flags ?? 0;
  const has = (bit: number) => (f & bit) !== 0;
  const req = (id: string, label: string, satisfied: boolean, detail: string): Requirement => ({
    id,
    label,
    satisfied,
    detail,
  });
  const problems = caps ? validateRegions(caps) : [{ region: "device", problem: "no capability reply" }];

  const requirements: Requirement[] = [
    req(
      "firmware",
      "Stem Tape firmware identity",
      caps?.firmwareId === STEM_TAPE_FIRMWARE_ID,
      caps ? `0x${(caps.firmwareId >>> 0).toString(16)}` : "no capability reply",
    ),
    req(
      "protocol",
      `transfer protocol ${PROTOCOL_MAJOR}.${PROTOCOL_MINOR} or newer`,
      caps?.protoMajor === PROTOCOL_MAJOR && (caps?.protoMinor ?? -1) >= PROTOCOL_MINOR,
      caps ? `${caps.protoMajor}.${caps.protoMinor}` : "not reported",
    ),
    req(
      "format",
      `storage format ${FORMAT_MAJOR}.${FORMAT_MINOR} (A/B)`,
      caps?.formatMajor === FORMAT_MAJOR && caps?.formatMinor === FORMAT_MINOR,
      caps ? `${caps.formatMajor}.${caps.formatMinor}` : "not reported",
    ),
    req(
      "stix",
      `STIX index version ${STIX_VERSION}`,
      caps?.stixVersion === STIX_VERSION,
      caps ? `v${caps.stixVersion}` : "not reported",
    ),
    req("four-stems", "four-stem support", has(CAP_FLAG.FOUR_STEMS), has(CAP_FLAG.FOUR_STEMS) ? "yes" : "not reported"),
    req("stereo", "stereo support", has(CAP_FLAG.STEREO), has(CAP_FLAG.STEREO) ? "yes" : "not reported"),
    req(
      "rate",
      "48 kHz support",
      has(CAP_FLAG.RATE_48K) && caps?.sampleRate === 48000,
      caps ? `${caps.sampleRate} Hz` : "not reported",
    ),
    req("depth", "24-bit support", has(CAP_FLAG.DEPTH_24), has(CAP_FLAG.DEPTH_24) ? "yes" : "not reported"),
    req(
      "index-ext",
      "index-extension support",
      has(CAP_FLAG.INDEX_EXTENSION),
      has(CAP_FLAG.INDEX_EXTENSION) ? "yes" : "not reported",
    ),
    req(
      "metadata",
      "BPM / downbeat metadata support",
      has(CAP_FLAG.BPM_DOWNBEAT),
      has(CAP_FLAG.BPM_DOWNBEAT) ? "yes" : "not reported",
    ),
    req(
      "dual-song",
      "dual song-data slots (A/B)",
      has(CAP_FLAG.DUAL_SONG_SLOTS),
      caps && has(CAP_FLAG.DUAL_SONG_SLOTS)
        ? `A ${caps.song[0].start}+${caps.song[0].blocks} · B ${caps.song[1].start}+${caps.song[1].blocks}`
        : "not reported",
    ),
    req(
      "dual-index",
      "dual index slots (A/B)",
      has(CAP_FLAG.DUAL_INDEX_SLOTS),
      caps && has(CAP_FLAG.DUAL_INDEX_SLOTS)
        ? `A ${caps.index[0].start}+${caps.index[0].blocks} · B ${caps.index[1].start}+${caps.index[1].blocks}`
        : "not reported",
    ),
    req(
      "generation-commit",
      "generation-based commit",
      has(CAP_FLAG.GENERATION_COMMIT),
      caps && has(CAP_FLAG.GENERATION_COMMIT) ? `generation ${caps.activeGeneration}` : "not reported",
    ),
    req(
      "crash-safe",
      "crash-safe replacement",
      has(CAP_FLAG.CRASH_SAFE_REPLACE),
      has(CAP_FLAG.CRASH_SAFE_REPLACE) ? "yes" : "not reported",
    ),
    req(
      "regions",
      "aligned, non-overlapping, in-bounds A/B regions",
      !!caps && problems.length === 0,
      problems.length === 0 ? "all four regions validated" : problems.map((p) => `${p.region}: ${p.problem}`).join(" · "),
    ),
    req(
      "explicit-init",
      "explicit-initialization support",
      has(CAP_FLAG.EXPLICIT_INIT),
      has(CAP_FLAG.EXPLICIT_INIT) ? "yes" : "not reported",
    ),
  ];

  if (demand) {
    const perSlot = caps ? Math.min(sectorsInRegion(caps.song[0]), sectorsInRegion(caps.song[1])) : 0;
    requirements.push(
      req(
        "capacity",
        "sufficient safe staging capacity for this song",
        !!caps && demand.requiredSectors > 0 && demand.requiredSectors <= perSlot,
        caps ? `needs ${demand.requiredSectors} of ${perSlot} logical sectors per song slot` : "not reported",
      ),
    );
  }

  const writable = requirements.every((r) => r.satisfied);
  return {
    writable,
    requirements,
    staging: has(CAP_FLAG.STAGING_COW),
    regionProblems: problems,
    summary: writable ? "Stem Tape v1.1 A/B firmware negotiated — writes enabled" : READ_ONLY_NOTICE,
  };
}

export function readOnlyVerdict(): CompatibilityVerdict {
  return evaluate(null);
}

/** Field-by-field capability equality, used for the immediately-before-write re-query. */
export function sameCapabilities(a: StemTapeCapabilities | null, b: StemTapeCapabilities | null): boolean {
  if (!a || !b) return false;
  const flat = (c: StemTapeCapabilities) =>
    JSON.stringify({ ...c, activeIndexSlot: undefined, activeSongSlot: undefined, activeGeneration: undefined });
  // Active pointers legitimately change as generations advance; the immutable
  // geometry and version fields must not.
  return flat(a) === flat(b);
}
