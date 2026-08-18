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
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
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

    // Byte-level difference over the transmitted frames.
    const byteDifferenceCount = differences.length;
    // Command-order difference: positional mismatch of the operation notes.
    const orderLen = Math.max(recA.opNotes.length, recB.opNotes.length);
    let commandOrderDifferenceCount = 0;
    for (let i = 0; i < orderLen; i++) if (recA.opNotes[i] !== recB.opNotes[i]) commandOrderDifferenceCount++;
    // Device-state difference: blocks whose stored contents differ after the run.
    const touchedBlocks = new Set<number>([...mockA.blocks.keys(), ...mockB.blocks.keys()]);
    const deviceStateDifferenceCount = [...touchedBlocks].filter(
      (blk) => hex(mockA.block(blk)) !== hex(mockB.block(blk)),
    ).length;

    mkdirSync(AUDIT_DIR, { recursive: true });
    const originalPath = resolve(AUDIT_DIR, "tape-looper-original-transcript.json");
    const inheritedPath = resolve(AUDIT_DIR, "stem-tape-inherited-transcript.json");
    writeFileSync(originalPath, JSON.stringify(original, null, 2));
    writeFileSync(inheritedPath, JSON.stringify(inherited, null, 2));

    const fileSha = (p: string) => createHash("sha256").update(readFileSync(p)).digest("hex");
    const originalKeys = Object.keys(original);
    const inheritedKeys = Object.keys(inherited);
    const wrapperOnlyKeys = originalKeys.filter((k) => !inheritedKeys.includes(k));
    /**
     * The two transcript FILES are different documents about different sources,
     * so their file hashes differ by construction. What must be identical is the
     * transmitted bytes, the command order and the resulting device state.
     */
    const payloadIdentical =
      JSON.stringify({ operations: original.operations, transmitted: original.transmitted, entries: original.entries }) ===
      JSON.stringify({
        operations: inherited.operations,
        transmitted: inherited.transmitted,
        entries: inherited.entries,
      });

    const comparison = {
      schema: "stem-tape-transcript-comparison/2",
      generatedAt: new Date().toISOString(),
      goldenFileSha256: original.sourceFileSha256,
      operations: OPS,
      transcriptFileSha256: { original: fileSha(originalPath), inherited: fileSha(inheritedPath) },
      transcriptFileHashesDiffer: fileSha(originalPath) !== fileSha(inheritedPath),
      fileHashDifferenceExplanation:
        "The two audit documents describe different sources and therefore carry different wrapper metadata keys " +
        `(${wrapperOnlyKeys.join(", ")}) and a different 'source' value. Their comparable payload — operations, ` +
        "transmitted frames and entries — is byte-identical, which is what the conformance claim rests on. Differently " +
        "structured audit documents are not expected to share a file hash.",
      wrapperOnlyKeys,
      payloadIdentical,
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
      byteDifferenceCount,
      commandOrderDifferenceCount,
      deviceStateDifferenceCount,
      differenceCount: differences.length,
      differences,
      verdict: differences.length === 0 ? "identical" : "DIVERGENT",
    };

    writeFileSync(resolve(AUDIT_DIR, "transcript-comparison.json"), JSON.stringify(comparison, null, 2));

    expect(comparison.differences).toEqual([]);
    expect(comparison.transmittedSha256.original).toBe(comparison.transmittedSha256.inherited);
    expect(comparison.operationOrder.identical).toBe(true);
    expect(comparison.deviceStateIdentical).toBe(true);
    expect(comparison.transmittedFrameCount.original).toBeGreaterThan(4);
    expect(comparison.byteDifferenceCount).toBe(0);
    expect(comparison.commandOrderDifferenceCount).toBe(0);
    expect(comparison.deviceStateDifferenceCount).toBe(0);
    expect(comparison.payloadIdentical).toBe(true);
  });

});
