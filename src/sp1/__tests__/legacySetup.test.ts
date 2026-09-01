/**
 * v1.1 → v1.2 upgrade path.
 *
 * A device set up by an earlier firmware holds CRC-valid STIX records with
 * formatMinor 1. That is NOT corruption, and the companion must both say so and
 * offer an explicit set-up action that writes one generation-1 v1.2 record.
 */
import { describe, expect, it } from "vitest";
import { readFileSync } from "node:fs";
import { parseCapabilities } from "../compatibility";
import { readSlot, selectActiveIndex, isLegacyRecord } from "../activeIndex";
import { buildIndexRecord, blankIndexDraft } from "../stemIndex";
import { FORMAT_MINOR, SLOT_A, SLOT_B, PHYSICAL_BLOCK_BYTES } from "../stemTapeFormat";
import { MockSp1 } from "./mockSerial";
import { Sp1Session } from "../protocol";
import { Sp1Transport } from "../transport";
import { StemTapeTransport } from "../transport";

const capsBytes = new Uint8Array(readFileSync("handoff/v1.1/binaries/stcp-capability-response.bin")).slice(4);
const caps = parseCapabilities(capsBytes);
const regions = { song: caps.song, index: caps.index };

function legacyBlock(slot: 0 | 1) {
  const b = new Uint8Array(PHYSICAL_BLOCK_BYTES);
  b.set(buildIndexRecord({ ...blankIndexDraft(slot, slot, 3), formatMinor: 1 }, true));
  return b;
}

describe("older-format library diagnosis", () => {
  it("reads a v1.1 record as an earlier format, never as corruption", () => {
    const a = readSlot(SLOT_A, legacyBlock(0), regions);
    const b = readSlot(SLOT_B, legacyBlock(1), regions);
    expect(isLegacyRecord(a)).toBe(true);
    const lib = selectActiveIndex(a, b);
    expect(lib.status).toBe("legacy");
    expect(lib.requiresInitialization).toBe(true);
    expect(lib.explanation).toContain("earlier version");
    expect(lib.explanation).not.toContain("corrupt");
  });

  it("blank blocks are uninitialized, not corrupt", () => {
    const z = new Uint8Array(PHYSICAL_BLOCK_BYTES);
    const lib = selectActiveIndex(readSlot(SLOT_A, z, regions), readSlot(SLOT_B, z, regions));
    expect(lib.status).toBe("blank");
    expect(lib.explanation).not.toContain("corrupt");
  });

  it("unparseable blocks are never described as corrupt storage", () => {
    const junk = new Uint8Array(PHYSICAL_BLOCK_BYTES).fill(0x5a);
    const lib = selectActiveIndex(readSlot(SLOT_A, junk, regions), readSlot(SLOT_B, junk, regions));
    expect(lib.requiresInitialization).toBe(true);
    expect(lib.explanation).not.toContain("corrupt storage");
    expect(lib.explanation).toContain("has not been set up");
  });
});

describe("set up this SP-1", () => {
  it("writes one generation-1 v1.2 record and the device then reports generation 1", async () => {
    const mock = new MockSp1();
    // pre-load an older-format library into both index slots
    mock.writeBlockDirect(mock.caps.index[0].start, legacyBlock(0));
    mock.writeBlockDirect(mock.caps.index[1].start, legacyBlock(1));

    const session = new Sp1Session(new Sp1Transport(mock.port()));
    await session.handshake(4);
    const raw = await session.queryCapabilities();
    const t = new StemTapeTransport(session, parseCapabilities(raw!.slice(4)), { kind: "mock" });
    const before = await t.readLibrary();
    expect(before!.status).toBe("legacy");

    const out = await t.setUpLibrary();
    expect(out.library.status).toBe("ok");
    expect(out.library.generation).toBe(1);
    expect(out.library.active!.formatMinor).toBe(FORMAT_MINOR);
    expect(out.library.active!.songPresent).toBe(false);
    expect(out.library.active!.songStartBlock).toBe(0);
    expect(out.library.active!.songBlockCount).toBe(0);
    expect(out.library.active!.frames).toBe(0);
    expect(out.library.active!.sectorCount).toBe(0);
    expect(out.library.activeIndexSlot).toBe(SLOT_A);
    expect(out.reportedGeneration).toBe(1);
    expect(out.confirmed).toBe(true);
  });
});
