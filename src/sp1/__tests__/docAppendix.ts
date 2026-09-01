/**
 * Generates the numeric appendix of docs/stem-tape-transfer-v1.1.md directly
 * from the production constants. The document must contain this text verbatim;
 * `docContract.test.ts` fails the moment a constant and the document diverge, so
 * the published contract can never drift from the code that implements it.
 */
import {
  BLOCKS_PER_SECTOR,
  BYTES_PER_FRAME_V11,
  BYTES_PER_SAMPLE,
  BYTES_PER_SAMPLE_V11,
  BYTES_PER_STEM_FRAME,
  CAPS_BYTES,
  CAPS_OFF,
  CAPS_TAG,
  CAP_FLAG,
  CHANNELS,
  CMD_CAPS,
  CRC_RANGE,
  CRC_ZEROED,
  FORMAT_MAJOR,
  FORMAT_MINOR,
  FRAMES_PER_SECTOR_V11,
  FRAMES_PER_GROUP,
  INDEX_MAGIC,
  INDEX_RECORD_BYTES,
  IX_FLAG,
  IX_OFF,
  NO_SLOT,
  PCM_BIT_DEPTH,
  PCM_BIT_DEPTH_V11,
  PHYSICAL_BLOCK_BYTES,
  PROTOCOL_MAJOR,
  PROTOCOL_MINOR,
  REQUIRED_ALIGNMENT,
  REQUIRED_CAP_FLAGS,
  SAMPLE_RATE,
  SECTOR_BYTES,
  SECTOR_HEADER_BYTES,
  SECTOR_PAYLOAD_BYTES,
  SLOT_A,
  SLOT_B,
  STEM_COUNT,
  STEM_TAPE_FIRMWARE_ID,
  STIX_VERSION,
  TEXT_BYTES,
} from "../stemTapeFormat";
import { ACK, BAUD_RATE, CONNECT_MAGIC } from "../protocol";
import { MAX_CHUNK_RETRIES } from "../transport";

const hex = (n: number, w = 8) => `0x${n.toString(16).padStart(w, "0")}`;

/** width in bytes of each STCP field, in declaration order */
const CAPS_WIDTH: Record<keyof typeof CAPS_OFF, number> = {
  firmwareId: 4,
  protoMajor: 2,
  protoMinor: 2,
  formatMajor: 2,
  formatMinor: 2,
  flags: 4,
  sampleRate: 4,
  blockSize: 4,
  sectorBytes: 4,
  alignment: 4,
  deviceBlocks: 4,
  songAStart: 4,
  songABlocks: 4,
  songBStart: 4,
  songBBlocks: 4,
  indexAStart: 4,
  indexABlocks: 4,
  indexBStart: 4,
  indexBBlocks: 4,
  activeIndexSlot: 4,
  activeSongSlot: 4,
  activeGenerationLo: 4,
  activeGenerationHi: 4,
  stixVersion: 2,
  reserved: 10,
};

const IX_WIDTH: Record<keyof typeof IX_OFF, number> = {
  magic: 4,
  indexVersion: 2,
  formatMajor: 2,
  formatMinor: 2,
  slotIdentity: 1,
  songSlot: 1,
  flags: 2,
  reserved0: 2,
  generationLo: 4,
  generationHi: 4,
  songStartBlock: 4,
  songBlockCount: 4,
  frames: 4,
  sectorCount: 4,
  sampleRate: 4,
  channels: 2,
  bitDepth: 2,
  bpmQ8: 4,
  downbeatFrame: 4,
  originalFrames: 16,
  stemChecksums: 16,
  songChecksum: 4,
  title: TEXT_BYTES,
  artist: TEXT_BYTES,
  reserved1: 40,
  crc32: 4,
};

/**
 * `formatMinor` is a parameter so the frozen v1.1 contract can still be
 * regenerated verbatim after the companion moved to v1.3 (planar 16-bit
 * groups). Version 1 selects the frozen mixed-frame 24-bit geometry and the
 * protocol minor that shipped with it; anything else describes v1.3.
 */
export function buildAppendix(formatMinor: number = FORMAT_MINOR): string {
  const v11 = formatMinor === 1;
  const protocolMinor = v11 ? 1 : PROTOCOL_MINOR;
  const bitDepth = v11 ? PCM_BIT_DEPTH_V11 : PCM_BIT_DEPTH;
  const bytesPerSample = v11 ? BYTES_PER_SAMPLE_V11 : BYTES_PER_SAMPLE;
  const bytesPerFrame = v11 ? BYTES_PER_FRAME_V11 : BYTES_PER_STEM_FRAME;
  const framesPerUnit = v11 ? FRAMES_PER_SECTOR_V11 : FRAMES_PER_GROUP;
  const L: string[] = [];
  L.push(
    "## 12. Generated numeric appendix",
    "",
    "Generated from `src/sp1/stemTapeFormat.ts`, `protocol.ts` and `transport.ts`",
    "by `src/sp1/__tests__/docAppendix.ts`; asserted verbatim by",
    "`src/sp1/__tests__/docContract.test.ts`. All multi-byte fields are",
    "**little-endian**. Do not edit by hand.",
    "",
    "### 12.1 Transport",
    "",
    "| item | value |",
    "| --- | --- |",
    `| physical block | ${PHYSICAL_BLOCK_BYTES} B |`,
    `| baud rate | ${BAUD_RATE} |`,
    `| entry magic | \`${new TextDecoder().decode(CONNECT_MAGIC)}\` |`,
    `| read ack | ${hex(ACK.READ, 2)} |`,
    `| write ack | ${hex(ACK.WRITE, 2)} |`,
    `| flush ack | ${hex(ACK.FLUSH, 2)} |`,
    `| capability command | ${hex(CMD_CAPS, 2)} \`Q\` |`,
    `| capability tag | \`${CAPS_TAG}\` |`,
    `| capability payload | ${CAPS_BYTES} B |`,
    `| per-block write retries | ${MAX_CHUNK_RETRIES} |`,
    "",
    "### 12.2 Versions and identity",
    "",
    "| item | value |",
    "| --- | --- |",
    `| firmware id | ${hex(STEM_TAPE_FIRMWARE_ID)} \`STFW\` |`,
    `| protocol | ${PROTOCOL_MAJOR}.${protocolMinor} |`,
    `| format | ${FORMAT_MAJOR}.${formatMinor} |`,
    `| STIX index version | ${STIX_VERSION} |`,
    `| index magic | ${hex(INDEX_MAGIC)} \`STIX\` |`,
    `| slot A / slot B | ${SLOT_A} / ${SLOT_B} |`,
    `| "no slot" sentinel | ${hex(NO_SLOT)} |`,
    "",
    "### 12.3 Audio geometry",
    "",
    "| item | value |",
    "| --- | --- |",
    `| sample rate | ${SAMPLE_RATE} Hz |`,
    `| stems x channels | ${STEM_COUNT} x ${CHANNELS} |`,
    `| bit depth | ${bitDepth}-bit (${bytesPerSample} B/sample) |`,
    `| bytes per frame | ${bytesPerFrame} |`,
    `| blocks per sector | ${BLOCKS_PER_SECTOR} |`,
    `| sector | ${SECTOR_BYTES} B = ${SECTOR_HEADER_BYTES} B header + ${SECTOR_PAYLOAD_BYTES} B payload |`,
    `| frames per sector | ${framesPerUnit} |`,
    `| region alignment | ${REQUIRED_ALIGNMENT} B |`,
    "",
    "### 12.4 Capability flags",
    "",
    "| flag | bit | required |",
    "| --- | ---: | --- |",
  );
  for (const [name, bit] of Object.entries(CAP_FLAG)) {
    // Bit 3 asserts the storage sample width, so its NAME moved with the
    // format: DEPTH_24 in v1.1, DEPTH_16 in v1.3. Same bit, same position.
    const shown = v11 && name === "DEPTH_16" ? "DEPTH_24" : name;
    L.push(`| ${shown} | ${Math.log2(bit)} | ${(REQUIRED_CAP_FLAGS & bit) === bit ? "yes" : "no"} |`);
  }
  L.push(
    `| **REQUIRED_CAP_FLAGS** | | ${hex(REQUIRED_CAP_FLAGS)} |`,
    "",
    "### 12.5 STCP capability record offsets",
    "",
    "| field | offset | size |",
    "| --- | ---: | ---: |",
  );
  for (const [name, off] of Object.entries(CAPS_OFF)) {
    L.push(`| ${name} | ${off} | ${CAPS_WIDTH[name as keyof typeof CAPS_OFF]} |`);
  }
  L.push(
    `| **total** | | ${CAPS_BYTES} |`,
    "",
    "### 12.6 STIX v2 index record offsets",
    "",
    "| field | offset | size |",
    "| --- | ---: | ---: |",
  );
  for (const [name, off] of Object.entries(IX_OFF)) {
    L.push(`| ${name} | ${off} | ${IX_WIDTH[name as keyof typeof IX_OFF]} |`);
  }
  L.push(
    `| **total** | | ${INDEX_RECORD_BYTES} |`,
    "",
    `Index flags: SONG_PRESENT = bit ${Math.log2(IX_FLAG.SONG_PRESENT)}.`,
    "",
    "### 12.7 CRC and commit rules",
    "",
    `- CRC-32 (IEEE 802.3) covers record bytes [${CRC_RANGE.from}, ${CRC_RANGE.to}).`,
    `- Bytes [${CRC_ZEROED.from}, ${CRC_ZEROED.to}) — the validity magic — are normalized to zero while computing it.`,
    `- The CRC field itself at offset ${IX_OFF.crc32} is excluded; bytes ${INDEX_RECORD_BYTES}..${PHYSICAL_BLOCK_BYTES} of the index block must be zero.`,
    "- Generation is an unsigned 64-bit lo/hi pair, starts at 1, increments by 1 per commit, compared numerically; strictly greater wins, tie resolves to slot A.",
    `- The magic ${hex(INDEX_MAGIC)} is written LAST and is the sole commit point; a record with magic 0 is a complete but uncommitted record sharing the committed record's CRC.`,
    "- Initialization is legal only when BOTH index records are invalid or blank, and is always explicit.",
    "- After reconnect the outcome is decided only by reparsing both stored records with the shared selector: a valid new generation is `committed`, a valid previous generation is `failed`, both invalid is `corrupt`.",
    "",
  );
  return L.join("\n");
}
