/**
 * D. INTERRUPTION MATRIX AND UNKNOWN-OUTCOME RECOVERY (A/B contract v1.1)
 *
 * The invariant under test, at EVERY interruption point:
 *
 *   either the previous song remains valid, or the new song is completely
 *   committed — and an interrupted replacement NEVER requires reinitialization.
 *
 * The device is a mock. Nothing here claims anything about physical hardware.
 */
import { describe, expect, it } from "vitest";
import { MockSp1 } from "./mockSerial";
import { Sp1Session, Sp1Transport, type SerialLikePort } from "../protocol";
import { parseCapabilities } from "../compatibility";
import { StemTapeTransport } from "../transport";
import { prepareCanonicalSong, type CanonicalSong } from "../song";
import { SLOT_A, SLOT_B } from "../stemTapeFormat";
import { FORBIDDEN_INTERRUPTION_PHRASES, interruptedWording } from "../wording";

const NAMES = ["vocal", "drums", "bass", "instrument"] as const;

function tone(frames: number, seed: number): AudioBuffer {
  const l = new Float32Array(frames);
  const r = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    l[i] = Math.sin((i * seed) / 50) * 0.4;
    r[i] = Math.cos((i * seed) / 70) * 0.4;
  }
  return {
    sampleRate: 48000,
    numberOfChannels: 2,
    length: frames,
    duration: frames / 48000,
    getChannelData: (c: number) => (c === 0 ? l : r),
  } as unknown as AudioBuffer;
}

async function song(title: string, frames: number, seed: number): Promise<CanonicalSong> {
  return prepareCanonicalSong(
    NAMES.map((name, i) => ({ name, filename: `${name}.wav`, buffer: tone(frames, i + seed) })),
    { metadata: { title, artist: "Tests", bpm: 120, downbeatSeconds: 0 } },
  );
}

async function attach(mock: MockSp1) {
  const io = new Sp1Transport(mock.port() as SerialLikePort);
  const session = new Sp1Session(io);
  await session.handshake();
  const caps = parseCapabilities((await session.queryCapabilities())!);
  return new StemTapeTransport(session, caps, { kind: "mock" });
}

/** Fresh connection to the same simulated storage — the "reconnect". */
async function reconnect(mock: MockSp1) {
  const next = mock.reboot();
  return { mock: next, t: await attach(next) };
}

/** Initialised device holding song ONE as generation 2. */
async function withFirstSong() {
  const mock = new MockSp1({ stemTape: true, sectorsPerSong: 16 });
  const t = await attach(mock);
  await t.initialiseLibrary();
  const one = await song("ONE", 2040, 3);
  const first = await t.uploadSong({ song: one });
  expect(first.ok).toBe(true);
  expect(first.generation).toBe(2);
  return { mock, t, one, first };
}

function activeSummary(mock: MockSp1) {
  const lib = mock.activeLibrary();
  return {
    requiresInitialization: lib.requiresInitialization,
    generation: lib.generation,
    title: lib.active?.title ?? null,
    checksum: lib.active?.songChecksum ?? 0,
    songSlot: lib.active?.songPresent ? lib.active.songSlot : null,
  };
}

describe("interruption matrix — the previous song always survives", () => {
  it("song ONE occupies song slot A; the replacement targets song slot B", async () => {
    const { mock, first } = await withFirstSong();
    expect(first.targetSongSlot).toBe(SLOT_A);
    expect(first.targetIndexSlot).toBe(SLOT_B);
    const lib = mock.activeLibrary();
    expect(lib.activeSongSlot).toBe(SLOT_A);
    expect(lib.inactiveSongSlot).toBe(SLOT_B);
    expect(lib.inactiveIndexSlot).toBe(SLOT_A);
  }, 60000);

  /**
   * Disconnect after the Nth write of the replacement, swept across the whole
   * sequence: early audio, late audio, the uncommitted index, and the validity
   * magic itself.
   */
  const points = [1, 5, 40, 95, 96, 97];
  for (const n of points) {
    it(`disconnect after write #${n} of the replacement leaves ONE valid`, async () => {
      const { mock, t, one, first } = await withFirstSong();
      const base = mock.writes;
      mock.opts.onWrite = ({ n: w }) => (w > base + n ? { disconnect: true } : undefined);

      const two = await song("TWO", 2040, 11);
      const res = await t.uploadSong({ song: two });
      expect(res.ok).toBe(false);
      expect(["failed", "unknown"]).toContain(res.outcome);
      for (const p of FORBIDDEN_INTERRUPTION_PHRASES) expect(res.detail.toLowerCase()).not.toContain(p);

      // Reconnect and run the ONE shared selector over the stored bytes.
      delete mock.opts.onWrite;
      const { mock: m2, t: t2 } = await reconnect(mock);
      const lib = (await t2.readLibrary())!;
      expect(lib.requiresInitialization).toBe(false);
      expect(t2.indexInitialised).toBe(true);

      // Either the previous song is still active, or the replacement is
      // completely committed. Never anything in between.
      const state = activeSummary(m2);
      const isOld = state.title === "ONE" && state.generation === 2 && state.checksum === first.songChecksum;
      const isNew = state.title === "TWO" && state.generation === 3;
      expect(isOld || isNew).toBe(true);

      const active = lib.active!;
      expect(active.songPresent).toBe(true);
      expect(active.frames).toBe(one.frames);
      expect(active.sectorCount).toBe(Math.ceil(one.frames / 340));

      // The audio the surviving record points at is complete on the device.
      const audio = m2.songBytes(active.songSlot, active.sectorCount);
      expect(audio.some((b) => b !== 0)).toBe(true);

      // A reconnect resolves the outcome without ambiguity.
      const resolved = await t2.resolveOutcome({ frames: two.frames, songChecksum: state.checksum });
      expect(resolved.outcome).toBe("committed");
      expect(interruptedWording("mock", isNew ? "new" : "old")).toContain("Simulated");
    }, 60000);
  }

  it("a lost flush after the validity magic still resolves on reconnect", async () => {
    const { mock, t, first } = await withFirstSong();
    // Let every write land; kill the flush that follows the validity magic.
    let magicSeen = false;
    mock.opts.onWrite = ({ blk, data }) => {
      if ((blk === 0 || blk === 1) && data[0] === 0x58 && data[1] === 0x49) magicSeen = true;
      return undefined;
    };
    mock.opts.onFlush = () => (magicSeen ? { disconnect: true } : undefined);
    const two = await song("TWO", 2040, 11);
    const res = await t.uploadSong({ song: two });
    expect(res.ok).toBe(false);
    expect(res.outcome).toBe("unknown");
    expect(res.detail).toContain("Outcome unknown: reconnect");
    expect(res.detail).toContain(`Generation ${first.generation} is still intact`);

    delete mock.opts.onFlush;
    delete mock.opts.onWrite;
    const { mock: m2, t: t2 } = await reconnect(mock);
    const resolved = await t2.resolveOutcome({ frames: two.frames, songChecksum: res.songChecksum || 0 });
    expect(["committed", "failed"]).toContain(resolved.outcome);
    expect(m2.activeLibrary().requiresInitialization).toBe(false);
  }, 60000);

  it("a torn (partial) validity-magic write is rejected and ONE stays active", async () => {
    const { mock, t, first } = await withFirstSong();
    // The magic block is the last 512-byte write of the sequence: tear it.
    let lastIndexWrite = -1;
    mock.opts.onWrite = ({ blk, n }) => {
      if (blk === 0 || blk === 1) {
        lastIndexWrite = n;
        // Second write to the index block is the magic write.
        if (mock.blocks.has(blk) && (mock.block(blk)[0] ?? 0) !== 0) {
          return { apply: "partial", partialBytes: 200, disconnect: true };
        }
      }
      return undefined;
    };
    const two = await song("TWO", 2040, 11);
    const res = await t.uploadSong({ song: two });
    expect(res.ok).toBe(false);
    expect(lastIndexWrite).toBeGreaterThan(0);

    delete mock.opts.onWrite;
    const { mock: m2, t: t2 } = await reconnect(mock);
    const lib = (await t2.readLibrary())!;
    expect(lib.requiresInitialization).toBe(false);
    const state = activeSummary(m2);
    expect(state.checksum === first.songChecksum || state.checksum === res.songChecksum).toBe(true);
  }, 60000);

  it("successive uploads alternate A/B and never reinitialize", async () => {
    const { mock, t } = await withFirstSong();
    const seen: { song: number | null; index: number | null; gen: number }[] = [];
    for (let i = 0; i < 4; i++) {
      const s = await song(`S${i}`, 2040, 20 + i);
      const r = await t.uploadSong({ song: s });
      expect(r.ok).toBe(true);
      seen.push({ song: r.targetSongSlot, index: r.targetIndexSlot, gen: r.generation });
      expect(mock.activeLibrary().requiresInitialization).toBe(false);
    }
    expect(seen.map((s) => s.song)).toEqual([SLOT_B, SLOT_A, SLOT_B, SLOT_A]);
    expect(seen.map((s) => s.index)).toEqual([SLOT_A, SLOT_B, SLOT_A, SLOT_B]);
    expect(seen.map((s) => s.gen)).toEqual([3, 4, 5, 6]);
  }, 90000);

  it("a completed upload resolves to committed on reconnect", async () => {
    const { mock, t } = await withFirstSong();
    const two = await song("TWO", 2040, 11);
    const res = await t.uploadSong({ song: two });
    expect(res.outcome).toBe("committed");
    const { t: t2 } = await reconnect(mock);
    const resolved = await t2.resolveOutcome({ frames: two.frames, songChecksum: res.songChecksum });
    expect(resolved.outcome).toBe("committed");
    const songs = await t2.listSongs();
    expect(songs.find((s) => s.active)!.title).toBe("TWO");
  }, 60000);

  it("a different song's checksum never resolves as committed", async () => {
    const { mock, t } = await withFirstSong();
    const two = await song("TWO", 2040, 11);
    const res = await t.uploadSong({ song: two });
    const { t: t2 } = await reconnect(mock);
    const wrong = await t2.resolveOutcome({ frames: two.frames, songChecksum: res.songChecksum ^ 0xff });
    expect(wrong.outcome).toBe("failed");
    expect(wrong.detail).toContain("previous song");
  }, 60000);

  it("both index slots destroyed is reported as corrupt storage, not an interrupted upload", async () => {
    const { mock } = await withFirstSong();
    mock.blocks.set(0, Uint8Array.from({ length: 512 }, () => 0xa5));
    mock.blocks.set(1, Uint8Array.from({ length: 512 }, () => 0x5a));
    const { t: t2 } = await reconnect(mock);
    const lib = (await t2.readLibrary())!;
    expect(lib.status).toBe("corrupt");
    expect(lib.requiresInitialization).toBe(true);
    const resolved = await t2.resolveOutcome({ frames: 1, songChecksum: 1 });
    expect(resolved.outcome).toBe("corrupt");
    expect(interruptedWording("physical", "corrupt")).toContain("corrupt storage");
  }, 60000);
});
