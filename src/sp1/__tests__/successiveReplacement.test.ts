/**
 * SECTION 4 — SUCCESSIVE A/B REPLACEMENT AND ROLLBACK
 *
 * Four successive replacements are performed on one simulated device. For each
 * one the destination song slot, destination index slot, generation, song-region
 * SHA-256, index SHA-256, the generation selected after a reboot and the
 * surviving rollback generation are recorded. The newest index is then corrupted
 * and selection must fall back to the immediately previous complete song — whose
 * audio must still be byte-exact.
 */
import { mkdirSync, writeFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import {
  reconnect,
  sha256,
  song,
  songImage,
  storedSong,
  withFirstSong,
} from "./abHarness";
import { IX_OFF, SLOT_A, SLOT_B, sectorsForFrames, slotName } from "../stemTapeFormat";
import type { CanonicalSong } from "../song";

const FRAMES = 680;
const REPORT_DIR = "handoff/v1.1/reports";

describe("successive A/B replacement", () => {
  it("four successive songs alternate slots, and a corrupt newest index rolls back", async () => {
    const base = await withFirstSong(FRAMES);
    interface Row {
      step: number;
      title: string;
      songSlot: "A" | "B";
      indexSlot: "A" | "B";
      generation: number;
      songRegionSha256: string;
      indexSha256: string;
      selectedGenerationAfterReboot: number;
      rollbackGeneration: number;
    }
    const rows: Row[] = [];
    let previous: { song: CanonicalSong; generation: number } = { song: base.one, generation: base.generation };
    let mock = base.mock;
    let t = base.t;

    rows.push({
      step: 0,
      title: "ONE (baseline)",
      songSlot: slotName(base.activeSongSlot),
      indexSlot: slotName(base.activeIndexSlot),
      generation: base.generation,
      songRegionSha256: sha256(storedSong(mock, base.caps, base.activeSongSlot, sectorsForFrames(base.one.frames))),
      indexSha256: sha256(mock.block(base.caps.index[base.activeIndexSlot].start)),
      selectedGenerationAfterReboot: base.generation,
      rollbackGeneration: 1,
    });

    let last!: { song: CanonicalSong; slot: 0 | 1; index: 0 | 1; generation: number };
    for (let i = 1; i <= 4; i++) {
      const s = await song(`S${i}`, FRAMES, 20 + i);
      const r = await t.uploadSong({ song: s });
      expect(r.ok).toBe(true);

      const { mock: m2, t: t2 } = await reconnect(mock);
      const lib = (await t2.readLibrary())!;
      expect(lib.requiresInitialization).toBe(false);
      expect(lib.generation).toBe(r.generation);
      expect(lib.activeIndexSlot).toBe(r.targetIndexSlot);
      expect(lib.activeSongSlot).toBe(r.targetSongSlot);

      // Both generations are simultaneously valid: previous is the rollback.
      const other = lib.slots.find((x) => x.slot !== lib.activeIndexSlot)!;
      expect(other.validation.valid).toBe(true);
      expect(other.record.generation).toBe(previous.generation);

      rows.push({
        step: i,
        title: s.metadata.title,
        songSlot: slotName(r.targetSongSlot!),
        indexSlot: slotName(r.targetIndexSlot!),
        generation: r.generation,
        songRegionSha256: sha256(storedSong(m2, base.caps, r.targetSongSlot!, sectorsForFrames(s.frames))),
        indexSha256: sha256(m2.block(base.caps.index[r.targetIndexSlot!].start)),
        selectedGenerationAfterReboot: lib.generation,
        rollbackGeneration: other.record.generation,
      });

      previous = { song: s, generation: r.generation };
      last = { song: s, slot: r.targetSongSlot!, index: r.targetIndexSlot!, generation: r.generation };
      mock = m2;
      t = t2;
      // The song bytes on the device match the canonical image exactly.
      expect(sha256(storedSong(m2, base.caps, r.targetSongSlot!, sectorsForFrames(s.frames)))).toBe(sha256(songImage(s)));
    }

    expect(rows.slice(1).map((r) => r.songSlot)).toEqual(["B", "A", "B", "A"]);
    expect(rows.slice(1).map((r) => r.indexSlot)).toEqual(["A", "B", "A", "B"]);
    expect(rows.slice(1).map((r) => r.generation)).toEqual([3, 4, 5, 6]);

    /* ---------- rollback: corrupt the NEWEST index record ---------- */
    const previousSong = await song("S3", FRAMES, 23); // generation 5, the rollback copy
    const blk = base.caps.index[last.index].start;
    const corrupted = mock.block(blk).slice(0);
    corrupted[IX_OFF.crc32] = corrupted[IX_OFF.crc32]! ^ 0xff;
    mock.blocks.set(blk, corrupted);

    const { mock: m3, t: t3 } = await reconnect(mock);
    const lib = (await t3.readLibrary())!;
    expect(lib.requiresInitialization).toBe(false);
    expect(lib.status).toBe("ok");
    expect(lib.activeIndexSlot).not.toBe(last.index);
    expect(lib.generation).toBe(last.generation - 1);
    const active = lib.active!;
    expect(active.songChecksum).toBe(previousSong.checksum);
    expect(sha256(storedSong(m3, base.caps, active.songSlot, active.sectorCount))).toBe(sha256(songImage(previousSong)));

    const rollback = {
      corruptedIndexSlot: slotName(last.index),
      corruptedGeneration: last.generation,
      selectedIndexSlot: slotName(lib.activeIndexSlot!),
      selectedGeneration: lib.generation,
      selectedSongSlot: slotName(active.songSlot),
      selectedSongSha256: sha256(storedSong(m3, base.caps, active.songSlot, active.sectorCount)),
      requiresInitialization: lib.requiresInitialization,
      explanation: lib.explanation,
    };

    mkdirSync(REPORT_DIR, { recursive: true });
    writeFileSync(
      `${REPORT_DIR}/successive-replacement.json`,
      JSON.stringify({ uploads: rows, rollback }, null, 2) + "\n",
    );
    expect([SLOT_A, SLOT_B]).toContain(lib.activeIndexSlot);
  }, 180000);
});
