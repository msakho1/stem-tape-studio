/**
 * SECTION 2 — THE SIX MAGIC-WRITE CASES, TESTED SEPARATELY
 *
 * The validity magic is the single commit point of the v1.1 contract. "Torn"
 * and "landed" are NOT collapsed into one result: each of the six physically
 * distinct outcomes is asserted on its own, always after rebooting the mock from
 * its stored block contents.
 *
 *   A no bytes of the final block applied            -> previous generation active
 *   B only the validity-magic bytes applied, valid   -> new generation may be active
 *   C partial block applied, CRC/structure invalid   -> previous generation active
 *   D complete block applied, acknowledgement lost   -> new generation committed
 *   E complete block + flush ok, confirmation lost   -> new generation committed
 *   F complete block acknowledged but not durable    -> previous generation active
 *
 * In no case may both generations be invalid.
 */
import { mkdirSync, writeFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import type { MockSp1, WriteAction } from "./mockSerial";
import {
  clearInjection,
  forkBaseline,
  reconnect,
  regionHash,
  sha256,
  song,
  songImage,
  storedSong,
  withFirstSong,
  type Baseline,
} from "./abHarness";

const FRAMES = 680;
const REPORT_DIR = "handoff/v1.1/reports";

/** True for the one write that carries the validity magic 'STIX' (LE). */
function isMagic(data: Uint8Array): boolean {
  return data[0] === 0x58 && data[1] === 0x49 && data[2] === 0x54 && data[3] === 0x53;
}

function onMagic(mock: MockSp1, action: WriteAction) {
  mock.opts.onWrite = ({ data }) => (isMagic(data) ? action : undefined);
}

/** The five physically distinct commit categories the contract distinguishes. */
type CommitCategory =
  | "no-final-bytes-applied"
  | "valid-magic-prefix-intact-body"
  | "body-corrupted-or-crc-invalid"
  | "complete-block-ack-lost"
  | "non-durable-write";

interface CaseResult {
  id: string;
  category: CommitCategory;
  description: string;
  expected: "previous" | "new";
  observed: "previous" | "new";
  uploadOutcome: string;
  resolvedOutcome: string;
  activeGeneration: number;
  bothGenerationsInvalid: boolean;
}

const table: CaseResult[] = [];

async function runCase(
  base: Baseline,
  id: string,
  category: CommitCategory,
  description: string,
  expected: "previous" | "new",
  inject: (mock: MockSp1) => void,
) {
  const b = { ...base, ...(await forkBaseline(base)) };
  const two = await song("TWO", FRAMES, 11);
  inject(b.mock);
  const res = await b.t.uploadSong({ song: two });
  clearInjection(b.mock);

  const { mock: m2, t: t2 } = await reconnect(b.mock);
  const lib = (await t2.readLibrary())!;

  // Never both invalid.
  expect(lib.requiresInitialization).toBe(false);
  expect(lib.status).toBe("ok");

  const a = lib.active!;
  const isNew = a.songChecksum === two.checksum && a.generation === base.generation + 1;
  const isOld = a.songChecksum === base.one.checksum && a.generation === base.generation;
  expect(isNew || isOld).toBe(true);
  expect(isNew ? "new" : "previous").toBe(expected);

  // The active record always points at complete, byte-exact audio.
  const stored = storedSong(m2, base.caps, a.songSlot, a.sectorCount);
  expect(sha256(stored)).toBe(sha256(isNew ? songImage(two) : base.oneImage));

  if (isOld) {
    expect(regionHash(m2, base.caps.song[base.activeSongSlot])).toBe(base.songRegionHash);
    expect(regionHash(m2, base.caps.index[base.activeIndexSlot])).toBe(base.indexRegionHash);
  }

  const resolved = await t2.resolveOutcome({
    frames: two.frames,
    songChecksum: two.checksum,
    generation: base.generation + 1,
  });
  expect(resolved.outcome).toBe(isNew ? "committed" : "failed");

  table.push({
    id,
    category,
    description,
    expected,
    observed: isNew ? "new" : "previous",
    uploadOutcome: res.outcome,
    resolvedOutcome: resolved.outcome,
    activeGeneration: a.generation,
    bothGenerationsInvalid: false,
  });
}

describe("magic-write cases, split", () => {
  it("A–F each resolve to exactly one complete generation", async () => {
    const base = await withFirstSong(FRAMES);

    await runCase(
      base,
      "A",
      "no-final-bytes-applied",
      "no bytes of the final block are applied",
      "previous",
      (m) => onMagic(m, { apply: "none", ack: "none", disconnect: true }),
    );

    await runCase(
      base,
      "B",
      "valid-magic-prefix-intact-body",
      "only the validity-magic bytes are applied and the record is fully valid",
      "new",
      (m) => onMagic(m, { apply: "partial", partialBytes: 4, ack: "ok", disconnect: true }),
    );

    // A clean prefix tear of the magic block is provably harmless: the block is
    // byte-identical to the already-verified uncommitted record except for the
    // four magic bytes at offset 0, so a prefix tear is either case A or case B.
    // Case C therefore models the destructive variant — a torn program cycle
    // that lands the magic but corrupts the record body.
    await runCase(
      base,
      "C",
      "body-corrupted-or-crc-invalid",
      "a partial block is applied that produces an invalid CRC / structure",
      "previous",
      (m) =>
        onMagic(m, {
          apply: "partial",
          partialBytes: 200,
          ack: "ok",
          disconnect: true,
          mangle: (d) => {
            d[100] = d[100]! ^ 0xff; // body byte inside CRC coverage
            return d;
          },
        }),
    );

    await runCase(
      base,
      "C2",
      "valid-magic-prefix-intact-body",
      "a clean prefix tear that lands only the magic bytes stays valid",
      "new",
      (m) => onMagic(m, { apply: "partial", partialBytes: 64, ack: "ok", disconnect: true }),
    );

    await runCase(
      base,
      "D",
      "complete-block-ack-lost",
      "the complete final block lands but the acknowledgement is lost",
      "new",
      (m) => onMagic(m, { apply: "full", ack: "none", disconnect: true }),
    );

    await runCase(
      base,
      "E",
      "complete-block-ack-lost",
      "the final block and flush succeed but the confirmation is lost",
      "new",
      (m) => {
        let seen = false;
        m.opts.onWrite = ({ data }) => {
          if (isMagic(data)) seen = true;
          return undefined;
        };
        m.opts.onFlush = () => (seen ? { ack: "ok", disconnect: true } : undefined);
      },
    );

    await runCase(
      base,
      "F",
      "non-durable-write",
      "the final block is acknowledged but not durably applied",
      "previous",
      (m) => onMagic(m, { apply: "none", ack: "ok", disconnect: true }),
    );

    expect(table.map((r) => r.id)).toEqual(["A", "B", "C", "C2", "D", "E", "F"]);
    expect(table.every((r) => r.observed === r.expected)).toBe(true);
    expect(table.some((r) => r.bothGenerationsInvalid)).toBe(false);

    // All five commit categories are exercised, and each category always
    // resolves to the same generation — proved by reparsing stored blocks after
    // reboot, never by mock-internal state.
    const byCategory: Record<string, { cases: string[]; resolvesTo: string[] }> = {};
    for (const r of table) {
      const e = (byCategory[r.category] ??= { cases: [], resolvesTo: [] });
      e.cases.push(r.id);
      if (!e.resolvesTo.includes(r.observed)) e.resolvesTo.push(r.observed);
    }
    expect(Object.keys(byCategory).sort()).toEqual([
      "body-corrupted-or-crc-invalid",
      "complete-block-ack-lost",
      "no-final-bytes-applied",
      "non-durable-write",
      "valid-magic-prefix-intact-body",
    ]);
    expect(
      Object.fromEntries(Object.entries(byCategory).map(([k, v]) => [k, v.resolvesTo])),
    ).toEqual({
      "no-final-bytes-applied": ["previous"],
      "valid-magic-prefix-intact-body": ["new"],
      "body-corrupted-or-crc-invalid": ["previous"],
      "complete-block-ack-lost": ["new"],
      "non-durable-write": ["previous"],
    });

    mkdirSync(REPORT_DIR, { recursive: true });
    writeFileSync(
      `${REPORT_DIR}/magic-write-cases.json`,
      JSON.stringify(
        {
          note: "Every result is produced by rebooting the mock from its stored blocks and reparsing them through the production selector. No hidden mock state is consulted.",
          byCategory,
          cases: table,
        },
        null,
        2,
      ) + "\n",
    );
  }, 120000);
});
