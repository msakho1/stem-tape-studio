/**
 * B. TRANSCRIPT AUDIT ARTIFACTS
 *
 * Runs the SAME inherited operation sequence twice — once through the verbatim
 * Tape Looper companion sliced out of firmware/web/index.html, once through the
 * React Sp1Session — against identical mock devices, through the identical
 * recording wrapper, and writes three machine-readable artifacts:
 *
 *   audit/tape-looper-original-transcript.json
 *   audit/stem-tape-inherited-transcript.json
 *   audit/transcript-comparison.json
 *
 * The test FAILS on any transmitted-byte difference; the comparison artifact is
 * written either way so a failure is inspectable.
 */
import { mkdirSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";
import { MockSp1 } from "./mockSerial";
import { Recorder, recordPort, hex } from "./recordingPort";
import { loadGoldenCompanion, goldenCompanionFileSha256, GOLDEN_COMPANION_PATH } from "./goldenCompanion";
import { Sp1Session, Sp1Transport, type SerialLikePort } from "../protocol";

const AUDIT_DIR = resolve(process.cwd(), "audit");
const block = (seed: number) => Uint8Array.from({ length: 512 }, (_, i) => (i * 13 + seed) & 255);

/** The inherited operation set: discovery, ping, read, write, flush, exit. */
const OPS = ["handshake", "read block 0", "write block 4", "flush", "read-back block 4", "exit"] as const;

function sha(v: unknown) {
  return createHash("sha256").update(JSON.stringify(v)).digest("hex");
}

describe("transcript audit artifacts", () => {
  it("writes original/inherited/comparison transcripts and fails on any byte difference", async () => {
    const mockA = new MockSp1();
    const mockB = new MockSp1();
    const recA = new Recorder();
    const recB = new Recorder();

    const g = loadGoldenCompanion();
    const ioA = new g.Serial(recordPort(mockA.port(), recA) as unknown) as {
      close(): Promise<void>;
    };
    g.setIo(ioA as never);
    recA.op(OPS[0]);
    await g.handshake();
    recA.op(OPS[1]);
    await g.readBlock(0);
    recA.op(OPS[2]);
    await g.writeBlock(4, block(1));
    recA.op(OPS[3]);
    await g.commitToDevice();
    recA.op(OPS[4]);
    await g.readBlock(4);
    recA.op(OPS[5]);
    await g.exit();
    g.setIo(null);

    const ioB = new Sp1Transport(recordPort(mockB.port(), recB) as SerialLikePort);
    const s = new Sp1Session(ioB);
    recB.op(OPS[0]);
    await s.handshake();
    recB.op(OPS[1]);
    await s.readBlock(0);
    recB.op(OPS[2]);
    await s.writeBlock(4, block(1));
    recB.op(OPS[3]);
    await s.flush();
    recB.op(OPS[4]);
    await s.readBlock(4);
    recB.op(OPS[5]);
    await s.exit();

    const original = {
      source: GOLDEN_COMPANION_PATH,
      sourceFileSha256: goldenCompanionFileSha256(),
      executedRegionSha256: g.sourceSha256,
      operations: OPS,
      transmitted: recA.txHex,
      entries: recA.entries,
    };
    const inherited = {
      source: "src/sp1/protocol.ts (Sp1Transport + Sp1Session)",
      operations: OPS,
      transmitted: recB.txHex,
      entries: recB.entries,
    };

    const differences: { index: number; original: string | undefined; inherited: string | undefined }[] = [];
    const n = Math.max(recA.txHex.length, recB.txHex.length);
    for (let i = 0; i < n; i++) {
      if (recA.txHex[i] !== recB.txHex[i]) {
        differences.push({ index: i, original: recA.txHex[i], inherited: recB.txHex[i] });
      }
    }

    const comparison = {
      schema: "stem-tape-transcript-comparison/1",
      generatedAt: new Date().toISOString(),
      goldenFileSha256: original.sourceFileSha256,
      operations: OPS,
      transmittedFrameCount: { original: recA.txHex.length, inherited: recB.txHex.length },
      transmittedSha256: {
        original: sha(recA.txHex),
        inherited: sha(recB.txHex),
      },
      operationOrder: {
        original: recA.opNotes,
        inherited: recB.opNotes,
        identical: JSON.stringify(recA.opNotes) === JSON.stringify(recB.opNotes),
      },
      deviceStateIdentical: hex(mockA.block(4)) === hex(mockB.block(4)),
      differenceCount: differences.length,
      differences,
      verdict: differences.length === 0 ? "identical" : "DIVERGENT",
    };

    mkdirSync(AUDIT_DIR, { recursive: true });
    writeFileSync(resolve(AUDIT_DIR, "tape-looper-original-transcript.json"), JSON.stringify(original, null, 2));
    writeFileSync(resolve(AUDIT_DIR, "stem-tape-inherited-transcript.json"), JSON.stringify(inherited, null, 2));
    writeFileSync(resolve(AUDIT_DIR, "transcript-comparison.json"), JSON.stringify(comparison, null, 2));

    expect(comparison.differences).toEqual([]);
    expect(comparison.transmittedSha256.original).toBe(comparison.transmittedSha256.inherited);
    expect(comparison.operationOrder.identical).toBe(true);
    expect(comparison.deviceStateIdentical).toBe(true);
    expect(comparison.transmittedFrameCount.original).toBeGreaterThan(4);
  });
});
