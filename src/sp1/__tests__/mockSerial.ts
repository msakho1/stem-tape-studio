/**
 * Mock SP-1 serial device implementing the real block protocol, used by the
 * focused tests and by the browser smoke check.
 */
import { put32, type SerialLikePort } from "../protocol";
import {
  CAP_FLAG,
  CAPS_BYTES,
  CAPS_OFF,
  FORMAT_MAJOR,
  PROTOCOL_MAJOR,
  REQUIRED_CAP_FLAGS,
  STEM_TAPE_FIRMWARE_ID,
  indexBlockCount,
} from "../stemTapeFormat";

export interface MockOptions {
  numSlots?: number;
  ntrk?: number;
  slot0?: number;
  trackBlocks?: number;
  blockSize?: number;
  magic?: number;
  /** Emit this stale text before the handshake reply. */
  banner?: string;
  /** Deliver replies in n-byte fragments. */
  fragment?: number;
  /** Block indices whose FIRST write attempt is NAKed. */
  failWriteOnce?: number[];
  /** Drop the connection after this many writes. */
  disconnectAfterWrites?: number;
  /** Answer the Stem Tape 'Q' capability query. Stock devices stay silent. */
  stemTape?: boolean;
  /** Override the reported capability flags (defaults to the required set). */
  capFlags?: number;
  /** Logical 8 KiB sectors reserved per song slot. */
  sectorsPerSong?: number;
}

export class MockSp1 {
  blocks = new Map<number, Uint8Array>();
  writes = 0;
  pings = 0;
  capQueries = 0;
  flushes = 0;
  exits = 0;
  closedPort = false;
  releasedReader = false;
  releasedWriter = false;
  transferMode = false;
  private out: Uint8Array[] = [];
  private wake: (() => void) | null = null;
  private inbox = new Uint8Array(0);
  private failed = new Set<number>();
  private disconnected = false;
  readonly opts: {
    numSlots: number;
    ntrk: number;
    slot0: number;
    trackBlocks: number;
    blockSize: number;
    magic: number;
    fragment: number;
    banner: string | undefined;
    failWriteOnce: number[] | undefined;
    disconnectAfterWrites: number | undefined;
    stemTape: boolean;
    capFlags: number;
    sectorsPerSong: number;
  };

  constructor(o: MockOptions = {}) {
    this.opts = {
      numSlots: o.numSlots ?? 8,
      ntrk: o.ntrk ?? 4,
      slot0: o.slot0 ?? 16,
      trackBlocks: o.trackBlocks ?? 64,
      blockSize: o.blockSize ?? 512,
      magic: o.magic ?? 0x53453441, // 'SE4A' -> 48 kHz
      fragment: o.fragment ?? 0,
      banner: o.banner,
      failWriteOnce: o.failWriteOnce,
      disconnectAfterWrites: o.disconnectAfterWrites,
      stemTape: o.stemTape ?? false,
      capFlags: o.capFlags ?? REQUIRED_CAP_FLAGS | CAP_FLAG.STAGING_COW,
      sectorsPerSong: o.sectorsPerSong ?? 8,
    };
    if (this.opts.banner) this.push(new TextEncoder().encode(this.opts.banner));
  }

  private push(bytes: Uint8Array) {
    const frag = this.opts.fragment;
    if (frag > 0) {
      for (let i = 0; i < bytes.length; i += frag) this.out.push(bytes.slice(i, i + frag));
    } else {
      this.out.push(bytes);
    }
    this.wake?.();
  }

  block(n: number): Uint8Array {
    return this.blocks.get(n) ?? new Uint8Array(512);
  }

  private handle(bytes: Uint8Array) {
    const merged = new Uint8Array(this.inbox.length + bytes.length);
    merged.set(this.inbox);
    merged.set(bytes, this.inbox.length);
    this.inbox = merged;

    for (;;) {
      const b = this.inbox;
      if (!b.length) return;
      if (b.length >= 8 && String.fromCharCode(...b.slice(0, 8)) === "SP1XFER!") {
        this.transferMode = true;
        this.inbox = b.slice(8);
        continue;
      }
      const cmd = b[0]!;
      if (cmd === 0x50) {
        this.pings++;
        this.inbox = b.slice(1);
        const info = new Uint8Array(24);
        put32(info, 0, this.opts.blockSize);
        put32(info, 4, this.opts.numSlots);
        put32(info, 8, this.opts.ntrk);
        put32(info, 12, this.opts.slot0);
        put32(info, 16, this.opts.trackBlocks);
        put32(info, 20, this.opts.magic);
        const reply = new Uint8Array(4 + 24);
        reply.set(new TextEncoder().encode("SP1!"));
        reply.set(info, 4);
        this.push(reply);
        continue;
      }
      if (cmd === 0x52) {
        if (b.length < 5) return;
        const blk = b[1]! | (b[2]! << 8) | (b[3]! << 16) | b[4]! * 0x1000000;
        this.inbox = b.slice(5);
        const out = new Uint8Array(513);
        out[0] = 0x72;
        out.set(this.block(blk), 1);
        this.push(out);
        continue;
      }
      if (cmd === 0x57) {
        if (b.length < 5 + 512) return;
        const blk = b[1]! | (b[2]! << 8) | (b[3]! << 16) | b[4]! * 0x1000000;
        const data = b.slice(5, 5 + 512);
        this.inbox = b.slice(5 + 512);
        this.writes++;
        if (this.opts.disconnectAfterWrites && this.writes > this.opts.disconnectAfterWrites) {
          this.disconnected = true;
          return;
        }
        if (this.opts.failWriteOnce?.includes(blk) && !this.failed.has(blk)) {
          this.failed.add(blk);
          this.push(new Uint8Array([0x21])); // NAK: not 'w'
          continue;
        }
        this.blocks.set(blk, data);
        this.push(new Uint8Array([0x77]));
        continue;
      }
      if (cmd === 0x51) {
        this.inbox = b.slice(1);
        this.capQueries++;
        if (!this.opts.stemTape) continue; // stock firmware: no reply at all
        const caps = new Uint8Array(CAPS_BYTES);
        put32(caps, CAPS_OFF.firmwareId, STEM_TAPE_FIRMWARE_ID);
        caps[CAPS_OFF.protoMajor] = PROTOCOL_MAJOR;
        caps[CAPS_OFF.formatMajor] = FORMAT_MAJOR;
        put32(caps, CAPS_OFF.flags, this.opts.capFlags);
        put32(caps, CAPS_OFF.sampleRate, 48000);
        put32(caps, CAPS_OFF.songSlots, this.opts.numSlots);
        put32(caps, CAPS_OFF.indexBlocks, indexBlockCount(this.opts.numSlots));
        put32(caps, CAPS_OFF.libraryBase, this.opts.slot0);
        put32(caps, CAPS_OFF.sectorsPerSong, this.opts.sectorsPerSong);
        put32(caps, CAPS_OFF.generation, 0);
        const reply = new Uint8Array(4 + CAPS_BYTES);
        reply.set(new TextEncoder().encode("STCP"));
        reply.set(caps, 4);
        this.push(reply);
        continue;
      }
      if (cmd === 0x46) {
        this.flushes++;
        this.inbox = b.slice(1);
        this.push(new Uint8Array([0x66]));
        continue;
      }
      if (cmd === 0x58) {
        this.exits++;
        this.transferMode = false;
        this.inbox = b.slice(1);
        this.push(new Uint8Array([0x78]));
        continue;
      }
      this.inbox = b.slice(1);
    }
  }

  port(): SerialLikePort {
    const self = this;
    return {
      readable: {
        getReader() {
          return {
            async read() {
              for (;;) {
                if (self.out.length) return { value: self.out.shift()!, done: false as boolean };
                if (self.disconnected) return { value: undefined, done: true as boolean };
                await new Promise<void>((r) => {
                  self.wake = r;
                  setTimeout(r, 5);
                });
              }
            },
            async cancel() {
              self.disconnected = true;
              self.wake?.();
            },
            releaseLock() {
              self.releasedReader = true;
            },
          };
        },
      },
      writable: {
        getWriter() {
          return {
            async write(v: Uint8Array) {
              if (self.disconnected) throw new Error("device disconnected");
              self.handle(v);
            },
            releaseLock() {
              self.releasedWriter = true;
            },
          };
        },
      },
      async open() {},
      async close() {
        self.closedPort = true;
      },
      async setSignals() {},
    };
  }
}
