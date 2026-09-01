/**
 * v1.1 → v1.2 upgrade path.
 *
 * A device set up by an earlier firmware holds CRC-valid STIX records with
 * formatMinor 1. That is NOT corruption, and the companion must both say so and
 * offer an explicit set-up action that writes one generation-1 v1.2 record.
 */
import { describe, expect, it } from "vitest";
import { parseCapabilities } from "../compatibility";
import { readSlot, selectActiveIndex, isLegacyRecord } from "../activeIndex";
import { buildIndexRecord, blankIndexDraft } from "../stemIndex";
import { FORMAT_MINOR, PHYSICAL_BLOCK_BYTES, SLOT_A, SLOT_B, type AbSlot } from "../stemTapeFormat";
import { MockSp1 } from "./mockSerial";
import { Sp1Session, Sp1Transport, type SerialLikePort } from "../protocol";
import { StemTapeTransport } from "../transport";

function legacyBlock(slot: AbSlot) {
  const b = new Uint8Array(PHYSICAL_BLOCK_BYTES);
  b.set(buildIndexRecord({ ...blankIndexDraft(slot, slot, 3), formatMinor: 1 }, true));
  return b;
}

async function attach(mock: MockSp1) {
  const session = new Sp1Session(new Sp1Transport(mock.port() as SerialLikePort));
  await session.handshake();
  const caps = parseCapabilities((await session.queryCapabilities())!);
  return { caps, t: new StemTapeTransport(session, caps, { kind: "mock" }) };
}

describe("older-format library diagnosis", () => {
  const mock = new MockSp1({ stemTape: true });
  const regions = { song: mock.capabilities.song, index: mock.capabilities.index };

  it("reads a v1.1 record as an earlier format, never as corruption", () => {
    const a = readSlot(SLOT_A, legacyBlock(SLOT_A), regions);
    const b = readSlot(SLOT_B, legacyBlock(SLOT_B), regions);
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
    const mock = new MockSp1({ stemTape: true });
    mock.blocks.set(mock.capabilities.index[0].start, legacyBlock(SLOT_A));
    mock.blocks.set(mock.capabilities.index[1].start, legacyBlock(SLOT_B));

    const { t } = await attach(mock);
    const before = await t.readLibrary();
    expect(before!.status).toBe("legacy");

    const out = await t.setUpLibrary();
    const rec = out.library.active!;
    expect(out.library.status).toBe("ok");
    expect(out.library.generation).toBe(1);
    expect(out.library.activeIndexSlot).toBe(SLOT_A);
    expect(rec.formatMinor).toBe(FORMAT_MINOR);
    expect(rec.songPresent).toBe(false);
    expect([rec.songStartBlock, rec.songBlockCount, rec.frames, rec.sectorCount]).toEqual([0, 0, 0, 0]);
    expect(out.reportedGeneration).toBe(1);
    expect(out.confirmed).toBe(true);
  }, 30000);
});
