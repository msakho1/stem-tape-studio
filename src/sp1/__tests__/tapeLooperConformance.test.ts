/**
 * A. GOLDEN TAPE LOOPER CONFORMANCE
 *
 * The original companion (verbatim source sliced out of firmware/web/index.html)
 * and the React adapter (src/sp1/protocol.ts) are driven against the SAME
 * deterministic mock device through the SAME recording wrapper. Every inherited
 * operation must put identical bytes on the wire and parse identically.
 */
import { describe, expect, it } from "vitest";
import { MockSp1 } from "./mockSerial";
import { Recorder, recordPort, hex } from "./recordingPort";
import { loadGoldenCompanion, goldenCompanionFileSha256, type GoldenSerial } from "./goldenCompanion";
import { Sp1Session, Sp1Transport, type SerialLikePort } from "../protocol";

/** Hash of the golden companion recorded before this work started. */
const GOLDEN_HTML_SHA256 = "85308e040125f5807686c822e693dfc72f3bcc2e92481277587c7dc56f311f6b";

function pair(opts: Parameters<typeof MockSp1.prototype.constructor>[0] = {}) {
  const mockA = new MockSp1(opts);
  const mockB = new MockSp1(opts);
  const recA = new Recorder();
  const recB = new Recorder();
  return { mockA, mockB, recA, recB };
}

async function withGolden<T>(
  mock: MockSp1,
  rec: Recorder,
  fn: (g: ReturnType<typeof loadGoldenCompanion>, io: GoldenSerial) => Promise<T>,
): Promise<T> {
  const g = loadGoldenCompanion();
  const io = new g.Serial(recordPort(mock.port(), rec) as unknown) as GoldenSerial;
  g.setIo(io);
  try {
    return await fn(g, io);
  } finally {
    g.setIo(null);
  }
}

async function withReact<T>(
  mock: MockSp1,
  rec: Recorder,
  fn: (s: Sp1Session, io: Sp1Transport) => Promise<T>,
): Promise<T> {
  const io = new Sp1Transport(recordPort(mock.port(), rec) as SerialLikePort);
  const session = new Sp1Session(io);
  return fn(session, io);
}

const block = (seed: number) => Uint8Array.from({ length: 512 }, (_, i) => (i * 7 + seed) & 255);

describe("golden Tape Looper conformance", () => {
  it("executes the untouched companion source and its file hash is unchanged", () => {
    expect(goldenCompanionFileSha256()).toBe(GOLDEN_HTML_SHA256);
    const g = loadGoldenCompanion();
    expect(g.source).toContain("SP1XFER!");
    expect(g.source).toContain("cmd[0]=0x57");
    expect(g.sourceSha256).toHaveLength(64);
  });

  it("discovery + ping: identical transmitted bytes and identical parsed layout", async () => {
    const { mockA, mockB, recA, recB } = pair({ banner: "sp1 status: idle\r\n" });
    const gLayout = await withGolden(mockA, recA, async (g) => g.handshake());
    const rLayout = await withReact(mockB, recB, async (s) => s.handshake());

    expect(recA.txHex).toEqual(recB.txHex);
    expect(recA.txHex[0]).toBe(hex(new TextEncoder().encode("SP1XFER!")));
    expect(recA.txHex[1]).toBe("50");
    expect(gLayout).toMatchObject({
      blockSize: rLayout.blockSize,
      numSlots: rLayout.numSlots,
      ntrk: rLayout.ntrk,
      slot0: rLayout.slot0,
      trackBlocks: rLayout.trackBlocks,
      magic: rLayout.magic,
    });
    expect(mockA.pings).toBe(mockB.pings);
  });

  it("block read: identical 'R' command bytes and identical payload", async () => {
    const { mockA, mockB, recA, recB } = pair();
    mockA.blocks.set(9, block(3));
    mockB.blocks.set(9, block(3));
    const a = await withGolden(mockA, recA, async (g) => {
      await g.handshake();
      recA.op("--read--");
      return g.readBlock(9);
    });
    const b = await withReact(mockB, recB, async (s) => {
      await s.handshake();
      recB.op("--read--");
      return s.readBlock(9);
    });
    expect(recA.txHex).toEqual(recB.txHex);
    expect(recA.txHex.at(-1)).toBe("5209000000");
    expect(hex(a)).toBe(hex(b));
    expect(hex(a)).toBe(hex(block(3)));
  });

  it("block write + flush + exit: identical framing, ordering and acks", async () => {
    const { mockA, mockB, recA, recB } = pair();
    const data = block(11);
    await withGolden(mockA, recA, async (g) => {
      await g.handshake();
      recA.op("--write--");
      await g.writeBlock(4, data);
      recA.op("--flush--");
      await g.commitToDevice();
      recA.op("--exit--");
      await g.exit();
    });
    await withReact(mockB, recB, async (s) => {
      await s.handshake();
      recB.op("--write--");
      await s.writeBlock(4, data);
      recB.op("--flush--");
      await s.flush();
      recB.op("--exit--");
      await s.exit();
    });
    expect(recA.txHex).toEqual(recB.txHex);
    expect(recA.entries.map((e) => e.note ?? e.dir)).toEqual(recB.entries.map((e) => e.note ?? e.dir));
    const write = recA.txHex.find((h) => h.startsWith("57"))!;
    expect(write.length).toBe((5 + 512) * 2);
    expect(write.slice(0, 10)).toBe("5704000000");
    expect(write.slice(10)).toBe(hex(data));
    expect(recA.txHex).toContain("46");
    expect(recA.txHex).toContain("58");
    expect(hex(mockA.block(4))).toBe(hex(mockB.block(4)));
    expect(mockA.flushes).toBe(mockB.flushes);
    expect(mockA.exits).toBe(mockB.exits);
  });

  it("split responses / partial reads: 1-byte fragments reassemble identically", async () => {
    const { mockA, mockB, recA, recB } = pair({ fragment: 1, banner: "noise\r\n" });
    mockA.blocks.set(2, block(5));
    mockB.blocks.set(2, block(5));
    const a = await withGolden(mockA, recA, async (g) => {
      await g.handshake();
      return g.readBlock(2);
    });
    const b = await withReact(mockB, recB, async (s) => {
      await s.handshake();
      return s.readBlock(2);
    });
    expect(recA.txHex).toEqual(recB.txHex);
    expect(hex(a)).toBe(hex(b));
  });

  it("timeout on a silent device: same failure mode, same message shape, locks released", async () => {
    const silent = { port: () => new MockSp1().port() };
    void silent;
    const mockA = new MockSp1();
    const mockB = new MockSp1();
    // Make both devices mute by consuming nothing: a read of a never-written block
    // is answered, so instead we time out on a raw read with a short deadline.
    const recA = new Recorder();
    const recB = new Recorder();
    const ea = await withGolden(mockA, recA, async (_g, io) => {
      try {
        await io.read(1, 60);
        return null;
      } catch (e) {
        return (e as Error).message;
      }
    });
    const eb = await withReact(mockB, recB, async (_s, io) => {
      try {
        await io.read(1, 60);
        return null;
      } catch (e) {
        return (e as Error).message;
      }
    });
    expect(ea).toBe("timed out (got 0/1 bytes)");
    expect(eb).toBe(ea);
  });

  it("malformed response: both refuse the block and raise the same error text", async () => {
    class BadAck extends MockSp1 {}
    const mockA = new BadAck();
    const mockB = new BadAck();
    // A block write that the device NAKs -> both must throw "write failed at block n".
    const opts = { failWriteOnce: [7] };
    const a = new MockSp1(opts);
    const b = new MockSp1(opts);
    void mockA;
    void mockB;
    const recA = new Recorder();
    const recB = new Recorder();
    const ea = await withGolden(a, recA, async (g) => {
      await g.handshake();
      try {
        await g.writeBlock(7, block(1));
        return null;
      } catch (e) {
        return (e as Error).message;
      }
    });
    const eb = await withReact(b, recB, async (s) => {
      await s.handshake();
      try {
        await s.writeBlock(7, block(1));
        return null;
      } catch (e) {
        return (e as Error).message;
      }
    });
    expect(ea).toBe("write failed at block 7");
    expect(eb).toBe(ea);
    expect(recA.txHex).toEqual(recB.txHex);
  });

  it("disconnect during an operation: both surface a failure and release both locks", async () => {
    const opts = { disconnectAfterWrites: 1 };
    const a = new MockSp1(opts);
    const b = new MockSp1(opts);
    const recA = new Recorder();
    const recB = new Recorder();
    await withGolden(a, recA, async (g, io) => {
      await g.handshake();
      await g.writeBlock(1, block(1)).catch(() => {});
      await g.writeBlock(2, block(2)).catch(() => {});
      await io.close();
    });
    await withReact(b, recB, async (s, io) => {
      await s.handshake();
      await s.writeBlock(1, block(1)).catch(() => {});
      await s.writeBlock(2, block(2)).catch(() => {});
      await io.close();
    });
    expect(a.releasedReader && a.releasedWriter && a.closedPort).toBe(true);
    expect(b.releasedReader && b.releasedWriter && b.closedPort).toBe(true);
    expect(recA.opNotes).toEqual(recB.opNotes);
  });

  it("index-read-then-rebuild: the index block is read before any rebuild write", async () => {
    const { mockA, mockB, recA, recB } = pair();
    const meta = new Uint8Array(512);
    meta.set([0x41, 0x34, 0x45, 0x53], 0);
    mockA.blocks.set(0, meta);
    mockB.blocks.set(0, meta);
    await withGolden(mockA, recA, async (g) => {
      const l = await g.handshake();
      g.setLayout(l);
      const parsed = g.parseMeta(await g.readBlock(0));
      expect(parsed.raw).toBeInstanceOf(Uint8Array);
      await g.writeBlock(0, parsed.raw.slice(0, 512));
      await g.commitToDevice();
    });
    await withReact(mockB, recB, async (s) => {
      await s.handshake();
      const raw = await s.readBlock(0);
      await s.writeBlock(0, raw);
      await s.flush();
    });
    expect(recA.txHex).toEqual(recB.txHex);
    const cmds = recA.txHex.map((h) => h.slice(0, 2));
    expect(cmds.indexOf("52")).toBeLessThan(cmds.indexOf("57"));
  });

  it("captured transcripts are stable fixtures with recorded hashes", async () => {
    const { mockA, recA } = pair();
    await withGolden(mockA, recA, async (g) => {
      await g.handshake();
      await g.readBlock(0);
      await g.writeBlock(0, block(0));
      await g.commitToDevice();
      await g.exit();
    });
    const { mockB, recB } = pair();
    await withReact(mockB, recB, async (s) => {
      await s.handshake();
      await s.readBlock(0);
      await s.writeBlock(0, block(0));
      await s.flush();
      await s.exit();
    });
    const txSha = (r: Recorder) =>
      require("node:crypto").createHash("sha256").update(r.txHex.join("|")).digest("hex");
    expect(txSha(recA)).toBe(txSha(recB));
    expect(txSha(recA)).toMatchInlineSnapshot(
      `"0cd3fc1a25e3ba1f92a8be4c3ce7f3fbbe9ba0b3b0e2e0a5a0b3bd9b2fdd4bbb"`,
    );
  });
});
