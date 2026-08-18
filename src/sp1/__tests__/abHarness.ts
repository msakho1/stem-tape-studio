/**
 * Shared harness for the v1.1 A/B safety audit.
 *
 * Everything here works on the mock's STORED BLOCK CONTENTS. After every
 * injected interruption the mock is rebooted (`MockSp1.reboot()` keeps the block
 * map and throws away all connection state), so no assertion can be satisfied by
 * hidden JavaScript state.
 */
import { createHash } from "node:crypto";
import { MockSp1 } from "./mockSerial";
import { Sp1Session, Sp1Transport, type SerialLikePort } from "../protocol";
import { parseCapabilities, type StemTapeCapabilities } from "../compatibility";
import { StemTapeTransport } from "../transport";
import { prepareCanonicalSong, type CanonicalSong } from "../song";
import { encodeSong } from "../sector";
import {
  BLOCKS_PER_SECTOR,
  PHYSICAL_BLOCK_BYTES,
  SECTOR_BYTES,
  regionsOverlap,
  type AbSlot,
  type BlockRegion,
} from "../stemTapeFormat";

export const NAMES = ["vocal", "drums", "bass", "instrument"] as const;

/** Deterministic two-channel tone; integer-stable across runs. */
export function tone(frames: number, seed: number): AudioBuffer {
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

export async function song(title: string, frames: number, seed: number): Promise<CanonicalSong> {
  return prepareCanonicalSong(
    NAMES.map((name, i) => ({ name, filename: `${name}.wav`, buffer: tone(frames, i + seed) })),
    { metadata: { title, artist: "Audit", bpm: 120, downbeatSeconds: 0 } },
  );
}

export async function attach(mock: MockSp1): Promise<StemTapeTransport> {
  const io = new Sp1Transport(mock.port() as SerialLikePort);
  const session = new Sp1Session(io);
  await session.handshake();
  const caps = parseCapabilities((await session.queryCapabilities())!);
  return new StemTapeTransport(session, caps, { kind: "mock" });
}

/** Fresh connection over the same stored blocks. */
export async function reconnect(mock: MockSp1) {
  const next = mock.reboot();
  return { mock: next, t: await attach(next) };
}

/** SHA-256 over the raw stored bytes of a device region. */
export function regionHash(mock: MockSp1, r: BlockRegion): string {
  const h = createHash("sha256");
  for (let i = 0; i < r.blocks; i++) h.update(Buffer.from(mock.block(r.start + i)));
  return h.digest("hex");
}

export function sha256(bytes: Uint8Array): string {
  return createHash("sha256").update(Buffer.from(bytes)).digest("hex");
}

/** Canonical sector image of a song, exactly as the transport transmits it. */
export function songImage(s: CanonicalSong): Uint8Array {
  const sectors = encodeSong(s);
  const out = new Uint8Array(sectors.length * SECTOR_BYTES);
  sectors.forEach((sec, i) => out.set(sec, i * SECTOR_BYTES));
  return out;
}

/** Stored bytes of the first `sectors` logical sectors of a song region. */
export function storedSong(mock: MockSp1, caps: StemTapeCapabilities, slot: AbSlot, sectors: number): Uint8Array {
  const start = caps.song[slot].start;
  const out = new Uint8Array(sectors * SECTOR_BYTES);
  for (let i = 0; i < sectors * BLOCKS_PER_SECTOR; i++) {
    out.set(mock.block(start + i), i * PHYSICAL_BLOCK_BYTES);
  }
  return out;
}

export function regionsDisjoint(caps: StemTapeCapabilities): boolean {
  return (
    !regionsOverlap(caps.song[0], caps.song[1]) &&
    !regionsOverlap(caps.index[0], caps.index[1]) &&
    !regionsOverlap(caps.song[0], caps.index[0]) &&
    !regionsOverlap(caps.song[0], caps.index[1]) &&
    !regionsOverlap(caps.song[1], caps.index[0]) &&
    !regionsOverlap(caps.song[1], caps.index[1])
  );
}

export interface Baseline {
  mock: MockSp1;
  t: StemTapeTransport;
  caps: StemTapeCapabilities;
  one: CanonicalSong;
  oneImage: Uint8Array;
  generation: number;
  activeSongSlot: AbSlot;
  activeIndexSlot: AbSlot;
  songRegionHash: string;
  indexRegionHash: string;
}

/**
 * Initialised device holding song ONE as generation 2, with the active song
 * region and active index region hashed BEFORE any replacement is attempted.
 */
export async function withFirstSong(frames = 680, sectorsPerSong = 8): Promise<Baseline> {
  const mock = new MockSp1({ stemTape: true, sectorsPerSong });
  const t = await attach(mock);
  await t.initialiseLibrary();
  const one = await song("ONE", frames, 3);
  const first = await t.uploadSong({ song: one });
  if (!first.ok) throw new Error(`baseline upload failed: ${first.detail}`);
  const caps = t.caps!;
  const lib = mock.activeLibrary();
  return {
    mock,
    t,
    caps,
    one,
    oneImage: songImage(one),
    generation: first.generation,
    activeSongSlot: lib.activeSongSlot!,
    activeIndexSlot: lib.activeIndexSlot!,
    songRegionHash: regionHash(mock, caps.song[lib.activeSongSlot!]),
    indexRegionHash: regionHash(mock, caps.index[lib.activeIndexSlot!]),
  };
}

/**
 * A fresh device holding exactly the baseline's stored blocks. Used by the
 * exhaustive sweep so each of the hundreds of points starts from identical
 * storage without re-running initialization and the first upload.
 */
export async function forkBaseline(b: Baseline): Promise<{ mock: MockSp1; t: StemTapeTransport }> {
  const clean = { ...b.mock.opts };
  delete clean.onWrite;
  delete clean.onRead;
  delete clean.onFlush;
  const mock = new MockSp1(clean);
  for (const [k, v] of b.mock.blocks) mock.blocks.set(k, v.slice(0));
  return { mock, t: await attach(mock) };
}

export type When = "before" | "after";

/** Interrupt the connection exactly at protocol operation `op`. */
export function interruptAt(mock: MockSp1, op: number, when: When) {
  mock.opts.onWrite = ({ op: o }) =>
    o !== op ? undefined : when === "before" ? { apply: "none", ack: "none", disconnect: true } : { disconnect: true };
  mock.opts.onRead = ({ op: o }) =>
    o !== op ? undefined : when === "before" ? { disconnect: true } : { disconnectAfter: true };
  mock.opts.onFlush = (_n, o) =>
    o !== op ? undefined : when === "before" ? { ack: "none", disconnect: true } : { disconnect: true };
}

export function clearInjection(mock: MockSp1) {
  delete mock.opts.onWrite;
  delete mock.opts.onRead;
  delete mock.opts.onFlush;
}
