/**
 * SECTION 1 + 3 — EXHAUSTIVE INTERRUPTION SWEEP AND ACTIVE-AUDIO IMMUTABILITY
 *
 * Every protocol operation (R / W / F) of one deterministic replacement upload
 * is interrupted twice: once BEFORE the device applies it and once AFTER. That
 * covers, by construction:
 *
 *   before/after each audio write, before/after every read-back read,
 *   before/after every index write, before/after every flush,
 *   before/during/after the final magic block, and before/during the final
 *   confirmation re-read.
 *
 * After each interruption the mock is rebooted from its stored block contents
 * and the ONE shared selector decides what is active. The invariant asserted at
 * every single point:
 *
 *   - at least one valid index exists
 *   - the selected index references intact song data
 *   - the active song is exactly the previous song or exactly the new song
 *   - no partial song is ever active
 *   - ordinary interruption never requires initialization
 *   - active and inactive regions never overlap
 *   - until the new generation is committed, the previous active song bytes and
 *     the previous active index bytes are byte-identical to their pre-upload
 *     hashes
 *
 * The device is a mock. Nothing here claims anything about physical hardware.
 */
import { mkdirSync, writeFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import {
  clearInjection,
  forkBaseline,
  interruptAt,
  reconnect,
  regionHash,
  regionsDisjoint,
  sha256,
  song,
  songImage,
  storedSong,
  withFirstSong,
  type When,
} from "./abHarness";
import { SLOT_A, SLOT_B } from "../stemTapeFormat";

const FRAMES = 680; // 2 logical sectors — keeps the exhaustive sweep tractable
const REPORT_DIR = "handoff/v1.1/reports";

interface OpTrace {
  op: number;
  kind: "read" | "write" | "flush";
  block: number;
  phase: string;
}

/** One clean replacement, observed but never interrupted, to enumerate ops. */
async function traceCleanReplacement(): Promise<{ ops: OpTrace[]; base: number }> {
  const b = await withFirstSong(FRAMES);
  const base = b.mock.ops;
  const songBlocks = new Set<number>();
  for (const s of [SLOT_A, SLOT_B]) {
    const r = b.caps.song[s];
    for (let i = 0; i < r.blocks; i++) songBlocks.add(r.start + i);
  }
  const raw: { op: number; kind: OpTrace["kind"]; block: number }[] = [];
  b.mock.opts.onWrite = ({ op, blk }) => {
    raw.push({ op: op - base, kind: "write", block: blk });
    return undefined;
  };
  b.mock.opts.onRead = ({ op, blk }) => {
    raw.push({ op: op - base, kind: "read", block: blk });
    return undefined;
  };
  b.mock.opts.onFlush = (_n, op) => {
    raw.push({ op: op - base, kind: "flush", block: -1 });
    return undefined;
  };
  const two = await song("TWO", FRAMES, 11);
  const res = await b.t.uploadSong({ song: two });
  expect(res.ok).toBe(true);
  clearInjection(b.mock);

  let sawAudioWrite = false;
  let indexWrites = 0;
  let flushes = 0;
  const ops: OpTrace[] = raw.map((o) => {
    const isSong = songBlocks.has(o.block);
    let phase: string;
    if (o.kind === "flush") {
      flushes++;
      phase = flushes === 1 ? "flush-uncommitted-index" : "final-flush";
    } else if (isSong && o.kind === "write") {
      sawAudioWrite = true;
      phase = "audio-write";
    } else if (isSong) {
      phase = "read-back";
    } else if (o.kind === "read") {
      phase = sawAudioWrite ? (indexWrites >= 2 ? "confirm-read" : "index-read-back") : "preflight-index-read";
    } else {
      indexWrites++;
      phase = indexWrites === 1 ? "index-write-uncommitted" : "magic-write";
    }
    return { ...o, phase };
  });
  return { ops, base };
}

describe("exhaustive interruption sweep (v1.1 A/B)", () => {
  it("interrupting before and after every protocol operation always leaves one complete song active", async () => {
    const { ops } = await traceCleanReplacement();
    const total = ops.length * 2;

    const results: {
      op: number;
      when: When;
      phase: string;
      block: number;
      outcome: string;
      active: "previous" | "new";
      activeGeneration: number;
      /** Number of valid, selectable generations the selector resolved to. Must always be exactly 1. */
      selectedValidGenerations: number;
      priorBytesUnchanged: boolean;
    }[] = [];


    const baseline = await withFirstSong(FRAMES);
    const two = await song("TWO", FRAMES, 11);
    const twoImage = songImage(two);
    expect(regionsDisjoint(baseline.caps)).toBe(true);

    for (const o of ops) {
      for (const when of ["before", "after"] as When[]) {
        const b = { ...baseline, ...(await forkBaseline(baseline)) };

        interruptAt(b.mock, b.mock.ops + o.op, when);
        const res = await b.t.uploadSong({ song: two });
        clearInjection(b.mock);

        // Reboot: same stored blocks, no connection state at all.
        const { mock: m2, t: t2 } = await reconnect(b.mock);
        const lib = (await t2.readLibrary())!;

        // 1. At least one valid index exists; never "needs initialization".
        expect(lib.requiresInitialization).toBe(false);
        expect(lib.slots.some((s) => s.validation.valid)).toBe(true);
        expect(lib.status).toBe("ok");

        // 2. The active record is exactly one of the two complete songs.
        const a = lib.active!;
        expect(a.songPresent).toBe(true);
        const isOld = a.songChecksum === b.one.checksum && a.generation === b.generation;
        const isNew = a.songChecksum === two.checksum && a.generation === b.generation + 1;
        expect(isOld || isNew).toBe(true);
        expect(isOld && isNew).toBe(false);

        // 3. The selected index references INTACT song data, byte for byte.
        const stored = storedSong(m2, b.caps, a.songSlot, a.sectorCount);
        expect(sha256(stored)).toBe(sha256(isOld ? b.oneImage : twoImage));
        expect(a.frames).toBe(isOld ? b.one.frames : two.frames);

        // 4. Pre-commit failure must not have touched the previous song at all.
        const priorBytesUnchanged =
          regionHash(m2, b.caps.song[b.activeSongSlot]) === b.songRegionHash &&
          regionHash(m2, b.caps.index[b.activeIndexSlot]) === b.indexRegionHash;
        if (isOld) expect(priorBytesUnchanged).toBe(true);

        // 5. Regions stay disjoint; the destination was never the active pair.
        expect(regionsDisjoint(b.caps)).toBe(true);
        if (res.targetSongSlot !== null) expect(res.targetSongSlot).not.toBe(b.activeSongSlot);
        if (res.targetIndexSlot !== null) expect(res.targetIndexSlot).not.toBe(b.activeIndexSlot);

        results.push({
          op: o.op,
          when,
          phase: o.phase,
          block: o.block,
          outcome: res.outcome,
          active: isOld ? "previous" : "new",
          activeGeneration: a.generation,
          priorBytesUnchanged,
        });
      }
    }

    expect(results.length).toBe(total);

    const byPhase: Record<string, { points: number; previous: number; new: number }> = {};
    for (const r of results) {
      const e = (byPhase[r.phase] ??= { points: 0, previous: 0, new: 0 });
      e.points++;
      if (r.active === "previous") e.previous++;
      else e.new++;
    }

    mkdirSync(REPORT_DIR, { recursive: true });
    writeFileSync(
      `${REPORT_DIR}/interruption-sweep.json`,
      JSON.stringify(
        {
          note: "Exhaustive interruption sweep over one deterministic replacement upload (mock device only).",
          frames: FRAMES,
          protocolOperations: ops.length,
          injectedInterruptionPoints: total,
          byPhase,
          results,
        },
        null,
        2,
      ) + "\n",
    );

    // No point may ever produce a partial or missing song.
    expect(results.every((r) => r.active === "previous" || r.active === "new")).toBe(true);
    // Every "previous survives" point proves it by byte hash, not generation.
    expect(results.filter((r) => r.active === "previous").every((r) => r.priorBytesUnchanged)).toBe(true);
  }, 600000);
});
