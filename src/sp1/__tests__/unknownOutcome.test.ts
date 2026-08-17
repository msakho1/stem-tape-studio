/**
 * D. INTERRUPTION AND UNKNOWN-OUTCOME RECOVERY
 *
 * The device is disconnected at three distinct points and each resulting
 * outcome is asserted, together with what a reconnect can and cannot resolve.
 * Nothing here claims anything about physical hardware: the device is a mock.
 */
import { describe, expect, it } from "vitest";
import { MockSp1 } from "./mockSerial";
import { Sp1Session, Sp1Transport, type SerialLikePort } from "../protocol";
import { parseCapabilities } from "../compatibility";
import { StemTapeTransport } from "../transport";
import { prepareCanonicalSong, type CanonicalSong } from "../song";
import { indexIsValid, parseStemIndex } from "../stemIndex";
import { FORMAT_MAJOR, PHYSICAL_BLOCK_BYTES, indexBlockCount } from "../stemTapeFormat";

const NAMES = ["vocal", "drums", "bass", "instrument"] as const;
const META = { title: "Interrupted", artist: "Tests", bpm: 120, downbeatSeconds: 0 };

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

async function song(frames = 2040): Promise<CanonicalSong> {
  return prepareCanonicalSong(
    NAMES.map((name, i) => ({ name, filename: `${name}.wav`, buffer: tone(frames, i + 3) })),
    { metadata: META },
  );
}

async function attach(mock: MockSp1) {
  const io = new Sp1Transport(mock.port() as SerialLikePort);
  const session = new Sp1Session(io);
  await session.handshake();
  const caps = parseCapabilities((await session.queryCapabilities())!);
  return new StemTapeTransport(session, caps, { kind: "mock" });
}

/** A fresh connection to the same simulated storage — the "reconnect". */
async function reconnect(mock: MockSp1) {
  const next = new MockSp1({ stemTape: true, sectorsPerSong: mock.opts.sectorsPerSong });
  next.blocks = mock.blocks;
  return { mock: next, t: await attach(next) };
}

function rawIndex(mock: MockSp1, slots: number) {
  const n = indexBlockCount(slots);
  const raw = new Uint8Array(n * PHYSICAL_BLOCK_BYTES);
  for (let i = 0; i < n; i++) raw.set(mock.block(i), i * PHYSICAL_BLOCK_BYTES);
  return parseStemIndex(raw, slots);
}

async function setup() {
  const mock = new MockSp1({ stemTape: true, sectorsPerSong: 16 });
  const t = await attach(mock);
  await t.initialiseLibrary();
  return { mock, t, base: mock.writes };
}

describe("interruption and unknown-outcome recovery", () => {
  it("1. disconnect mid-audio: failed, previous index intact, nothing playable", async () => {
    const s = await song();
    const { mock, t, base } = await setup();
    const before = rawIndex(mock, 8)!;
    mock.opts.disconnectAfterWrites = base + 20; // deep inside the audio blocks
    const res = await t.uploadSong({ slot: 1, song: s });

    expect(res.ok).toBe(false);
    expect(res.outcome).toBe("failed");
    expect(res.detail).toContain("no validity magic was sent");
    expect(t.magicAttempted).toBe(false);
    const after = rawIndex(mock, 8)!;
    expect(indexIsValid(after, FORMAT_MAJOR)).toBe(true);
    expect(after.generation).toBe(before.generation);
    expect(after.songs[1]!.committed).toBe(false);
  }, 30000);

  it("2. disconnect while the index metadata is written (magic not yet sent): failed", async () => {
    const s = await song();
    const { mock, t, base } = await setup();
    const audioBlocks = 6 * 16; // 2040 frames -> 6 sectors
    mock.opts.disconnectAfterWrites = base + audioBlocks + 1; // during the metadata blocks
    const res = await t.uploadSong({ slot: 1, song: s });

    expect(res.ok).toBe(false);
    expect(res.outcome).toBe("failed");
    expect(t.magicAttempted).toBe(false);
    expect(res.writtenBlocks).toBe(audioBlocks);
    expect(rawIndex(mock, 8)!.songs[1]!.committed).toBe(false);
  }, 30000);

  it("3. disconnect at the validity-magic write: unknown, and a reconnect must resolve it", async () => {
    const s = await song();
    const { mock, t, base } = await setup();
    const audioBlocks = 6 * 16;
    const idxBlocks = indexBlockCount(8);
    // audio + (idxBlocks-1 continuation) + block0-without-magic, then the magic write dies.
    mock.opts.disconnectAfterWrites = base + audioBlocks + idxBlocks;
    const res = await t.uploadSong({ slot: 1, song: s });

    expect(res.outcome).toBe("unknown");
    expect(t.magicAttempted).toBe(true);
    expect(res.detail).toContain("Outcome unknown: reconnect to verify");

    const { t: t2 } = await reconnect(mock);
    const resolved = await t2.resolveOutcome({ slot: 1, frames: s.frames, songChecksum: res.songChecksum || 0 });
    // The magic never landed, so the index image on the device is not valid and
    // a reconnect cannot upgrade the outcome to committed. It stays unknown and
    // the library must be re-initialised — this is the non-staging failure mode.
    expect(resolved).toBe("unknown");
    expect(indexIsValid(rawIndex(mock, 8)!, FORMAT_MAJOR)).toBe(false);
    expect(t2.indexInitialised).toBe(false);
  }, 30000);

  it("4. a completed upload resolves to committed on reconnect", async () => {
    const s = await song();
    const { mock, t } = await setup();
    const res = await t.uploadSong({ slot: 2, song: s });
    expect(res.outcome).toBe("committed");

    const { t: t2 } = await reconnect(mock);
    const resolved = await t2.resolveOutcome({ slot: 2, frames: s.frames, songChecksum: res.songChecksum });
    expect(resolved).toBe("committed");
    expect((await t2.listSongs())[2]!.occupied).toBe(true);
  }, 30000);

  it("5. a different song's checksum never resolves as committed", async () => {
    const s = await song();
    const { mock, t } = await setup();
    const res = await t.uploadSong({ slot: 3, song: s });
    const { t: t2 } = await reconnect(mock);
    expect(await t2.resolveOutcome({ slot: 3, frames: s.frames, songChecksum: res.songChecksum ^ 0xff })).toBe("failed");
    expect(await t2.resolveOutcome({ slot: 4, frames: s.frames, songChecksum: res.songChecksum })).toBe("failed");
  }, 30000);
});
