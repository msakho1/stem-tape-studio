/**
 * v1.3 LAYOUT PROOF (audit helper, not shipped).
 *
 * Answers the three asks of the "one thing still outstanding" addendum using
 * the production modules only:
 *   1. first 32 bytes of each stem's first group in the assembled region;
 *   2. the complete committed 256-byte STIX record from a real mock upload;
 *   3. padding / seq-ordering / unknown-outcome behaviour, observed not asserted.
 */
import { prepareCanonicalSong } from "../src/sp1/song";
import { encodeSong } from "../src/sp1/sector";
import { stemChecksums16, songChecksumFromStems } from "../src/sp1/pcm16";
import { STEM_ORDER } from "../src/sp1/prepare";
import { decodeWavFixture, offlineStub } from "../src/sp1/__tests__/fixtureWav";
import {
  BLOCKS_PER_GROUP,
  FRAMES_PER_GROUP,
  GROUP_BYTES,
  PHYSICAL_BLOCK_BYTES,
  SECTOR_BYTES,
  STEM_COUNT,
  groupsForFrames,
  planarBlockOf,
} from "../src/sp1/stemTapeFormat";
import { MockSp1 } from "../src/sp1/__tests__/mockSerial";
import { attach } from "../src/sp1/__tests__/abHarness";
import { parseIndexRecord } from "../src/sp1/stemIndex";
import { crc32 } from "../src/sp1/crc32";
import { CRC_RANGE, CRC_ZEROED } from "../src/sp1/stemTapeFormat";

const hex = (b: Uint8Array) => Buffer.from(b).toString("hex").replace(/(..)/g, "$1 ").trim();

const inputs = STEM_ORDER.map((name) => ({ name, filename: `${name}.wav`, buffer: decodeWavFixture(name) }));
const song = await prepareCanonicalSong(inputs, {
  metadata: { title: "audit", artist: "audit", bpm: 120, downbeatSeconds: 0 },
  make: offlineStub,
});

const sectors = encodeSong(song);
const region = new Uint8Array(sectors.length * SECTOR_BYTES);
sectors.forEach((s, i) => region.set(s, i * SECTOR_BYTES));

const groups = groupsForFrames(song.frames);
console.log("frames               ", song.frames);
console.log("framesPerGroup       ", FRAMES_PER_GROUP);
console.log("groupsPerStem        ", groups);
console.log("realFramesLastGroup  ", song.frames - (groups - 1) * FRAMES_PER_GROUP);
console.log("padFramesLastGroup   ", groups * FRAMES_PER_GROUP - song.frames);
console.log("totalBlocks          ", groups * STEM_COUNT * BLOCKS_PER_GROUP);
console.log("totalBytes           ", region.length);

const sums = stemChecksums16(song);
sums.forEach((c, i) =>
  console.log(`stem ${i} ${STEM_ORDER[i]!.padEnd(11)} ${c >>> 0} 0x${(c >>> 0).toString(16).padStart(8, "0")}`),
);
const songSum = songChecksumFromStems(sums) >>> 0;
const digest = new Uint8Array(16);
sums.forEach((c, i) => new DataView(digest.buffer).setUint32(i * 4, c >>> 0, true));
console.log("digest               ", hex(digest));
console.log("songChecksum         ", songSum, "0x" + songSum.toString(16).padStart(8, "0"));

console.log("\n-- first 32 bytes of each stem's first group (assembled region) --");
for (let s = 0; s < STEM_COUNT; s++) {
  const blk = planarBlockOf(0, s, 0, groups);
  const off = blk * PHYSICAL_BLOCK_BYTES;
  console.log(`stem ${s}   ${hex(region.subarray(off, off + 16))}`);
  console.log(`         ${hex(region.subarray(off + 16, off + 32))}`);
}

console.log("\n-- flags byte written into EVERY group --");
const flagsSeen = new Set<number>();
const magicBad: number[] = [];
for (let s = 0; s < STEM_COUNT; s++) {
  for (let g = 0; g < groups; g++) {
    const off = planarBlockOf(0, s, g, groups) * PHYSICAL_BLOCK_BYTES;
    if (region[off] !== 0x50 || region[off + 1] !== 0x4c) magicBad.push(off);
    if (region[off + 2] !== s) magicBad.push(off);
    const gi = new DataView(region.buffer, region.byteOffset).getUint32(off + 4, true);
    if (gi !== g) magicBad.push(off);
    flagsSeen.add(region[off + 3]!);
  }
}
console.log("groups inspected     ", groups * STEM_COUNT);
console.log("distinct flag bytes  ", [...flagsSeen].map((f) => "0x" + f.toString(16).padStart(2, "0")).join(","));
console.log("header/index faults  ", magicBad.length);

console.log("\n-- final-group zero padding --");
for (let s = 0; s < STEM_COUNT; s++) {
  const off = planarBlockOf(0, s, groups - 1, groups) * PHYSICAL_BLOCK_BYTES;
  const real = song.frames - (groups - 1) * FRAMES_PER_GROUP;
  const tail = region.subarray(off + 8 + real * 4, off + GROUP_BYTES);
  console.log(`stem ${s} padBytes=${tail.length} allZero=${tail.every((b) => b === 0)}`);
}

/* ---------- real upload against the mock, committed STIX record ---------- */
const mock = new MockSp1({ bulk: true });
const t = await attach(mock);
await t.initialiseLibrary();
const seqs: number[] = [];
const m2 = new MockSp1({ bulk: true, onBulk: ({ seq }) => void seqs.push(seq) });
const t2 = await attach(m2);
await t2.initialiseLibrary();
const res = await t2.uploadSong({ song });
console.log("\n-- upload --");
console.log("outcome              ", res.outcome ?? "committed");
console.log("bulk seq count       ", seqs.length);
console.log("bulk seq ascending   ", seqs.every((v, i) => v === i));
console.log("bulk seq first/last  ", seqs[0], seqs[seqs.length - 1]);

const lib = await t2.readLibrary();
const idxBlock = m2.block(
  lib!.activeIndexSlot === 0 ? m2.caps.index[0].start : m2.caps.index[1].start,
);
const record = idxBlock.subarray(0, 256);
console.log("\n-- committed 256-byte STIX record (hex) --");
for (let i = 0; i < 256; i += 16) console.log(hex(record.subarray(i, i + 16)));
const parsed = parseIndexRecord(record);
console.log("crc stored/computed  ", parsed.crc.toString(16), crc32(record, CRC_RANGE.from, CRC_RANGE.to, CRC_ZEROED).toString(16));
console.log("version/format/depth ", parsed.indexVersion, `${parsed.formatMajor}.${parsed.formatMinor}`, parsed.bitDepth);
console.log("frames/sectors/blocks", parsed.frames, parsed.sectorCount, parsed.songBlockCount);
console.log("padding beyond 256   ", idxBlock.subarray(256).every((b) => b === 0));
void mock;
void t;
