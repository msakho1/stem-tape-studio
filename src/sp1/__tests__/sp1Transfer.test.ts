import { describe, it, expect, vi } from "vitest";
import { MockSp1 } from "./mockSerial";
import { Sp1Transport, Sp1Session, assertCompatible, parsePing, rateFromMagic, put32 } from "../protocol";
import { parseMeta, buildMeta, capacity, blocksToSeconds } from "../meta";
import {
  downmixToMono,
  packInt16Blocks,
  prepareFourStems,
  validatePackage,
  peakOf,
  bpmFromTaps,
  type PrepareInput,
} from "../prepare";
import { uploadSong, findResumeBlock, checksum32, equalBytes, deleteSlot } from "../upload";

/* ---------- helpers ---------- */

async function connect(mock: MockSp1) {
  const io = new Sp1Transport(mock.port());
  const session = new Sp1Session(io);
  await session.handshake(4);
  return { io, session };
}

function fakeBuffer(frames: number, channels: number, sampleRate: number, fill: (i: number, c: number) => number) {
  const data: Float32Array[] = [];
  for (let c = 0; c < channels; c++) {
    const arr = new Float32Array(frames);
    for (let i = 0; i < frames; i++) arr[i] = fill(i, c);
    data.push(arr);
  }
  return {
    sampleRate,
    numberOfChannels: channels,
    length: frames,
    duration: frames / sampleRate,
    getChannelData: (c: number) => data[c]!,
  } as unknown as AudioBuffer;
}

async function preparedFour(frames = 700, rate = 48000) {
  const inputs: PrepareInput[] = (["vocal", "drums", "bass", "instrument"] as const).map((name, k) => ({
    name,
    filename: `${name}.wav`,
    buffer: fakeBuffer(frames - k * 50, 2, rate, (i) => Math.sin(i / 20) * 0.5),
  }));
  return prepareFourStems(inputs, { deviceRate: rate, maxBlocks: 64 });
}

/* ---------- protocol ---------- */

describe("SP-1 handshake", () => {
  it("completes past stale banner text and reports the layout", async () => {
    const mock = new MockSp1({ banner: "sp1 idle status line\r\nrec 0\r\n" });
    const { io, session } = await connect(mock);
    expect(session.layout).toMatchObject({ blockSize: 512, numSlots: 8, ntrk: 4, trackBlocks: 64 });
    expect(session.layout!.sampleRate).toBe(48000);
    await io.close();
  });

  it("reassembles fragmented serial reads (1 byte at a time)", async () => {
    const mock = new MockSp1({ fragment: 1, banner: "noise" });
    const { io, session } = await connect(mock);
    expect(session.layout!.numSlots).toBe(8);
    await io.close();
  });

  it("derives 24 kHz from the SE24 index magic", () => {
    expect(rateFromMagic(0x53453234)).toBe(24000);
    expect(rateFromMagic(0x53453441)).toBe(48000);
  });

  it("rejects incompatible firmware with a precise message and does not retry", async () => {
    const mock = new MockSp1({ blockSize: 4096 });
    const io = new Sp1Transport(mock.port());
    const session = new Sp1Session(io);
    await expect(session.handshake(40)).rejects.toThrow(/incompatible firmware.*4096-byte blocks/);
    await io.close();
  });

  it("refuses a device that does not report 4 tracks per song", () => {
    expect(() => assertCompatible({ ...parsePing(new Uint8Array(24)), blockSize: 512, ntrk: 2, numSlots: 8, trackBlocks: 8 })).toThrow(
      /requires exactly 4/,
    );
  });

  it("times out cleanly on a silent device and still releases both locks", async () => {
    const mock = new MockSp1();
    // Swallow every command: no replies.
    const port = mock.port();
    const silent = { ...port, writable: { getWriter: () => ({ async write() {}, releaseLock() { mock.releasedWriter = true; } }) } };
    const io = new Sp1Transport(silent);
    const session = new Sp1Session(io);
    await expect(session.handshake(1)).rejects.toThrow(/never sent its reply/);
    await io.close();
    expect(mock.releasedReader).toBe(true);
    expect(mock.releasedWriter).toBe(true);
    expect(mock.closedPort).toBe(true);
  });
});

describe("command serialization", () => {
  it("keepalive pings never interleave with a block exchange", async () => {
    vi.useFakeTimers();
    const mock = new MockSp1();
    const io = new Sp1Transport(mock.port());
    const session = new Sp1Session(io);
    vi.useRealTimers();
    await session.handshake(4);

    let inFlight = 0;
    let overlaps = 0;
    const guarded = async () => {
      inFlight++;
      if (inFlight > 1) overlaps++;
      await new Promise((r) => setTimeout(r, 5));
      inFlight--;
    };
    session.startKeepalive(1);
    await Promise.all(Array.from({ length: 12 }, () => session.lock.run(guarded)));
    session.stopKeepalive();
    expect(overlaps).toBe(0);
    await io.close();
  });
});

/* ---------- audio preparation ---------- */

describe("audio preparation", () => {
  it("duplicates mono and downmixes multichannel with equal weights", () => {
    const mono = downmixToMono([Float32Array.from([0.5, -0.5])]);
    expect(Array.from(mono)).toEqual([0.5, -0.5]);
    const stereo = downmixToMono([Float32Array.from([1, 0]), Float32Array.from([0, 1])]);
    expect(Array.from(stereo)).toEqual([0.5, 0.5]);
    const five = downmixToMono(Array.from({ length: 5 }, () => Float32Array.from([1])));
    expect(five[0]).toBeCloseTo(1, 6);
  });

  it("packs int16 LE into whole 512-byte blocks with silent padding", () => {
    const { bytes, blocks } = packInt16Blocks(Float32Array.from([1, -1, 0]));
    expect(blocks).toBe(1);
    expect(bytes.length).toBe(512);
    const v = new Int16Array(bytes.buffer);
    expect(v[0]).toBe(32767);
    expect(v[1]).toBe(-32767);
    expect(v[2]).toBe(0);
    expect(v[255]).toBe(0);
  });

  it("zero-pads unequal stems to the longest and reports the spread", async () => {
    const res = await preparedFour();
    const lens = res.stems.map((s) => s.bytes.length);
    expect(new Set(lens).size).toBe(1);
    expect(res.lengthSpreadSeconds).toBeGreaterThan(0);
    expect(res.stems[3]!.padSamples).toBeGreaterThan(0);
    // Padding is digital silence, not repeated audio.
    const v = new Int16Array(res.stems[3]!.bytes.buffer);
    expect(v[res.stems[3]!.mono.length]).toBe(0);
  });

  it("validates the package by decoding known frames back", async () => {
    const res = await preparedFour();
    expect(validatePackage(res)).toMatchObject({ ok: true });
    res.stems[0]!.bytes[0] = 0xff;
    expect(validatePackage(res).ok).toBe(false);
  });

  it("flags clipping and never normalizes", async () => {
    const res = await prepareFourStems(
      [{ name: "vocal", filename: "hot.wav", buffer: fakeBuffer(300, 1, 48000, () => 1) }],
      { deviceRate: 48000, maxBlocks: 64 },
    );
    expect(res.stems[0]!.clipped).toBe(true);
    expect(peakOf(res.stems[0]!.mono)).toBe(1);
  });

  it("truncation is reported, never silent", async () => {
    const res = await prepareFourStems(
      [{ name: "vocal", filename: "long.wav", buffer: fakeBuffer(300000, 1, 48000, () => 0.2) }],
      { deviceRate: 48000, maxBlocks: 64 },
    );
    expect(res.truncated).toBe(true);
    expect(res.blocks).toBe(64);
  });

  it("derives BPM from tap timestamps", () => {
    expect(bpmFromTaps([0, 500, 1000, 1500])).toBe(120);
    expect(bpmFromTaps([0])).toBeNull();
  });
});

/* ---------- meta ---------- */

describe("index block", () => {
  it("round-trips and preserves unmanaged tail bytes", async () => {
    const mock = new MockSp1();
    const { io, session } = await connect(mock);
    const layout = session.layout!;
    const raw = new Uint8Array(512);
    put32(raw, 0, layout.magic);
    raw[500] = 0xa5; // unmanaged tail byte
    const meta = parseMeta(raw, layout);
    meta.slots[0]!.present[0] = 1;
    meta.slots[0]!.trkLen[0] = 12;
    const rebuilt = buildMeta(meta, layout);
    expect(rebuilt[500]).toBe(0xa5);
    const reparsed = parseMeta(rebuilt, layout);
    expect(reparsed.slots[0]!.trkLen[0]).toBe(12);
    expect(capacity(layout, reparsed).totalBlocks).toBe(8 * 4 * 64);
    expect(blocksToSeconds(64, 48000)).toBeCloseTo(0.341, 3);
    await io.close();
  });
});

/* ---------- transactional upload ---------- */

describe("transactional upload", () => {
  it("uploads, retries a failed chunk, verifies and finalizes atomically", async () => {
    const mock = new MockSp1();
    const { io, session } = await connect(mock);
    const meta = parseMeta(mock.block(0), session.layout!);
    const res = await preparedFour(700);
    const failBlk = session.trackBlock(1, 0) + 1;
    (mock.opts as { failWriteOnce?: number[] }).failWriteOnce = [failBlk];

    const stages: string[] = [];
    const out = await uploadSong({
      session,
      meta,
      slot: 1,
      stems: res.stems,
      verifyStride: 1,
      onProgress: (p) => stages.push(p.stage),
    });

    expect(out.ok).toBe(true);
    expect(out.retries).toBeGreaterThan(0);
    expect(out.detail).toBe("Upload verified. You may disconnect the SP-1 and use it standalone.");
    expect(new Set(stages)).toEqual(new Set(["checksumming", "uploading", "verifying", "finalizing", "complete"]));
    // index committed: block 0 written LAST and flushed
    expect(mock.flushes).toBe(1);
    const after = parseMeta(mock.block(0), session.layout!);
    expect(after.slots[1]!.present).toEqual([1, 1, 1, 1]);
    expect(after.slots[1]!.trkLen[0]).toBe(res.stems[0]!.blocks);
    // byte-exact audio on the device
    const dst = session.trackBlock(1, 0);
    expect(equalBytes(mock.block(dst), res.stems[0]!.bytes.subarray(0, 512))).toBe(true);
    await io.close();
  });

  it("cancellation leaves the slot unreachable — no partial song is playable", async () => {
    const mock = new MockSp1();
    const { io, session } = await connect(mock);
    const meta = parseMeta(mock.block(0), session.layout!);
    const res = await preparedFour(2000);
    const signal = { aborted: false };
    const out = await uploadSong({
      session,
      meta,
      slot: 2,
      stems: res.stems,
      signal,
      onProgress: (p) => {
        if (p.stage === "uploading" && p.fraction > 0.1) signal.aborted = true;
      },
    });
    expect(out.ok).toBe(false);
    expect(out.detail).toMatch(/cancelled/);
    expect(mock.flushes).toBe(0);
    expect(parseMeta(mock.block(0), session.layout!).slots[2]!.present).toEqual([0, 0, 0, 0]);
    await io.close();
  });

  it("a disconnect mid-upload commits nothing and the index stays authoritative", async () => {
    const mock = new MockSp1({ disconnectAfterWrites: 5 });
    const { io, session } = await connect(mock);
    const meta = parseMeta(mock.block(0), session.layout!);
    const res = await preparedFour(2000);
    const out = await uploadSong({ session, meta, slot: 0, stems: res.stems });
    expect(out.ok).toBe(false);
    expect(mock.flushes).toBe(0);
    await io.close();
    expect(mock.releasedReader && mock.releasedWriter && mock.closedPort).toBe(true);
  }, 30000);

  it("resume finds the first unwritten block by read-back", async () => {
    const mock = new MockSp1();
    const { io, session } = await connect(mock);
    const res = await preparedFour(1500);
    const stem = res.stems[0]!;
    const dst = session.trackBlock(3, 0);
    for (let i = 0; i < 3; i++) await session.writeBlock(dst + i, stem.bytes.subarray(i * 512, (i + 1) * 512));
    expect(await findResumeBlock(session, 3, 0, stem)).toBe(3);
    for (let i = 3; i < stem.blocks; i++) await session.writeBlock(dst + i, stem.bytes.subarray(i * 512, (i + 1) * 512));
    expect(await findResumeBlock(session, 3, 0, stem)).toBe(stem.blocks);
    await io.close();
  });

  it("delete clears the slot in the index and flushes", async () => {
    const mock = new MockSp1();
    const { io, session } = await connect(mock);
    const meta = parseMeta(mock.block(0), session.layout!);
    const res = await preparedFour(400);
    await uploadSong({ session, meta, slot: 4, stems: res.stems });
    await deleteSlot(session, meta, 4);
    expect(parseMeta(mock.block(0), session.layout!).slots[4]!.present).toEqual([0, 0, 0, 0]);
    await io.close();
  });

  it("checksums are host-local and stable", () => {
    expect(checksum32(Uint8Array.from([1, 2, 3]))).toBe(checksum32(Uint8Array.from([1, 2, 3])));
    expect(checksum32(Uint8Array.from([1, 2, 3]))).not.toBe(checksum32(Uint8Array.from([3, 2, 1])));
  });
});

describe("privacy", () => {
  it("no uploader module performs any network request", async () => {
    const fetchSpy = vi.fn();
    const original = globalThis.fetch;
    globalThis.fetch = fetchSpy as unknown as typeof fetch;
    const mock = new MockSp1();
    const { io, session } = await connect(mock);
    const meta = parseMeta(mock.block(0), session.layout!);
    const res = await preparedFour(400);
    await uploadSong({ session, meta, slot: 5, stems: res.stems });
    await io.close();
    globalThis.fetch = original;
    expect(fetchSpy).not.toHaveBeenCalled();
  });
});
