/**
 * SP-1 Web Serial transfer protocol — companion uploader transport.
 *
 * Source of truth: the proven SP-1 transfer implementation
 * (chattock/sp1-tape-looper @ a4982c9, docs/index.html). The repository does
 * NOT contain docs/stem-tape-transfer-v1.md, so nothing here is invented:
 * every command byte, reply tag, block size, offset and timeout below is
 * copied from that implementation.
 *
 * Wire contract (block protocol, CDC @ 115200 8N1):
 *   connect magic  "SP1XFER!"            -> enters transfer mode
 *   'P' 0x50       ping                  -> "SP1!" + 24-byte layout
 *   'R' 0x52 + u32 read  block           -> 'r' 0x72 + 512 bytes
 *   'W' 0x57 + u32 + 512 write block     -> 'w' 0x77
 *   'F' 0x46       flush cache to NAND   -> 'f' 0x66
 *   'X' 0x58       exit (also flushes)   -> 1 byte
 *
 * There is no CRC, no staging area, no per-chunk checksum, no resume offset
 * report and no title/artist/BPM field in this protocol. Verification here is
 * therefore a real read-back byte compare over 'R'.
 */

export const BAUD_RATE = 115200;
export const SAMPLES_PER_BLOCK = 256;
export const BLOCK_BYTES = 512;
export const CONNECT_MAGIC = new Uint8Array([0x53, 0x50, 0x31, 0x58, 0x46, 0x45, 0x52, 0x21]); // "SP1XFER!"
export const REPLY_TAG = "SP1!";

export const CMD = {
  PING: 0x50,
  READ: 0x52,
  WRITE: 0x57,
  FLUSH: 0x46,
  EXIT: 0x58,
} as const;
export const ACK = { READ: 0x72, WRITE: 0x77, FLUSH: 0x66 } as const;

/** 'SE24' / 'SE2A' index magics mean a 24 kHz device; every other known magic is 48 kHz. */
export function rateFromMagic(magic: number): number {
  return magic === 0x53453234 || magic === 0x53453241 ? 24000 : 48000;
}

export interface Sp1Layout {
  blockSize: number;
  numSlots: number;
  ntrk: number;
  slot0: number;
  trackBlocks: number;
  magic: number;
  /** Derived from the index magic — the device's native sample rate. */
  sampleRate: number;
}

export function le32(a: Uint8Array, off: number): number {
  return ((a[off]! | (a[off + 1]! << 8) | (a[off + 2]! << 16)) >>> 0) + a[off + 3]! * 0x1000000;
}
export function put32(a: Uint8Array, off: number, v: number): void {
  a[off] = v & 255;
  a[off + 1] = (v >>> 8) & 255;
  a[off + 2] = (v >>> 16) & 255;
  a[off + 3] = (v >>> 24) & 255;
}

export function parsePing(info: Uint8Array): Sp1Layout {
  const magic = le32(info, 20);
  return {
    blockSize: le32(info, 0),
    numSlots: le32(info, 4),
    ntrk: le32(info, 8),
    slot0: le32(info, 12),
    trackBlocks: le32(info, 16),
    magic,
    sampleRate: rateFromMagic(magic),
  };
}

/** Minimal structural typing so tests can drive a mock port with no DOM. */
export interface SerialLikePort {
  readable: { getReader(): { read(): Promise<{ value?: Uint8Array | undefined; done: boolean }>; cancel(): Promise<void>; releaseLock(): void } };
  writable: { getWriter(): { write(v: Uint8Array): Promise<void>; releaseLock(): void } };
  open(options: { baudRate: number }): Promise<void>;
  close(): Promise<void>;
  setSignals?(s: { dataTerminalReady: boolean; requestToSend: boolean }): Promise<void>;
}

/**
 * Byte transport. A background pump drains the stream into one buffer so
 * fragmented CDC reads reassemble, and read() waits against a wall-clock
 * deadline rather than blocking on reader.read() forever.
 */
export class Sp1Transport {
  private reader: ReturnType<SerialLikePort["readable"]["getReader"]>;
  private writer: ReturnType<SerialLikePort["writable"]["getWriter"]>;
  private buf = new Uint8Array(0);
  closed = false;
  rxTotal = 0;

  constructor(private port: SerialLikePort) {
    this.reader = port.readable.getReader();
    this.writer = port.writable.getWriter();
    void this.pump();
  }

  private async pump() {
    try {
      while (!this.closed) {
        const { value, done } = await this.reader.read();
        if (done) break;
        if (value && value.length) {
          const merged = new Uint8Array(this.buf.length + value.length);
          merged.set(this.buf);
          merged.set(value, this.buf.length);
          this.buf = merged;
          this.rxTotal += value.length;
        }
      }
    } catch {
      /* reader cancelled on close */
    }
  }

  async write(bytes: Uint8Array): Promise<void> {
    await this.writer.write(bytes);
  }

  /** Discard stale/banner bytes printed before the handshake. */
  drain(): void {
    this.buf = new Uint8Array(0);
  }

  async read(n: number, timeoutMs = 4000): Promise<Uint8Array> {
    const deadline = Date.now() + timeoutMs;
    while (this.buf.length < n) {
      if (this.closed) throw new Error("port closed");
      if (Date.now() > deadline) throw new Error(`timed out (got ${this.buf.length}/${n} bytes)`);
      await new Promise((r) => setTimeout(r, 4));
    }
    const out = this.buf.slice(0, n);
    this.buf = this.buf.slice(n);
    return out;
  }

  /** Always releases both locks, then closes the port — every failure path. */
  async close(): Promise<void> {
    this.closed = true;
    try {
      await this.reader.cancel();
    } catch { /* ignore */ }
    try {
      this.reader.releaseLock();
    } catch { /* ignore */ }
    try {
      this.writer.releaseLock();
    } catch { /* ignore */ }
    try {
      await this.port.close();
    } catch { /* ignore */ }
  }
}

/**
 * One command queue for the single shared port: keepalive pings can never
 * interleave with a block read or write.
 */
export class CommandLock {
  private tail: Promise<unknown> = Promise.resolve();
  run<T>(fn: () => Promise<T>): Promise<T> {
    const next = this.tail.then(fn, fn) as Promise<T>;
    this.tail = next.catch(() => {});
    return next;
  }
}

export class Sp1Session {
  readonly lock = new CommandLock();
  layout: Sp1Layout | null = null;
  private keepalive: ReturnType<typeof setInterval> | null = null;

  constructor(public io: Sp1Transport) {}

  /** Skip status-line noise until the 4-byte "SP1!" reply tag appears. */
  private async readUntilTag(timeoutMs: number): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    const win: number[] = [];
    while (Date.now() < deadline) {
      const b = (await this.io.read(1, Math.max(50, deadline - Date.now())))[0]!;
      win.push(b);
      if (win.length > 4) win.shift();
      if (win.length === 4 && String.fromCharCode(...win) === REPLY_TAG) return;
    }
    throw new Error("timed out waiting for the SP-1 reply");
  }

  /** Enter transfer mode and read the layout, tolerating noise and retrying. */
  async handshake(attempts = 40, onAttempt?: (n: number, err?: string) => void): Promise<Sp1Layout> {
    for (let attempt = 1; attempt <= attempts; attempt++) {
      this.io.drain();
      await this.io.write(CONNECT_MAGIC);
      await new Promise((r) => setTimeout(r, 120));
      await this.io.write(new Uint8Array([CMD.PING]));
      try {
        await this.readUntilTag(1200);
        const layout = parsePing(await this.io.read(24, 1200));
        assertCompatible(layout);
        this.layout = layout;
        return layout;
      } catch (e) {
        const msg = e instanceof Error ? e.message : String(e);
        if (msg.startsWith("incompatible")) throw e; // never retry a version refusal
        onAttempt?.(attempt, msg);
      }
    }
    throw new Error("the SP-1 never sent its reply");
  }

  async readBlock(blk: number): Promise<Uint8Array> {
    const cmd = new Uint8Array(5);
    cmd[0] = CMD.READ;
    put32(cmd, 1, blk);
    await this.io.write(cmd);
    const h = await this.io.read(1);
    if (h[0] !== ACK.READ) throw new Error(`read failed at block ${blk}`);
    return this.io.read(BLOCK_BYTES);
  }

  async writeBlock(blk: number, data512: Uint8Array): Promise<void> {
    const cmd = new Uint8Array(5 + BLOCK_BYTES);
    cmd[0] = CMD.WRITE;
    put32(cmd, 1, blk);
    cmd.set(data512, 5);
    await this.io.write(cmd);
    const h = await this.io.read(1, 9000);
    if (h[0] !== ACK.WRITE) throw new Error(`write failed at block ${blk}`);
  }

  /** 'F' — flush the write cache to NAND so an upload survives unplug. */
  async flush(): Promise<void> {
    await this.io.write(new Uint8Array([CMD.FLUSH]));
    const h = await this.io.read(1, 15000);
    if (h[0] !== ACK.FLUSH) throw new Error("the device didn't confirm the save");
  }

  /** 'X' — clean exit; also flushes. Never throws. */
  async exit(): Promise<void> {
    try {
      await this.io.write(new Uint8Array([CMD.EXIT]));
      await this.io.read(1, 15000);
    } catch { /* ignore */ }
  }

  trackBlock(slot: number, track: number): number {
    const l = this.layout!;
    return l.slot0 + (slot * l.ntrk + track) * l.trackBlocks;
  }

  startKeepalive(periodMs = 7000): void {
    this.stopKeepalive();
    this.keepalive = setInterval(() => {
      void this.lock.run(async () => {
        try {
          await this.io.write(new Uint8Array([CMD.PING]));
          await new Promise((r) => setTimeout(r, 80));
          this.io.drain();
        } catch { /* ignore */ }
      });
    }, periodMs);
  }
  stopKeepalive(): void {
    if (this.keepalive) clearInterval(this.keepalive);
    this.keepalive = null;
  }

  /**
   * Stem Tape capability query ('Q'). This command does NOT exist in the Tape
   * Looper protocol: stock firmware simply never answers, which is exactly the
   * fail-closed signal the compatibility gate needs. Returns the raw
   * capability payload following the "STCP" tag, or null on silence.
   */
  async queryCapabilities(timeoutMs = 700): Promise<Uint8Array | null> {
    return this.lock.run(async () => {
      try {
        this.io.drain();
        await this.io.write(new Uint8Array([CMD_CAPS]));
        const deadline = Date.now() + timeoutMs;
        const win: number[] = [];
        while (Date.now() < deadline) {
          const b = (await this.io.read(1, Math.max(40, deadline - Date.now())))[0]!;
          win.push(b);
          if (win.length > 4) win.shift();
          if (win.length === 4 && String.fromCharCode(...win) === CAPS_TAG) {
            return await this.io.read(CAPS_BYTES, 1000);
          }
        }
        return null;
      } catch {
        return null;
      }
    });
  }
}


/**
 * Compatibility refusal. The ping layout is the only version signal this
 * protocol carries — there is no separate firmware/protocol version field.
 */
export function assertCompatible(layout: Sp1Layout): void {
  if (layout.blockSize !== BLOCK_BYTES) {
    throw new Error(
      `incompatible firmware: this SP-1 reports ${layout.blockSize}-byte blocks, this uploader only speaks the 512-byte block protocol.`,
    );
  }
  if (layout.ntrk !== 4) {
    throw new Error(
      `incompatible firmware: this SP-1 reports ${layout.ntrk} tracks per song; the four-stem uploader requires exactly 4.`,
    );
  }
  if (layout.numSlots < 1 || layout.numSlots > 64 || layout.trackBlocks < 1) {
    throw new Error(
      `incompatible firmware: implausible layout (${layout.numSlots} slots, ${layout.trackBlocks} blocks per track).`,
    );
  }
}
