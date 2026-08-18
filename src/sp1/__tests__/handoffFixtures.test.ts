/**
 * SECTION 6 — THE COMMITTED HANDOFF BUNDLE IS CONSUMED, NOT RECREATED
 *
 * These tests read the version-controlled files in handoff/v1.1/ and push them
 * through the production parsers. No expected byte table is rebuilt here: if a
 * production offset changes, the committed fixtures stop parsing and this fails.
 */
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { parseCapabilities } from "../compatibility";
import { parseIndexRecord, validateIndexRecord } from "../stemIndex";
import { readSlot, selectActiveIndex } from "../activeIndex";
import { decodeSectors } from "../sector";
import { checksum32 } from "../song";
import { crc32 } from "../crc32";
import {
  INDEX_MAGIC,
  IX_OFF,
  SECTOR_BYTES,
  SLOT_A,
  SLOT_B,
  STIX_VERSION,
} from "../stemTapeFormat";

const DIR = "handoff/v1.1";
const bin = (n: string) => new Uint8Array(readFileSync(`${DIR}/binaries/${n}`));
const json = (n: string) => JSON.parse(readFileSync(`${DIR}/decoded/${n}`, "utf8"));
const transcript = (n: string) => JSON.parse(readFileSync(`${DIR}/transcripts/${n}`, "utf8"));

const capsDecoded = json("stcp-capability-response.json").decoded;
const regions = { song: capsDecoded.song, index: capsDecoded.index };

describe("frozen v1.1 handoff bundle", () => {
  it("every listed file matches the committed SHA-256 manifest", () => {
    const lines = readFileSync(`${DIR}/SHA256SUMS.txt`, "utf8").trim().split("\n");
    expect(lines.length).toBeGreaterThanOrEqual(20);
    for (const line of lines) {
      const [want, path] = line.split(/\s{2,}/);
      const got = createHash("sha256").update(readFileSync(path!)).digest("hex");
      expect(`${path}:${got}`).toBe(`${path}:${want}`);
    }
  });

  it("every listed file matches the committed CRC-32 manifest", () => {
    const lines = readFileSync(`${DIR}/CRC32SUMS.txt`, "utf8").trim().split("\n");
    for (const line of lines) {
      const [want, path] = line.split(/\s{2,}/);
      expect(crc32(new Uint8Array(readFileSync(path!))).toString(16).padStart(8, "0")).toBe(want);
    }
  });

  it("the STCP capability binary parses to the decoded JSON", () => {
    const raw = bin("stcp-capability-response.bin");
    expect(new TextDecoder().decode(raw.subarray(0, 4))).toBe("STCP");
    const parsed = parseCapabilities(raw.subarray(4));
    expect(parsed).not.toBeNull();
    expect(JSON.parse(JSON.stringify(parsed))).toEqual(capsDecoded);
    expect(parsed!.stixVersion).toBe(STIX_VERSION);
  });

  it("the initialized-storage fixture holds one valid generation-1 record and a blank B", () => {
    const img = bin("storage-initialized-empty.bin");
    const a = img.subarray(0, 512);
    const b = img.subarray(512, 1024);
    const lib = selectActiveIndex(readSlot(SLOT_A, a, regions), readSlot(SLOT_B, b, regions));
    expect(lib.requiresInitialization).toBe(false);
    expect(lib.activeIndexSlot).toBe(SLOT_A);
    expect(lib.generation).toBe(1);
    expect(lib.active!.songPresent).toBe(false);
    expect(b.every((x) => x === 0)).toBe(true);
  });

  it("index A and index B fixtures are both valid and select the newer generation", () => {
    const a = bin("index-a-valid.bin");
    const b = bin("index-b-valid.bin");
    const ra = readSlot(SLOT_A, a, regions);
    const rb = readSlot(SLOT_B, b, regions);
    expect(ra.validation.valid).toBe(true);
    expect(rb.validation.valid).toBe(true);
    const lib = selectActiveIndex(ra, rb);
    expect(lib.generation).toBe(Math.max(ra.record.generation, rb.record.generation));
    expect(lib.requiresInitialization).toBe(false);
  });

  it("the uncommitted fixture is never selectable and the magic block differs only in bytes 0..4", () => {
    const un = bin("index-uncommitted.bin");
    const magic = bin("index-final-magic-block.bin");
    const slot = json("index-uncommitted.json").decoded.record.slotIdentity as 0 | 1;

    expect(parseIndexRecord(un).committed).toBe(false);
    expect(validateIndexRecord(parseIndexRecord(un), slot, regions).valid).toBe(false);

    const rec = parseIndexRecord(magic);
    expect(rec.committed).toBe(true);
    expect(validateIndexRecord(rec, slot, regions).valid).toBe(true);
    expect(rec.crc).toBe(rec.crcComputed);

    const diff: number[] = [];
    for (let i = 0; i < un.length; i++) if (un[i] !== magic[i]) diff.push(i);
    expect(diff).toEqual([0, 1, 2, 3]);
    expect(
      magic[IX_OFF.magic]! |
        (magic[IX_OFF.magic + 1]! << 8) |
        (magic[IX_OFF.magic + 2]! << 16) |
        (magic[IX_OFF.magic + 3]! * 0x1000000),
    ).toBe(INDEX_MAGIC);
  });

  it("the four-stem song sectors decode to the checksums recorded in the decoded JSON", () => {
    const img = bin("song-sectors-four-stem.bin");
    const meta = json("song-sectors-four-stem.json");
    expect(img.length).toBe(meta.sectorCount * SECTOR_BYTES);
    const sectors: Uint8Array[] = [];
    for (let i = 0; i < meta.sectorCount; i++) sectors.push(img.subarray(i * SECTOR_BYTES, (i + 1) * SECTOR_BYTES));
    const decoded = decodeSectors(sectors, meta.frames);
    const sums = decoded.stems.map((s) => checksum32(s));
    expect(sums).toEqual(meta.stems.map((s: { checksum: number }) => s.checksum));
  });

  it("the committed transcripts record the guaranteed outcomes", () => {
    expect(transcript("upload-1-successful.json").result.outcome).toBe("committed");
    expect(transcript("upload-2-successful.json").result.outcome).toBe("committed");
    expect(transcript("upload-1-successful.json").result.indexSlot).not.toBe(
      transcript("upload-2-successful.json").result.indexSlot,
    );

    const before = transcript("interrupted-before-magic.json");
    expect(before.uploadResult.outcome).toBe("failed");
    expect(before.afterReconnect.requiresInitialization).toBe(false);
    expect(before.afterReconnect.title).toBe("HANDOFF ONE");

    const ackLost = transcript("magic-applied-ack-lost.json");
    expect(ackLost.afterReconnect.title).toBe("HANDOFF TWO");
    expect(ackLost.afterReconnect.requiresInitialization).toBe(false);

    const torn = transcript("torn-invalid-magic.json");
    expect(torn.afterReconnect.title).toBe("HANDOFF ONE");
    expect(torn.afterReconnect.requiresInitialization).toBe(false);

    const fallback = transcript("corrupt-newest-index-fallback.json");
    expect(fallback.selected.requiresInitialization).toBe(false);
    expect(fallback.selected.title).toBe("HANDOFF ONE");
    expect(fallback.selected.activeIndexSlot).not.toBe(fallback.corruptedIndexSlot);
  });
});
