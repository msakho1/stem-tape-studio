/**
 * StemTapeDeviceTransport — the single boundary that owns every device
 * specific detail: command bytes, addressing, index serialization, sector
 * serialization, resume semantics and commit behaviour.
 *
 * No React route and no audio-preparation module may construct command bytes,
 * block addresses, index bytes or CRC records. They talk to this interface.
 *
 * Physical mutation is locked at this boundary. A transport reports
 * `mutationLocked`; when locked, every mutating call throws before any byte
 * reaches the wire. There is no production override: the only unlock is a
 * port that identifies itself as an in-process mock (`__stemTapeMock`), which
 * a real Web Serial port cannot do.
 */

import { LOCK_NOTICE, type CompatibilityVerdict } from "./compatibility";
import {
  buildMeta,
  capacity,
  blocksToSeconds,
  metaBlockCount,
  parseMeta,
  trackAudioBlocks,
  type Sp1Meta,
} from "./meta";
import { BLOCK_BYTES, type Sp1Session, type SerialLikePort } from "./protocol";
import { packInt16Blocks, type PreparedStem } from "./prepare";
import { deleteSlot, uploadSong, type UploadProgress } from "./upload";
import { type CanonicalSong } from "./song";

export interface DeviceSongSlot {
  index: number;
  occupied: boolean;
  durationSeconds: number;
  bytes: number;
  title: string | null;
  artist: string | null;
}

export interface DeviceDescription {
  deviceName: string;
  protocol: string;
  storageLayoutVersion: string;
  formatIdentifier: string;
  sampleRate: number;
  slots: number;
  usedBytes: number;
  totalBytes: number;
  perTrackSeconds: number;
  /** True when this is the legacy/provisional adapter rather than a generated one. */
  provisional: boolean;
}

export interface TransportMode {
  /** "mock" for in-process fixtures, "physical" for a real Web Serial port. */
  kind: "mock" | "physical";
}

export interface StemTapeDeviceTransport {
  readonly id: string;
  readonly provisional: boolean;
  readonly mode: TransportMode;
  readonly verdict: CompatibilityVerdict;
  /** When true every mutating method throws before touching the wire. */
  readonly mutationLocked: boolean;
  readonly lockReason: string;
  describe(): DeviceDescription;
  listSongs(): Promise<DeviceSongSlot[]>;
  initialiseLibrary(): Promise<void>;
  writeSong(args: {
    slot: number;
    song: CanonicalSong;
    signal?: { aborted: boolean };
    onProgress?: (p: UploadProgress) => void;
  }): Promise<{ ok: boolean; detail: string; durableCommitAcknowledged: boolean; independentReReadMatches: boolean }>;
  deleteSong(slot: number): Promise<void>;
  disconnect(): Promise<void>;
}

export class MutationLockedError extends Error {
  constructor(op: string, reason: string) {
    super(`${op} is locked: ${reason}`);
  }
}

export function portIsMock(port: SerialLikePort): boolean {
  return (port as SerialLikePort & { __stemTapeMock?: boolean }).__stemTapeMock === true;
}

/**
 * LEGACY / PROVISIONAL ADAPTER.
 *
 * Speaks the classic 512-byte, magic-last SP1XFER block protocol of the Tape
 * Looper. Its storage assumptions (slot count, per-track regions, index block
 * position, commit ordering) are NOT the Stem Tape firmware contract; they are
 * whatever the connected legacy device reports. It is never selected
 * automatically for a physical device and can never mutate one.
 */
export class LegacyProvisionalTransport implements StemTapeDeviceTransport {
  readonly id = "legacy-sp1xfer-block-v1 (provisional)";
  readonly provisional = true;

  constructor(
    private session: Sp1Session,
    private meta: Sp1Meta,
    readonly mode: TransportMode,
    readonly verdict: CompatibilityVerdict,
  ) {}

  get mutationLocked(): boolean {
    // Physical devices: always locked. Mock: unlocked so protocol tests run.
    return this.mode.kind !== "mock";
  }

  get lockReason(): string {
    return LOCK_NOTICE;
  }

  private guard(op: string) {
    if (this.mutationLocked) throw new MutationLockedError(op, this.lockReason);
  }

  describe(): DeviceDescription {
    const l = this.session.layout!;
    const cap = capacity(l, this.meta);
    return {
      deviceName: "legacy SP-1 (classic SP1XFER block protocol)",
      protocol: "SP1XFER block v1 — provisional adapter",
      storageLayoutVersion: "device-reported",
      formatIdentifier: `mono int16 @ ${l.sampleRate} Hz (legacy device format)`,
      sampleRate: l.sampleRate,
      slots: l.numSlots,
      usedBytes: cap.usedBlocks * l.blockSize,
      totalBytes: cap.totalBlocks * l.blockSize,
      perTrackSeconds: cap.perTrackSeconds,
      provisional: true,
    };
  }

  async refresh(): Promise<Sp1Meta> {
    const session = this.session;
    const layout = session.layout!;
    const n = metaBlockCount(layout);
    const raw = await session.lock.run(async () => {
      const b0 = await session.readBlock(0);
      if (n === 1) return b0;
      const b1 = await session.readBlock(1);
      const joined = new Uint8Array(2 * BLOCK_BYTES);
      joined.set(b0, 0);
      joined.set(b1, BLOCK_BYTES);
      return joined;
    });
    this.meta = parseMeta(raw, layout);
    return this.meta;
  }

  get indexMatchesFirmware(): boolean {
    return this.meta.magic === this.session.layout!.magic;
  }

  async listSongs(): Promise<DeviceSongSlot[]> {
    await this.refresh();
    const l = this.session.layout!;
    return this.meta.slots.map((s, index) => ({
      index,
      occupied: s.present.some((p) => !!p),
      durationSeconds: blocksToSeconds(trackAudioBlocks(s, 0), l.sampleRate),
      bytes: (s.trkLen[0] || 0) * 4 * l.blockSize,
      // The legacy index carries no title/artist fields.
      title: null,
      artist: null,
    }));
  }

  async initialiseLibrary(): Promise<void> {
    this.guard("index initialisation");
    const session = this.session;
    const layout = session.layout!;
    await session.lock.run(async () => {
      const fresh = parseMeta(new Uint8Array(metaBlockCount(layout) * BLOCK_BYTES), layout);
      const mb = buildMeta(fresh, layout);
      if (metaBlockCount(layout) === 2) await session.writeBlock(1, mb.slice(BLOCK_BYTES, 2 * BLOCK_BYTES));
      await session.writeBlock(0, mb.slice(0, BLOCK_BYTES));
      await session.flush();
    });
    await this.refresh();
  }

  /**
   * Device-sector serialization lives here and only here: the canonical song
   * is stereo 24-bit; this legacy device stores mono int16 blocks, so the
   * adapter performs that lossy reduction itself at the wire boundary.
   */
  private toLegacyStems(song: CanonicalSong): PreparedStem[] {
    return song.stems.map((s) => {
      const mono = new Float32Array(song.frames);
      for (let i = 0; i < song.frames; i++) {
        const o = i * 6;
        const l = (s.pcm24[o]! | (s.pcm24[o + 1]! << 8) | (s.pcm24[o + 2]! << 16)) << 8 >> 8;
        const r = (s.pcm24[o + 3]! | (s.pcm24[o + 4]! << 8) | (s.pcm24[o + 5]! << 16)) << 8 >> 8;
        mono[i] = (l + r) / 2 / 8388607;
      }
      const packed = packInt16Blocks(mono, song.frames);
      return {
        name: s.name,
        filename: s.filename,
        source: s.source,
        mono,
        bytes: packed.bytes,
        blocks: packed.blocks,
        peak: s.peak,
        clipped: s.clipped,
        outputBytes: packed.bytes.length,
        padSamples: s.padFrames,
      };
    });
  }

  async writeSong(args: {
    slot: number;
    song: CanonicalSong;
    signal?: { aborted: boolean };
    onProgress?: (p: UploadProgress) => void;
  }) {
    this.guard("block write / commit");
    const out = await uploadSong({
      session: this.session,
      meta: this.meta,
      slot: args.slot,
      stems: this.toLegacyStems(args.song),
      signal: args.signal,
      onProgress: args.onProgress,
    });
    return {
      ok: out.ok,
      detail: out.ok ? "Mock protocol smoke passed" : out.detail,
      // A mock acknowledgement is never hardware verification.
      durableCommitAcknowledged: false,
      independentReReadMatches: false,
    };
  }

  async deleteSong(slot: number): Promise<void> {
    this.guard("delete / replace");
    await deleteSlot(this.session, this.meta, slot);
    await this.refresh();
  }

  async disconnect(): Promise<void> {
    this.session.stopKeepalive();
    await this.session.exit();
    await this.session.io.close();
  }
}
