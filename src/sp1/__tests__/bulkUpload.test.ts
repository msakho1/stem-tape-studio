/**
 * Bulk verified-sector upload ('U') conformance.
 *
 * Every byte sequence asserted here comes from Claude's committed handoff
 * (`stem-tape-bulk-upload-handoff.md` §2, §3, §7, §8) — the request header, the
 * 14-byte response, the "STBC" capability block and the sequencing rules. The
 * mock device performs a real write, a real read-back and a real CRC of the
 * bytes it read back; it never echoes an acknowledgement it did not earn.
 *
 * Nothing here claims physical hardware verification.
 */
import { describe, expect, it } from "vitest";
import {
  BULK_CMD,
  BULK_PAYLOAD_BYTES,
  BULK_REQ_HEADER_BYTES,
  BULK_RESP_BYTES,
  BULK_STATUS,
  bulkDestBlock,
  bulkStatusIsRetryable,
  buildBulkCaps,
  buildBulkRequest,
  buildBulkResponse,
  describeBulkStatus,
  parseBulkCaps,
  parseBulkHeader,
  parseBulkResponse,
} from "../bulkTransfer";
import { crc32 } from "../crc32";
import { MockSp1 } from "./mockSerial";
import { Sp1Session, Sp1Transport, type SerialLikePort } from "../protocol";
import { attach, song, songImage } from "./abHarness";

/** A live session over the mock, before any 'Q' has opened a write session. */
async function openSession(dev: MockSp1) {
  const io = new Sp1Transport(dev.port() as SerialLikePort);
  const session = new Sp1Session(io);
  await session.handshake();
  return { session };
}

const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");

describe("bulk wire contract", () => {
  it("builds the exact 17-byte request header from the handoff's worked example", () => {
    const payload = new Uint8Array(BULK_PAYLOAD_BYTES);
    payload.set(new TextEncoder().encode("STSC"));
    const req = buildBulkRequest({ seq: 0, destBlock: 16, payload });
    expect(req.length).toBe(1 + BULK_REQ_HEADER_BYTES + BULK_PAYLOAD_BYTES);
    expect(req[0]).toBe(BULK_CMD); // 'U' = 0x55
    const h = parseBulkHeader(req.slice(1, 1 + BULK_REQ_HEADER_BYTES));
    expect(h).toMatchObject({ version: 1, seq: 0, destBlock: 16, payloadLen: 8192 });
    expect(h.payloadCrc32).toBe(crc32(payload));
    // version=01, seq=00000000, dest=10 00 00 00, len=00 20 00 00
    expect(hex(req.slice(1, 14))).toBe("01" + "00000000" + "10000000" + "00200000");
  });

  it("parses the 14-byte response layout", () => {
    const resp = buildBulkResponse(BULK_STATUS.OK, 0, 16, 0xdfe2813a);
    expect(resp.length).toBe(BULK_RESP_BYTES);
    expect(hex(resp)).toBe("00" + "00000000" + "10000000" + "3a81e2df" + "00");
    expect(parseBulkResponse(resp)).toEqual({
      status: 0,
      seq: 0,
      destBlock: 16,
      verifiedCrc32: 0xdfe2813a,
      retryable: false,
    });
  });

  it("marks exactly the frozen retryable set", () => {
    const retryable = [3, 4, 5, 14, 15, 16];
    for (let s = 0; s <= 16; s++) expect(bulkStatusIsRetryable(s)).toBe(retryable.includes(s));
    for (let s = 1; s <= 16; s++) expect(describeBulkStatus(s)).not.toMatch(/unknown/);
  });

  it("reads support only from the explicit STBC extension", () => {
    expect(hex(buildBulkCaps())).toBe("53544243" + "01000000" + "00200000");
    expect(parseBulkCaps(buildBulkCaps())?.supported).toBe(true);
    expect(parseBulkCaps(new Uint8Array(12))).toBeNull();
    expect(parseBulkCaps(null)).toBeNull();
  });

  it("derives destination blocks as region_start + seq * 16", () => {
    expect(bulkDestBlock(16, 0)).toBe(16);
    expect(bulkDestBlock(16, 3)).toBe(64);
  });
});

describe("bulk session behaviour on the mock device", () => {
  it("refuses bulk writes when no session is open and accepts them after 'Q'", async () => {
    const dev = new MockSp1({ stemTape: true, bulk: true });
    const { session } = await openSession(dev);
    const payload = new Uint8Array(BULK_PAYLOAD_BYTES).fill(7);
    const first = await session.writeSectorBulk(0, 16, payload);
    expect(first.status).toBe(BULK_STATUS.NO_SESSION);
    await session.queryCapabilities();
    expect(session.bulkCaps?.supported).toBe(true);
    const ok = await session.writeSectorBulk(0, 16, payload);
    expect(ok.status).toBe(BULK_STATUS.OK);
    expect(ok.verifiedCrc32).toBe(crc32(payload));
    expect(dev.songBytes(0, 1).slice(0, BULK_PAYLOAD_BYTES)).toEqual(payload);
  });

  it("replays the identical response for a duplicate transaction and never advances", async () => {
    const dev = new MockSp1({ stemTape: true, bulk: true });
    const { session } = await openSession(dev);
    await session.queryCapabilities();
    const payload = new Uint8Array(BULK_PAYLOAD_BYTES).fill(3);
    const a = await session.writeSectorBulk(0, 16, payload);
    const b = await session.writeSectorBulk(0, 16, payload);
    // The WIRE response must be byte-identical on a duplicate transaction
    // (the whole point of idempotent retry) -- writeMs/ackMs are host-side
    // timing metadata added alongside the response, not part of it, and
    // are expected to differ between two real calls.
    const { writeMs: _aWriteMs, ackMs: _aAckMs, ...aWire } = a;
    const { writeMs: _bWriteMs, ackMs: _bAckMs, ...bWire } = b;
    expect(bWire).toEqual(aWire);
    // The next genuinely new sector is still seq 1.
    const c = await session.writeSectorBulk(1, 32, payload);
    expect(c.status).toBe(BULK_STATUS.OK);
    expect(c.seq).toBe(1);
  });

  it("rejects an out-of-sequence transaction and a mismatched destination", async () => {
    const dev = new MockSp1({ stemTape: true, bulk: true });
    const { session } = await openSession(dev);
    await session.queryCapabilities();
    const payload = new Uint8Array(BULK_PAYLOAD_BYTES).fill(1);
    expect((await session.writeSectorBulk(4, 80, payload)).status).toBe(BULK_STATUS.OUT_OF_SEQUENCE);
    expect((await session.writeSectorBulk(0, 48, payload)).status).toBe(BULK_STATUS.DEST_MISMATCH);
  });

  it("reports a stored-CRC mismatch when the bytes do not survive storage", async () => {
    const dev = new MockSp1({
      stemTape: true,
      bulk: true,
      onBulk: () => ({ mangle: (d) => { d[100] = d[100]! ^ 0xff; return d; } }),
    });
    const { session } = await openSession(dev);
    await session.queryCapabilities();
    const payload = new Uint8Array(BULK_PAYLOAD_BYTES).fill(9);
    const r = await session.writeSectorBulk(0, 16, payload);
    expect(r.status).toBe(BULK_STATUS.READBACK_CRC_MISMATCH);
    expect(r.retryable).toBe(true);
    expect(r.verifiedCrc32).not.toBe(crc32(payload));
  });
});

describe("full upload over the bulk path", () => {
  async function bulkDevice(sectorsPerSong = 8, o: Partial<ConstructorParameters<typeof MockSp1>[0]> = {}) {
    const mock = new MockSp1({ stemTape: true, bulk: true, sectorsPerSong, ...o });
    const t = await attach(mock);
    await t.initialiseLibrary();
    return { mock, t };
  }

  it("transfers, verifies and commits a multi-sector song without a 512-byte read-back pass", async () => {
    const { mock, t } = await bulkDevice();
    const s = await song("WON'T DO", 1400, 5); // 5 sectors
    const before = mock.writes;
    const res = await t.uploadSong({ song: s });
    expect(res.ok).toBe(true);
    expect(res.outcome).toBe("committed");
    expect(mock.bulkWrites).toBe(res.sectorCount);
    expect(res.sectorCount).toBeGreaterThan(1);
    // Song data reached storage only through 'U'; the index used 'W'.
    expect(mock.writes - before).toBe(res.sectorCount * 16 + 2);
    const lib = mock.activeLibrary();
    expect(lib.active?.title).toBe("WON'T DO");
    expect(mock.songBytes(lib.activeSongSlot!, res.sectorCount)).toEqual(songImage(s));
    // No physical playback claim is ever made by a mock run.
    expect(res.verification.physicalPlaybackVerification).toBe(false);
    expect(res.verification.deviceReadbackVerification).toBe(false);
  });

  it("recovers from a lost acknowledgement by resending the identical transaction", async () => {
    let dropped = false;
    const { mock, t } = await bulkDevice(8, {
      onBulk: (c) => {
        if (c.seq === 2 && !dropped) {
          dropped = true;
          return { ack: "none" };
        }
        return undefined;
      },
    });
    const s = await song("RETRY", 1400, 6);
    const res = await t.uploadSong({ song: s });
    expect(dropped).toBe(true);
    expect(res.ok).toBe(true);
    expect(res.retries).toBeGreaterThan(0);
    expect(mock.songBytes(mock.activeLibrary().activeSongSlot!, res.sectorCount)).toEqual(songImage(s));
  }, 30000);

  it("recovers from a transient incoming-CRC rejection", async () => {
    let once = false;
    const { t } = await bulkDevice(8, {
      onBulk: (c) => {
        if (c.seq === 1 && !once) {
          once = true;
          return { status: BULK_STATUS.CRC_MISMATCH };
        }
        return undefined;
      },
    });
    const res = await t.uploadSong({ song: await song("CRC", 1400, 7) });
    expect(once).toBe(true);
    expect(res.ok).toBe(true);
    expect(res.retries).toBe(1);
  });

  it("stops without committing when storage keeps failing, leaving the previous song active", async () => {
    const { mock, t } = await bulkDevice();
    const first = await t.uploadSong({ song: await song("ONE", 700, 1) });
    expect(first.ok).toBe(true);
    const activeBefore = mock.activeLibrary();
    mock.opts.onBulk = () => ({ status: BULK_STATUS.EMMC_WRITE_FAIL });
    const res = await t.uploadSong({ song: await song("TWO", 700, 2) });
    expect(res.ok).toBe(false);
    expect(res.outcome).toBe("failed");
    expect(res.detail).toMatch(/storage failed to accept the sector/);
    const after = mock.activeLibrary();
    expect(after.generation).toBe(activeBefore.generation);
    expect(after.active?.title).toBe("ONE");
  });

  it("does not retry a structural refusal", async () => {
    const { t } = await bulkDevice(8, { onBulk: () => ({ status: BULK_STATUS.OUT_OF_SEQUENCE }) });
    const res = await t.uploadSong({ song: await song("SEQ", 700, 4) });
    expect(res.ok).toBe(false);
    expect(res.detail).toMatch(/expected a different sector/);
  });

  it("leaves the previous song intact when the connection drops during audio", async () => {
    const { mock, t } = await bulkDevice();
    const first = await t.uploadSong({ song: await song("ONE", 1400, 1) });
    expect(first.ok).toBe(true);
    const before = mock.activeLibrary();
    mock.opts.onBulk = (c) => (c.seq === 2 ? { disconnectBefore: true } : undefined);
    const res = await t.uploadSong({ song: await song("TWO", 1400, 2) });
    expect(res.ok).toBe(false);
    expect(res.outcome).toBe("failed");
    const after = mock.activeLibrary();
    expect(after.generation).toBe(before.generation);
    expect(after.active?.title).toBe("ONE");
  }, 30000);
});
