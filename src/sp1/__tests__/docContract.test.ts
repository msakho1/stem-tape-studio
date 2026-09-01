/**
 * SECTION 4 — DOCUMENTATION / CONSTANT CONSISTENCY
 *
 * docs/stem-tape-transfer-v1.1.md is the published contract. This test proves
 * the document agrees with the code that implements it and with the frozen
 * handoff binaries, so the frozen SHA-256 means something.
 *
 *   - the generated numeric appendix is present verbatim
 *   - every prose number in the document matches a production constant
 *   - the committed STCP binary decodes to the documented geometry and flags
 *   - the committed index binaries decode to the documented versions and CRC rule
 */
import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { buildAppendix } from "./docAppendix";
import {
  BLOCKS_PER_SECTOR,
  BYTES_PER_FRAME_V11,
  CAPS_BYTES,
  CHANNELS,
  CRC_RANGE,
  CRC_ZEROED,
  FORMAT_MAJOR,
  FORMAT_MINOR,
  FORMAT_MINOR_V11,
  FRAMES_PER_SECTOR_V11,
  INDEX_RECORD_BYTES,
  IX_OFF,
  PCM_BIT_DEPTH_V11,
  PHYSICAL_BLOCK_BYTES,
  REQUIRED_CAP_FLAGS,
  SAMPLE_RATE,
  SECTOR_BYTES,
  SECTOR_HEADER_BYTES,
  SECTOR_PAYLOAD_BYTES,
  STIX_VERSION,
  TEXT_BYTES,
} from "../stemTapeFormat";
import { ACK, BAUD_RATE } from "../protocol";
import { MAX_CHUNK_RETRIES } from "../transport";
import { parseCapabilities, validateRegions } from "../compatibility";
import { parseIndexRecord } from "../stemIndex";
import { crc32 } from "../crc32";

const DOC = "docs/stem-tape-transfer-v1.1.md";
const doc = readFileSync(DOC, "utf8");
const bin = (name: string) => new Uint8Array(readFileSync(`handoff/v1.1/binaries/${name}`));

describe("published contract matches the implementation", () => {
  it("contains the generated numeric appendix verbatim", () => {
    // The published v1.1 document is frozen; it is regenerated at its own version.
    const appendix = buildAppendix(FORMAT_MINOR_V11);
    // Point at the first divergent line instead of dumping the whole appendix.
    const want = appendix.split("\n");
    if (!doc.includes(appendix)) {
      const missing = want.find((l) => l.trim().length > 0 && !doc.includes(l));
      expect({ missingAppendixLine: missing }).toEqual({ missingAppendixLine: undefined });
    }
    expect(doc.includes(appendix)).toBe(true);
  });

  it("prose numbers match production constants", () => {
    const claims: [string, boolean][] = [
      ["512-byte blocks", doc.includes(`${PHYSICAL_BLOCK_BYTES}-byte blocks`)],
      ["baud", doc.includes(`${BAUD_RATE} baud`)],
      ["read cmd/reply", doc.includes("`0x52`") && doc.includes(`0x${ACK.READ.toString(16)}`)],
      ["write cmd/reply", doc.includes("`0x57`") && doc.includes(`0x${ACK.WRITE.toString(16)}`)],
      ["flush cmd/reply", doc.includes("`0x46`") && doc.includes(`0x${ACK.FLUSH.toString(16)}`)],
      ["record size", doc.includes(`STIX v2 index record (${INDEX_RECORD_BYTES} bytes`)],
      ["title/artist width", doc.includes(`${TEXT_BYTES} bytes each`)],
      ["sector", doc.includes(`${SECTOR_BYTES.toLocaleString("en-US")}-byte logical sector`)],
      ["blocks per sector", doc.includes(`${BLOCKS_PER_SECTOR} × ${PHYSICAL_BLOCK_BYTES}-byte`)],
      [
        "sector split",
        doc.includes(
          `${SECTOR_HEADER_BYTES}-byte header + ${SECTOR_PAYLOAD_BYTES.toLocaleString("en-US")}-byte payload`,
        ),
      ],
      [
        "frames per sector",
        doc.includes(`${FRAMES_PER_SECTOR_V11} frames/sector at ${BYTES_PER_FRAME_V11} B/frame`),
      ],
      [
        "audio format",
        doc.includes(`${SAMPLE_RATE / 1000} kHz · stereo · signed ${PCM_BIT_DEPTH_V11}-bit LE`),
      ],
      ["retry budget", doc.includes(`max ${MAX_CHUNK_RETRIES}`)],
      ["sector size rule", doc.includes(`${BLOCKS_PER_SECTOR} × ${PHYSICAL_BLOCK_BYTES} B`)],
      ["little-endian stated", doc.toLowerCase().includes("little-endian")],
      ["magic written last", doc.includes("**written last**")],
      ["tie to A", doc.includes("tie → A")],
      ["channels", doc.includes("stereo") && CHANNELS === 2],
    ];
    expect(claims.filter(([, ok]) => !ok).map(([name]) => name)).toEqual([]);
  });

  it("the committed STCP binary decodes to the documented geometry", () => {
    const raw = bin("stcp-capability-response.bin");
    expect(new TextDecoder().decode(raw.slice(0, 4))).toBe("STCP");
    expect(raw.length).toBe(4 + CAPS_BYTES);
    const caps = parseCapabilities(raw.slice(4));
    expect(caps.formatMajor).toBe(FORMAT_MAJOR);
    expect(caps.formatMinor).toBe(FORMAT_MINOR_V11); // frozen v1.1 fixture
    expect(caps.stixVersion).toBe(STIX_VERSION);
    expect(caps.sampleRate).toBe(SAMPLE_RATE);
    expect(caps.blockSize).toBe(PHYSICAL_BLOCK_BYTES);
    expect(caps.sectorBytes).toBe(SECTOR_BYTES);
    expect((caps.flags & REQUIRED_CAP_FLAGS) === REQUIRED_CAP_FLAGS).toBe(true);
    expect(caps.song).toHaveLength(2);
    expect(caps.index).toHaveLength(2);
    expect(validateRegions(caps)).toEqual([]);
  });

  it("the committed index binaries obey the documented CRC, magic and version rules", () => {
    const committed = bin("index-a-valid.bin");
    const uncommitted = bin("index-uncommitted.bin");
    for (const b of [committed, uncommitted]) {
      expect(b.length).toBe(PHYSICAL_BLOCK_BYTES);
      // Padding beyond the record must be zero.
      expect(b.slice(INDEX_RECORD_BYTES).every((x) => x === 0)).toBe(true);
      // Documented CRC coverage: [0, 252) with the magic normalized to zero.
      const covered = b.slice(CRC_RANGE.from, CRC_RANGE.to);
      covered.fill(0, CRC_ZEROED.from, CRC_ZEROED.to);
      const view = new DataView(b.buffer, b.byteOffset, b.byteLength);
      expect(crc32(covered)).toBe(view.getUint32(IX_OFF.crc32, true));
      const rec = parseIndexRecord(b);
      expect(rec.indexVersion).toBe(STIX_VERSION);
      expect(rec.formatMajor).toBe(FORMAT_MAJOR);
      expect(rec.formatMinor).toBe(FORMAT_MINOR_V11); // frozen v1.1 fixture
      expect(rec.sampleRate).toBe(SAMPLE_RATE);
      expect(rec.channels).toBe(CHANNELS);
      expect(rec.bitDepth).toBe(PCM_BIT_DEPTH_V11);
      expect(rec.generation).toBeGreaterThanOrEqual(1);
    }
    // The only difference between the two images is the four magic bytes.
    expect(parseIndexRecord(committed).committed).toBe(true);
    expect(parseIndexRecord(uncommitted).committed).toBe(false);
  });
});
