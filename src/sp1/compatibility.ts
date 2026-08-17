/**
 * Stem Tape compatibility gate.
 *
 * Answering the classic `SP1XFER!` magic proves only that an SP-1-class device
 * with the Tape Looper transfer build is listening. It authorizes NOTHING here.
 * Physical mutation requires the Stem Tape capability reply ('Q' -> "STCP") to
 * positively report firmware identity, protocol version, storage-format
 * version, four-stem/stereo/48 kHz/24-bit support, the index extension,
 * BPM+downbeat metadata, storage boundaries and explicit-init support.
 *
 * Stock SP-1 and ordinary Tape Looper firmware are detected and remain
 * READ-ONLY. Storage addresses are never guessed: every address below comes
 * from the device's own reply.
 */

import {
  CAP_FLAG,
  CAPS_OFF,
  FORMAT_MAJOR,
  PROTOCOL_MAJOR,
  STEM_TAPE_FIRMWARE_ID,
} from "./stemTapeFormat";

export const READ_ONLY_NOTICE =
  "This device answers the Tape Looper transfer protocol but does not report Stem Tape firmware. It stays read-only: no initialization, no writes, no delete.";

export interface StemTapeCapabilities {
  firmwareId: number;
  protoMajor: number;
  protoMinor: number;
  formatMajor: number;
  formatMinor: number;
  flags: number;
  sampleRate: number;
  songSlots: number;
  /** Blocks reserved for the index extension, starting at block 0. */
  indexBlocks: number;
  /** First physical block of song storage. Device-reported, never guessed. */
  libraryBase: number;
  /** Logical 8 KB sectors reserved per song slot. */
  sectorsPerSong: number;
  generation: number;
}

function get32(a: Uint8Array, o: number): number {
  return ((a[o]! | (a[o + 1]! << 8) | (a[o + 2]! << 16)) >>> 0) + a[o + 3]! * 0x1000000;
}
function get16(a: Uint8Array, o: number): number {
  return a[o]! | (a[o + 1]! << 8);
}

export function parseCapabilities(bytes: Uint8Array): StemTapeCapabilities {
  return {
    firmwareId: get32(bytes, CAPS_OFF.firmwareId),
    protoMajor: get16(bytes, CAPS_OFF.protoMajor),
    protoMinor: get16(bytes, CAPS_OFF.protoMinor),
    formatMajor: get16(bytes, CAPS_OFF.formatMajor),
    formatMinor: get16(bytes, CAPS_OFF.formatMinor),
    flags: get32(bytes, CAPS_OFF.flags),
    sampleRate: get32(bytes, CAPS_OFF.sampleRate),
    songSlots: get32(bytes, CAPS_OFF.songSlots),
    indexBlocks: get32(bytes, CAPS_OFF.indexBlocks),
    libraryBase: get32(bytes, CAPS_OFF.libraryBase),
    sectorsPerSong: get32(bytes, CAPS_OFF.sectorsPerSong),
    generation: get32(bytes, CAPS_OFF.generation),
  };
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
  /** Copy-on-write / staging support, reported by the device. */
  staging: boolean;
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

  const requirements: Requirement[] = [
    req("firmware", "Stem Tape firmware identity", caps?.firmwareId === STEM_TAPE_FIRMWARE_ID, caps ? `0x${(caps.firmwareId >>> 0).toString(16)}` : "no capability reply"),
    req("protocol", "compatible transfer-protocol version", caps?.protoMajor === PROTOCOL_MAJOR, caps ? `${caps.protoMajor}.${caps.protoMinor}` : "not reported"),
    req("format", "compatible storage-format version", caps?.formatMajor === FORMAT_MAJOR, caps ? `${caps.formatMajor}.${caps.formatMinor}` : "not reported"),
    req("four-stems", "four-stem support", has(CAP_FLAG.FOUR_STEMS), has(CAP_FLAG.FOUR_STEMS) ? "yes" : "not reported"),
    req("stereo", "stereo support", has(CAP_FLAG.STEREO), has(CAP_FLAG.STEREO) ? "yes" : "not reported"),
    req("rate", "48 kHz support", has(CAP_FLAG.RATE_48K) && caps?.sampleRate === 48000, caps ? `${caps.sampleRate} Hz` : "not reported"),
    req("depth", "24-bit support", has(CAP_FLAG.DEPTH_24), has(CAP_FLAG.DEPTH_24) ? "yes" : "not reported"),
    req("index-ext", "index-extension support", has(CAP_FLAG.INDEX_EXTENSION), has(CAP_FLAG.INDEX_EXTENSION) ? "yes" : "not reported"),
    req("metadata", "BPM / downbeat metadata support", has(CAP_FLAG.BPM_DOWNBEAT), has(CAP_FLAG.BPM_DOWNBEAT) ? "yes" : "not reported"),
    req(
      "boundaries",
      "reported storage boundaries",
      !!caps && caps.songSlots > 0 && caps.indexBlocks > 0 && caps.libraryBase >= caps.indexBlocks && caps.sectorsPerSong > 0,
      caps ? `${caps.songSlots} slots · index ${caps.indexBlocks} blk · base ${caps.libraryBase} · ${caps.sectorsPerSong} sectors/song` : "not reported",
    ),
    req(
      "plausible-boundaries",
      "plausible, non-overlapping boundaries",
      !!caps &&
        caps.songSlots <= MAX_PLAUSIBLE_SLOTS &&
        caps.sectorsPerSong <= MAX_PLAUSIBLE_SECTORS_PER_SONG &&
        caps.indexBlocks <= MAX_PLAUSIBLE_INDEX_BLOCKS &&
        caps.libraryBase >= caps.indexBlocks,
      caps
        ? caps.libraryBase < caps.indexBlocks
          ? "song region overlaps the index region"
          : "within plausible limits"
        : "not reported",
    ),
    req("explicit-init", "explicit-initialization support", has(CAP_FLAG.EXPLICIT_INIT), has(CAP_FLAG.EXPLICIT_INIT) ? "yes" : "not reported"),
  ];

  if (demand) {
    requirements.push(
      req(
        "capacity",
        "sufficient capacity for this song",
        !!caps && demand.requiredSectors > 0 && demand.requiredSectors <= caps.sectorsPerSong,
        caps ? `needs ${demand.requiredSectors} of ${caps.sectorsPerSong} logical sectors per slot` : "not reported",
      ),
    );
  }

  const writable = requirements.every((r) => r.satisfied);
  return {
    writable,
    requirements,
    staging: has(CAP_FLAG.STAGING_COW),
    summary: writable
      ? "Stem Tape firmware negotiated — writes enabled"
      : READ_ONLY_NOTICE,
  };
}

/** Upper bounds used only to reject implausible device-reported boundaries. */
export const MAX_PLAUSIBLE_SLOTS = 256;
export const MAX_PLAUSIBLE_SECTORS_PER_SONG = 262144; // ≈ 2 GiB per slot
export const MAX_PLAUSIBLE_INDEX_BLOCKS = 4096;

export function readOnlyVerdict(): CompatibilityVerdict {
  return evaluate(null);
}

/** Field-by-field capability equality, used for the immediately-before-write re-query. */
export function sameCapabilities(a: StemTapeCapabilities | null, b: StemTapeCapabilities | null): boolean {
  if (!a || !b) return false;
  return (Object.keys(a) as (keyof StemTapeCapabilities)[]).every((k) => a[k] === b[k]);
}

