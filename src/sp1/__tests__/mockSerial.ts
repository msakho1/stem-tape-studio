/**
 * Mock SP-1 serial device implementing the real block protocol, used by the
 * focused tests and by the browser smoke check.
 *
 * The Stem Tape v1.1 model here stores TWO song regions and TWO index regions
 * as plain 512-byte blocks. There is deliberately NO hidden "active song"
 * variable: `activeLibrary()` parses the stored index blocks and runs the same
 * shared selector the companion uses, so the mock can never mask a selection
 * bug. Interruption injection (torn writes, lost acknowledgements, flush
 * failures, CRC corruption) is modelled at the block level, below the format.
 */
import { put32, type SerialLikePort } from "../protocol";
import {
  BLOCKS_PER_SECTOR,
  CAPS_BYTES,
  CAP_FLAG,
  FORMAT_MAJOR,
  FORMAT_MINOR,
  PHYSICAL_BLOCK_BYTES,
  PROTOCOL_MAJOR,
  PROTOCOL_MINOR,
  REQUIRED_ALIGNMENT,
  REQUIRED_CAP_FLAGS,
  SECTOR_BYTES,
  SLOT_A,
  SLOT_B,
  STEM_TAPE_FIRMWARE_ID,
  STIX_VERSION,
  type AbSlot,
} from "../stemTapeFormat";
import { serializeCapabilities, type StemTapeCapabilities } from "../compatibility";
import { readSlot, selectActiveIndex, type LibraryState } from "../activeIndex";
import {
  BULK_CMD,
  BULK_PAYLOAD_BYTES,
  BULK_REQ_HEADER_BYTES,
  BULK_STATUS,
  BULK_PROTO_VERSION,
  buildBulkCaps,
  buildBulkResponse,
  parseBulkHeader,
} from "../bulkTransfer";
import { crc32 } from "../crc32";

/** What the mock does with one incoming 'W' block write. */
export interface WriteAction {
  /** How much of the payload reaches storage. */
  apply?: "full" | "none" | "partial";
  /** Bytes applied when apply === "partial". */
  partialBytes?: number;
  /** Acknowledgement behaviour: 'w', a NAK byte, or silence. */
  ack?: "ok" | "nak" | "none";
  /** Drop the connection after handling this write. */
  disconnect?: boolean;
  /** Corrupt the bytes that reach storage (models a failed flash program). */
  mangle?: (data: Uint8Array) => Uint8Array;
}

export interface FlushAction {
  ack?: "ok" | "nak" | "none";
  disconnect?: boolean;
}

/** What the mock does with one incoming 'U' bulk verified-sector write. */
export interface BulkAction {
  /** Force this status instead of performing the real write+read-back. */
  status?: number;
  /** Withhold the 14-byte response entirely (models a lost acknowledgement). */
  ack?: "ok" | "none";
  /** Corrupt what actually lands in storage, so the read-back CRC disagrees. */
  mangle?: (data: Uint8Array) => Uint8Array;
  /** Drop the connection after handling this request. */
  disconnect?: boolean;
  /** Drop the connection before handling this request at all. */
  disconnectBefore?: boolean;
}

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
  /** Logical 8 KiB sectors reserved per song slot (A and B alike). */
  sectorsPerSong?: number;
  /** Override reported versions to model incompatible firmware. */
  protoMinor?: number;
  formatMinor?: number;
  stixVersion?: number;
  /** Override the reported A/B geometry (used by region-validation tests). */
  geometry?: Partial<Pick<StemTapeCapabilities, "song" | "index" | "deviceBlocks" | "alignment" | "sectorBytes">>;
  /** Per-write interruption injection. */
  onWrite?: (ctx: { n: number; blk: number; op: number; data: Uint8Array }) => WriteAction | undefined;
  /** Per-flush interruption injection. */
  onFlush?: (n: number, op: number) => FlushAction | undefined;
  /** Per-read interruption injection. */
  onRead?: (ctx: { n: number; blk: number; op: number }) => { disconnect?: boolean; disconnectAfter?: boolean; corrupt?: boolean } | undefined;
  /** Advertise the "STBC" bulk verified-sector extension on the 'Q' reply. */
  bulk?: boolean;
  /** Per-bulk-sector interruption injection. */
  onBulk?: (ctx: { n: number; seq: number; destBlock: number; crc: number }) => BulkAction | undefined;
}

export class MockSp1 {
  blocks = new Map<number, Uint8Array>();
  writes = 0;
  reads = 0;
  /** Monotonic counter over every R/W/F protocol operation. */
  ops = 0;
  pings = 0;
  capQueries = 0;
  /** Bulk verified-sector round trips actually handled. */
  bulkWrites = 0;
  /** Session state the 'U' command is gated on: opened by 'Q', closed by 'X'. */
  private sessionOpen = false;
  private expectedSeq = 0;
  private lastBulkResponse: Uint8Array | null = null;
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
  readonly opts: Required<
    Pick<MockOptions, "numSlots" | "ntrk" | "slot0" | "trackBlocks" | "blockSize" | "magic" | "fragment" | "stemTape" | "capFlags" | "sectorsPerSong">
  > &
    MockOptions;

  constructor(o: MockOptions = {}) {
    this.opts = {
      ...o,
      numSlots: o.numSlots ?? 8,
      ntrk: o.ntrk ?? 4,
      slot0: o.slot0 ?? 16,
      trackBlocks: o.trackBlocks ?? 64,
      blockSize: o.blockSize ?? 512,
      magic: o.magic ?? 0x53453441, // 'SE4A' -> 48 kHz
      fragment: o.fragment ?? 0,
      stemTape: o.stemTape ?? false,
      capFlags: o.capFlags ?? REQUIRED_CAP_FLAGS | CAP_FLAG.STAGING_COW,
      sectorsPerSong: o.sectorsPerSong ?? 8,
    };
    if (this.opts.banner) this.push(new TextEncoder().encode(this.opts.banner));
  }

  /* ---------- Stem Tape v1.1 A/B geometry ---------- */

  /**
   * Fixed, device-reported layout:
   *   index A  block 0            (1 block)
   *   index B  block 1            (1 block)
   *   song  A  block 16           (sectorsPerSong * 16 blocks)
   *   song  B  block 16 + A       (sectorsPerSong * 16 blocks)
   */
  get capabilities(): StemTapeCapabilities {
    const songBlocks = this.opts.sectorsPerSong * BLOCKS_PER_SECTOR;
    const songAStart = 16;
    const songBStart = songAStart + songBlocks;
    const base: StemTapeCapabilities = {
      firmwareId: STEM_TAPE_FIRMWARE_ID,
      protoMajor: PROTOCOL_MAJOR,
      protoMinor: this.opts.protoMinor ?? PROTOCOL_MINOR,
      formatMajor: FORMAT_MAJOR,
      formatMinor: this.opts.formatMinor ?? FORMAT_MINOR,
      flags: this.opts.capFlags,
      sampleRate: 48000,
      blockSize: PHYSICAL_BLOCK_BYTES,
      sectorBytes: SECTOR_BYTES,
      alignment: REQUIRED_ALIGNMENT,
      deviceBlocks: songBStart + songBlocks,
      song: [
        { start: songAStart, blocks: songBlocks },
        { start: songBStart, blocks: songBlocks },
      ],
      index: [
        { start: 0, blocks: 1 },
        { start: 1, blocks: 1 },
      ],
      activeIndexSlot: 0,
      activeSongSlot: 0,
      activeGeneration: 0,
      stixVersion: this.opts.stixVersion ?? STIX_VERSION,
    };
    const merged = { ...base, ...this.opts.geometry } as StemTapeCapabilities;
    // The advisory active pointers are derived from the stored records, never
    // from a hidden variable.
    const lib = this.parseLibrary(merged);
    merged.activeIndexSlot = lib.activeIndexSlot ?? 0xffffffff;
    merged.activeSongSlot = lib.activeSongSlot ?? 0xffffffff;
    merged.activeGeneration = lib.generation;
    return merged;
  }

  private parseLibrary(caps: StemTapeCapabilities): LibraryState {
    const regions = { song: caps.song, index: caps.index };
    return selectActiveIndex(
      readSlot(SLOT_A, this.block(caps.index[0].start), regions),
      readSlot(SLOT_B, this.block(caps.index[1].start), regions),
    );
  }

  /** The mock's own view of which song is active — computed, never stored. */
  activeLibrary(): LibraryState {
    return this.parseLibrary(this.capabilities);
  }

  /** Bytes of one song region as currently stored. */
  songBytes(slot: AbSlot, sectors: number): Uint8Array {
    const caps = this.capabilities;
    const start = caps.song[slot].start;
    const out = new Uint8Array(sectors * SECTOR_BYTES);
    for (let i = 0; i < sectors * BLOCKS_PER_SECTOR; i++) {
      out.set(this.block(start + i), i * PHYSICAL_BLOCK_BYTES);
    }
    return out;
  }

  /** Simulated power-cycle / rescan: same storage, fresh connection state. */
  reboot(): MockSp1 {
    const clean: MockOptions = { ...this.opts };
    delete clean.onWrite;
    delete clean.onFlush;
    delete clean.onRead;
    delete clean.disconnectAfterWrites;
    delete clean.failWriteOnce;
    const next = new MockSp1(clean);
    next.blocks = this.blocks;
    return next;
  }

  /* ---------- wire ---------- */

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
        this.reads++;
        this.ops++;
        const act = this.opts.onRead?.({ n: this.reads, blk, op: this.ops });
        if (act?.disconnect) {
          this.disconnected = true;
          this.wake?.();
          return;
        }
        const payload = this.block(blk).slice(0);
        if (act?.corrupt) payload[0] = payload[0]! ^ 0xff;
        const out = new Uint8Array(513);
        out[0] = 0x72;
        out.set(payload, 1);
        this.push(out);
        if (act?.disconnectAfter) {
          this.disconnected = true;
          this.wake?.();
          return;
        }
        continue;
      }
      if (cmd === 0x57) {
        if (b.length < 5 + 512) return;
        const blk = b[1]! | (b[2]! << 8) | (b[3]! << 16) | b[4]! * 0x1000000;
        const data = b.slice(5, 5 + 512);
        this.inbox = b.slice(5 + 512);
        this.writes++;
        this.ops++;
        if (this.opts.disconnectAfterWrites && this.writes > this.opts.disconnectAfterWrites) {
          this.disconnected = true;
          this.wake?.();
          return;
        }
        if (this.opts.failWriteOnce?.includes(blk) && !this.failed.has(blk)) {
          this.failed.add(blk);
          this.push(new Uint8Array([0x21])); // NAK: not 'w'
          continue;
        }
        const act = this.opts.onWrite?.({ n: this.writes, blk, op: this.ops, data }) ?? {};
        const apply = act.apply ?? "full";
        const eff = act.mangle ? act.mangle(data.slice(0)) : data;
        if (apply === "full") {
          this.blocks.set(blk, eff);
        } else if (apply === "partial") {
          const cur = this.block(blk).slice(0);
          const n = Math.max(0, Math.min(512, act.partialBytes ?? 256));
          cur.set(eff.slice(0, n), 0);
          this.blocks.set(blk, cur);
        }
        const ack = act.ack ?? "ok";
        if (ack === "ok") this.push(new Uint8Array([0x77]));
        else if (ack === "nak") this.push(new Uint8Array([0x21]));
        if (act.disconnect) {
          this.disconnected = true;
          this.wake?.();
          return;
        }
        continue;
      }
      if (cmd === 0x51) {
        this.inbox = b.slice(1);
        this.capQueries++;
        if (!this.opts.stemTape) continue; // stock firmware: no reply at all
        const caps = serializeCapabilities(this.capabilities);
        const bulk = this.opts.bulk === true;
        const reply = new Uint8Array(4 + CAPS_BYTES + (bulk ? 12 : 0));
        reply.set(new TextEncoder().encode("STCP"));
        reply.set(caps, 4);
        if (bulk) reply.set(buildBulkCaps(), 4 + CAPS_BYTES);
        // 'Q' (re)opens the write session and resets the bulk sequence to 0.
        this.sessionOpen = bulk;
        this.expectedSeq = 0;
        this.lastBulkResponse = null;
        this.push(reply);
        continue;
      }
      if (cmd === 0x46) {
        this.flushes++;
        this.ops++;
        this.inbox = b.slice(1);
        const act = this.opts.onFlush?.(this.flushes, this.ops) ?? {};
        const ack = act.ack ?? "ok";
        if (ack === "ok") this.push(new Uint8Array([0x66]));
        else if (ack === "nak") this.push(new Uint8Array([0x21]));
        if (act.disconnect) {
          this.disconnected = true;
          this.wake?.();
          return;
        }
        continue;
      }
      if (cmd === BULK_CMD) {
        if (b.length < 1 + BULK_REQ_HEADER_BYTES) return;
        const header = parseBulkHeader(b.slice(1, 1 + BULK_REQ_HEADER_BYTES));
        const declared = header.payloadLen;
        // Only a well-formed length can tell us where this request ends.
        const payloadLen = declared === BULK_PAYLOAD_BYTES ? BULK_PAYLOAD_BYTES : 0;
        if (b.length < 1 + BULK_REQ_HEADER_BYTES + payloadLen) return;
        const payload = b.slice(1 + BULK_REQ_HEADER_BYTES, 1 + BULK_REQ_HEADER_BYTES + payloadLen);
        this.inbox = b.slice(1 + BULK_REQ_HEADER_BYTES + payloadLen);
        this.bulkWrites++;
        this.ops++;
        const act =
          this.opts.onBulk?.({ n: this.bulkWrites, seq: header.seq, destBlock: header.destBlock, crc: header.payloadCrc32 }) ?? {};
        if (act.disconnectBefore) {
          this.disconnected = true;
          this.wake?.();
          return;
        }
        const answer = (status: number, verified: number) => {
          const resp = buildBulkResponse(status, header.seq, header.destBlock, verified);
          this.lastBulkResponse = status === BULK_STATUS.OK ? resp : this.lastBulkResponse;
          if ((act.ack ?? "ok") === "ok") this.push(resp);
          if (act.disconnect) {
            this.disconnected = true;
            this.wake?.();
          }
        };
        if (act.status !== undefined) {
          answer(act.status, 0);
          if (act.disconnect) return;
          continue;
        }
        if (header.version !== BULK_PROTO_VERSION) {
          answer(BULK_STATUS.UNSUPPORTED_VERSION, 0);
          continue;
        }
        if (declared !== BULK_PAYLOAD_BYTES) {
          answer(BULK_STATUS.BAD_LENGTH, 0);
          continue;
        }
        if (!this.sessionOpen) {
          answer(BULK_STATUS.NO_SESSION, 0);
          continue;
        }
        if (crc32(payload) !== header.payloadCrc32) {
          answer(BULK_STATUS.CRC_MISMATCH, 0);
          continue;
        }
        // A retry of the immediately preceding accepted sector replays the
        // byte-identical response and never advances the sequence.
        if (header.seq === this.expectedSeq - 1 && this.lastBulkResponse) {
          if ((act.ack ?? "ok") === "ok") this.push(this.lastBulkResponse.slice(0));
          if (act.disconnect) {
            this.disconnected = true;
            this.wake?.();
            return;
          }
          continue;
        }
        if (header.seq !== this.expectedSeq) {
          answer(BULK_STATUS.OUT_OF_SEQUENCE, 0);
          continue;
        }
        const caps2 = this.capabilities;
        const lib2 = this.parseLibrary(caps2);
        const inactive = lib2.inactiveSongSlot;
        const region = caps2.song[inactive];
        const expectDest = region.start + header.seq * BLOCKS_PER_SECTOR;
        if (header.destBlock !== expectDest) {
          const activeRegion = lib2.activeSongSlot === null ? null : caps2.song[lib2.activeSongSlot];
          if (
            activeRegion &&
            header.destBlock >= activeRegion.start &&
            header.destBlock < activeRegion.start + activeRegion.blocks
          ) {
            answer(BULK_STATUS.ACTIVE_REGION, 0);
          } else if (header.destBlock < region.start || header.destBlock >= region.start + region.blocks) {
            answer(BULK_STATUS.OUT_OF_BOUNDS, 0);
          } else {
            answer(BULK_STATUS.DEST_MISMATCH, 0);
          }
          continue;
        }
        if (header.destBlock + BLOCKS_PER_SECTOR > region.start + region.blocks) {
          answer(BULK_STATUS.OUT_OF_BOUNDS, 0);
          continue;
        }
        const stored = act.mangle ? act.mangle(payload.slice(0)) : payload;
        for (let i = 0; i < BLOCKS_PER_SECTOR; i++) {
          this.blocks.set(header.destBlock + i, stored.slice(i * PHYSICAL_BLOCK_BYTES, (i + 1) * PHYSICAL_BLOCK_BYTES));
        }
        this.writes += BLOCKS_PER_SECTOR;
        // Real read-back off storage, then a real CRC of those bytes.
        const back = new Uint8Array(BULK_PAYLOAD_BYTES);
        for (let i = 0; i < BLOCKS_PER_SECTOR; i++) {
          back.set(this.block(header.destBlock + i), i * PHYSICAL_BLOCK_BYTES);
        }
        const verified = crc32(back);
        if (verified !== header.payloadCrc32) {
          answer(BULK_STATUS.READBACK_CRC_MISMATCH, verified);
          continue;
        }
        this.expectedSeq = header.seq + 1;
        answer(BULK_STATUS.OK, verified);
        if (act.disconnect) return;
        continue;
      }
      if (cmd === 0x58) {
        this.exits++;
        this.sessionOpen = false;
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
