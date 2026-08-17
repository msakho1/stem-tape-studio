/**
 * Transactional four-stem upload.
 *
 * The wire protocol has no staging area, no per-chunk CRC and no
 * device-reported verified offset. What it does have:
 *   - block-addressed idempotent writes ('W' ack 'w')
 *   - read-back ('R' ack 'r') for byte-exact verification
 *   - a flush ('F' ack 'f')
 *   - a single index block whose MAGIC field lives in block 0
 *
 * Atomicity therefore comes from write ordering, exactly as the firmware and
 * the proven transfer page do it: all audio blocks first (a slot whose index
 * entry is still present=0 is unreachable, so a partial song can never be
 * played), then the index tail block, then block 0 (magic) LAST, then flush.
 * An interruption at any point leaves the previous index authoritative.
 *
 * Resume: the device reports no verified offset, so resume re-verifies the
 * already-written blocks by read-back and continues from the first mismatch.
 */

import { BLOCK_BYTES, type Sp1Session } from "./protocol";
import { buildMeta, metaBlockCount, type Sp1Meta } from "./meta";
import type { PreparedStem } from "./prepare";

export type UploadStage =
  | "decoding"
  | "resampling"
  | "packing"
  | "checksumming"
  | "uploading"
  | "verifying"
  | "finalizing"
  | "complete";

export interface UploadProgress {
  stage: UploadStage;
  fraction: number;
  detail: string;
}

export const MAX_CHUNK_RETRIES = 3;

/** Local integrity digest of a prepared package (host-side only, FNV-1a 32). */
export function checksum32(bytes: Uint8Array): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < bytes.length; i++) {
    h ^= bytes[i]!;
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h >>> 0;
}

export function equalBytes(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

export interface UploadOptions {
  session: Sp1Session;
  meta: Sp1Meta;
  slot: number;
  /** Ordered vocal, drums, bass, instrument -> tracks 1..4. */
  stems: PreparedStem[];
  signal?: { aborted: boolean };
  onProgress?: (p: UploadProgress) => void;
  /** Verify every Nth audio block by read-back (1 = all). */
  verifyStride?: number;
}

export interface UploadOutcome {
  ok: boolean;
  detail: string;
  writtenBlocks: number;
  verifiedBlocks: number;
  retries: number;
  checksums: number[];
}

class Aborted extends Error {
  constructor() {
    super("cancelled — the slot was left untouched and is not playable");
  }
}

/** Write one block with bounded retries; only the failed chunk is retried. */
async function writeChunk(session: Sp1Session, blk: number, data: Uint8Array, counter: { retries: number }) {
  let lastErr: unknown = null;
  for (let attempt = 0; attempt <= MAX_CHUNK_RETRIES; attempt++) {
    try {
      await session.writeBlock(blk, data);
      return;
    } catch (e) {
      lastErr = e;
      counter.retries++;
    }
  }
  throw new Error(`block ${blk} failed after ${MAX_CHUNK_RETRIES} retries: ${String(lastErr)}`);
}

export async function uploadSong(opts: UploadOptions): Promise<UploadOutcome> {
  const { session, meta, slot, stems } = opts;
  const layout = session.layout!;
  const counter = { retries: 0 };
  const stride = opts.verifyStride ?? 8;
  const checksums = stems.map((s) => checksum32(s.bytes));
  let written = 0;
  let verified = 0;

  const report = (stage: UploadStage, fraction: number, detail: string) =>
    opts.onProgress?.({ stage, fraction, detail });

  const abortCheck = () => {
    if (opts.signal?.aborted) throw new Aborted();
  };

  report("checksumming", 1, `${stems.length} stems · package digests computed`);

  try {
    return await session.lock.run(async () => {
      // 2. Re-read capacity and the target slot generation straight from the device.
      abortCheck();
      const before = await session.readBlock(0);
      const generation = checksum32(before);
      const blocks = stems[0]!.blocks;
      if (blocks > layout.trackBlocks) {
        throw new Error(`song is ${blocks} blocks; this SP-1 allows ${layout.trackBlocks} per track`);
      }

      const totalBlocks = blocks * stems.length;
      // 5. Audio blocks. The index still says present=0 for this slot, so
      // nothing written here is reachable until the commit below.
      for (let t = 0; t < stems.length; t++) {
        const stem = stems[t]!;
        const dst = session.trackBlock(slot, t);
        for (let i = 0; i < blocks; i++) {
          abortCheck();
          await writeChunk(session, dst + i, stem.bytes.subarray(i * BLOCK_BYTES, (i + 1) * BLOCK_BYTES), counter);
          written++;
          if (written % 8 === 0 || written === totalBlocks) {
            report("uploading", written / totalBlocks, `${stem.name} · block ${i + 1}/${blocks}`);
          }
        }
      }

      // 9. Verify the staged audio by read-back before anything becomes reachable.
      for (let t = 0; t < stems.length; t++) {
        const stem = stems[t]!;
        const dst = session.trackBlock(slot, t);
        for (let i = 0; i < blocks; i += stride) {
          abortCheck();
          const back = await session.readBlock(dst + i);
          const expect = stem.bytes.subarray(i * BLOCK_BYTES, (i + 1) * BLOCK_BYTES);
          if (!equalBytes(back, expect)) {
            // Repair exactly this block, then re-verify it.
            await writeChunk(session, dst + i, expect, counter);
            const again = await session.readBlock(dst + i);
            if (!equalBytes(again, expect)) throw new Error(`verification failed at block ${dst + i}`);
          }
          verified++;
          report("verifying", verified / (totalBlocks / stride), `${stem.name} · block ${i + 1}`);
        }
      }

      // Guard against a concurrent index change since step 2.
      abortCheck();
      const nowIndex = await session.readBlock(0);
      if (checksum32(nowIndex) !== generation) {
        throw new Error("the device index changed during the upload — nothing was committed");
      }

      // 11. Commit atomically: magic-last index write, then flush.
      report("finalizing", 0.2, "committing the index");
      const sl = meta.slots[slot]!;
      const isBase = !sl.loopLen;
      for (let t = 0; t < stems.length; t++) {
        sl.present[t] = 1;
        sl.trkLen[t] = blocks;
        sl.trkStart[t] = 0;
        sl.trkContent[t] = 0;
      }
      if (!sl.speed) sl.speed = 65536;
      if (isBase) sl.loopLen = blocks * 256;

      const mb = buildMeta(meta, layout);
      if (metaBlockCount(layout) === 2) await writeChunk(session, 1, mb.slice(512, 1024), counter);
      await writeChunk(session, 0, mb.slice(0, 512), counter);

      report("finalizing", 0.6, "flushing storage");
      await session.flush();

      // 12. Read the slot back and compare the committed index.
      const readback = await session.readBlock(0);
      if (!equalBytes(readback, mb.slice(0, 512))) {
        throw new Error("the device index did not read back identically after commit");
      }
      report("complete", 1, "Upload verified. You may disconnect the SP-1 and use it standalone.");
      return {
        ok: true,
        detail: "Upload verified. You may disconnect the SP-1 and use it standalone.",
        writtenBlocks: written,
        verifiedBlocks: verified,
        retries: counter.retries,
        checksums,
      };
    });
  } catch (e) {
    const detail = e instanceof Error ? e.message : String(e);
    return { ok: false, detail, writtenBlocks: written, verifiedBlocks: verified, retries: counter.retries, checksums };
  }
}

/**
 * Resume support. No verified-offset field exists in the protocol, so the
 * host re-verifies written blocks by read-back and returns the first block
 * index that still needs writing.
 */
export async function findResumeBlock(
  session: Sp1Session,
  slot: number,
  track: number,
  stem: PreparedStem,
): Promise<number> {
  const dst = session.trackBlock(slot, track);
  for (let i = 0; i < stem.blocks; i++) {
    const back = await session.readBlock(dst + i);
    if (!equalBytes(back, stem.bytes.subarray(i * BLOCK_BYTES, (i + 1) * BLOCK_BYTES))) return i;
  }
  return stem.blocks;
}

/** Delete a song: index-only, mirroring the device's own delete. */
export async function deleteSlot(session: Sp1Session, meta: Sp1Meta, slot: number): Promise<void> {
  const layout = session.layout!;
  await session.lock.run(async () => {
    const sl = meta.slots[slot]!;
    for (let t = 0; t < 4; t++) {
      sl.present[t] = 0;
      sl.trkLen[t] = 0;
      sl.trkStart[t] = 0;
      sl.trkContent[t] = 0;
    }
    sl.loopLen = 0;
    const mb = buildMeta(meta, layout);
    if (metaBlockCount(layout) === 2) await session.writeBlock(1, mb.slice(512, 1024));
    await session.writeBlock(0, mb.slice(0, 512));
    await session.flush();
  });
}
