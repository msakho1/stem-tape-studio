/**
 * Frozen v1.1 handoff-bundle generator.
 *
 * Run: bun scripts/make-handoff-fixtures.ts
 * Output: handoff/v1.1/
 *
 * Every binary is produced by the PRODUCTION modules (src/sp1/*), never by
 * hand-written byte tables, and every binary is accompanied by a decoded JSON
 * explanation produced by the production parsers. The committed files are what
 * the fixture tests consume; the tests never rebuild the expected bytes.
 *
 * Nothing here touches physical hardware. The device is the in-process mock.
 */
import { createHash } from "node:crypto";
import { mkdirSync, writeFileSync } from "node:fs";
import { MockSp1, type WriteAction } from "../src/sp1/__tests__/mockSerial";
import { Recorder, recordPort } from "../src/sp1/__tests__/recordingPort";
import { decodeWavFixture, offlineStub } from "../src/sp1/__tests__/fixtureWav";
import { Sp1Session, Sp1Transport, type SerialLikePort } from "../src/sp1/protocol";
import { parseCapabilities, serializeCapabilities } from "../src/sp1/compatibility";
import { StemTapeTransport } from "../src/sp1/transport";
import { prepareCanonicalSong, type CanonicalSong } from "../src/sp1/song";
import { encodeSong } from "../src/sp1/sector";
import { indexRecordBlock, parseIndexRecord, validateIndexRecord } from "../src/sp1/stemIndex";
import { readSlot, selectActiveIndex } from "../src/sp1/activeIndex";
import { crc32 } from "../src/sp1/crc32";
import { IX_OFF, SECTOR_BYTES, SLOT_A, SLOT_B, sectorsForFrames } from "../src/sp1/stemTapeFormat";

const OUT = "handoff/v1.1";
const BIN = `${OUT}/binaries`;
const JSONDIR = `${OUT}/decoded`;
const TX = `${OUT}/transcripts`;
for (const d of [OUT, BIN, JSONDIR, TX]) mkdirSync(d, { recursive: true });

const files: { path: string; bytes: Uint8Array }[] = [];

function writeBin(name: string, bytes: Uint8Array) {
  const path = `${BIN}/${name}`;
  writeFileSync(path, bytes);
  files.push({ path, bytes });
  return path;
}
function writeJson(name: string, value: unknown) {
  const path = `${JSONDIR}/${name}`;
  const bytes = new TextEncoder().encode(JSON.stringify(value, null, 2) + "\n");
  writeFileSync(path, bytes);
  files.push({ path, bytes });
  return path;
}
function writeTranscript(name: string, value: unknown) {
  const path = `${TX}/${name}`;
  const bytes = new TextEncoder().encode(JSON.stringify(value, null, 2) + "\n");
  writeFileSync(path, bytes);
  files.push({ path, bytes });
  return path;
}

const NAMES = ["vocal", "drums", "bass", "instrument"] as const;

function tone(frames: number, seed: number): AudioBuffer {
  const l = new Float32Array(frames);
  const r = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    l[i] = Math.sin((i * seed) / 50) * 0.4;
    r[i] = Math.cos((i * seed) / 70) * 0.4;
  }
  return {
    sampleRate: 48000,
    numberOfChannels: 2,
    length: frames,
    duration: frames / 48000,
    getChannelData: (c: number) => (c === 0 ? l : r),
  } as unknown as AudioBuffer;
}

async function toneSong(title: string, frames: number, seed: number): Promise<CanonicalSong> {
  return prepareCanonicalSong(
    NAMES.map((name, i) => ({ name, filename: `${name}.wav`, buffer: tone(frames, i + seed) })),
    { metadata: { title, artist: "Stem Tape handoff", bpm: 120, downbeatSeconds: 0 } },
  );
}

async function attach(mock: MockSp1, rec?: Recorder) {
  const port = rec ? (recordPort(mock.port(), rec) as SerialLikePort) : (mock.port() as SerialLikePort);
  const session = new Sp1Session(new Sp1Transport(port));
  await session.handshake();
  const caps = parseCapabilities((await session.queryCapabilities())!);
  return { session, caps: caps!, t: new StemTapeTransport(session, caps, { kind: "mock" }) };
}

function isMagic(d: Uint8Array) {
  return d[0] === 0x58 && d[1] === 0x49 && d[2] === 0x54 && d[3] === 0x53;
}

function decodeIndexBlock(block: Uint8Array, slot: 0 | 1, regions: { song: never; index: never } | any) {
  const rec = parseIndexRecord(block);
  return { record: rec, validation: validateIndexRecord(rec, slot, regions) };
}

/* ---------- 1. STCP capability response ---------- */

const probe = new MockSp1({ stemTape: true, sectorsPerSong: 8 });
const caps = probe.capabilities;
const capsBytes = serializeCapabilities(caps);
const capsReply = new Uint8Array(4 + capsBytes.length);
capsReply.set(new TextEncoder().encode("STCP"));
capsReply.set(capsBytes, 4);
writeBin("stcp-capability-response.bin", capsReply);
writeJson("stcp-capability-response.json", {
  note: "Bytes 0..4 are the ASCII tag STCP; bytes 4..100 are the 96-byte little-endian capability structure (CAPS_OFF in src/sp1/stemTapeFormat.ts).",
  tag: "STCP",
  decoded: parseCapabilities(capsBytes),
});

/* ---------- 2. empty initialized storage ---------- */

const initMock = new MockSp1({ stemTape: true, sectorsPerSong: 8 });
{
  const { t } = await attach(initMock);
  await t.initialiseLibrary();
}
const regions = { song: caps.song, index: caps.index };
const initA = initMock.block(caps.index[0].start);
const initB = initMock.block(caps.index[1].start);
const initImage = new Uint8Array(initA.length + initB.length);
initImage.set(initA);
initImage.set(initB, initA.length);
writeBin("storage-initialized-empty.bin", initImage);
writeJson("storage-initialized-empty.json", {
  note: "Index region A followed by index region B immediately after explicit initialization. Generation 1, no song present.",
  indexA: decodeIndexBlock(initA, SLOT_A, regions),
  indexB: { allZero: initB.every((b) => b === 0) },
  selection: (() => {
    const l = selectActiveIndex(readSlot(SLOT_A, initA, regions), readSlot(SLOT_B, initB, regions));
    return { activeIndexSlot: l.activeIndexSlot, generation: l.generation, status: l.status, explanation: l.explanation };
  })(),
});

/* ---------- 3/4. two successful uploads, valid index A and index B ---------- */

const rec1 = new Recorder();
const live = new MockSp1({ stemTape: true, sectorsPerSong: 8 });
const first = await (async () => {
  const { t } = await attach(live);
  await t.initialiseLibrary();
  return t;
})();
const songOne = await toneSong("HANDOFF ONE", 680, 3);
const songTwo = await toneSong("HANDOFF TWO", 680, 11);

// upload 1 (recorded)
const rec1Session = await attach(live, rec1);
const up1 = await rec1Session.t.uploadSong({ song: songOne });
writeTranscript("upload-1-successful.json", {
  note: "First successful replacement upload (mock device). tx/rx are raw wire bytes in hex.",
  result: { outcome: up1.outcome, generation: up1.generation, songSlot: up1.targetSongSlot, indexSlot: up1.targetIndexSlot },
  sha256: rec1.sha256(),
  entries: rec1.entries,
});
void first;

// upload 2 (recorded)
const rec2 = new Recorder();
const rec2Session = await attach(live, rec2);
const up2 = await rec2Session.t.uploadSong({ song: songTwo });
writeTranscript("upload-2-successful.json", {
  note: "Second successful replacement upload; destination is the opposite A/B pair.",
  result: { outcome: up2.outcome, generation: up2.generation, songSlot: up2.targetSongSlot, indexSlot: up2.targetIndexSlot },
  sha256: rec2.sha256(),
  entries: rec2.entries,
});

const idxA = live.block(caps.index[0].start);
const idxB = live.block(caps.index[1].start);
writeBin("index-a-valid.bin", idxA);
writeJson("index-a-valid.json", decodeIndexBlock(idxA, SLOT_A, regions));
writeBin("index-b-valid.bin", idxB);
writeJson("index-b-valid.json", decodeIndexBlock(idxB, SLOT_B, regions));

/* ---------- 5. uncommitted index and final magic block ---------- */

const draftSlot = up2.targetIndexSlot!;
const uncommittedSource = live.block(caps.index[draftSlot].start).slice(0);
const uncommitted = uncommittedSource.slice(0);
uncommitted.set([0, 0, 0, 0], IX_OFF.magic); // exactly what step 13 writes
writeBin("index-uncommitted.bin", uncommitted);
writeJson("index-uncommitted.json", {
  note: "Step 13 image: the complete next-generation record with the validity magic deliberately absent (bytes 0..4 zero). Never selectable.",
  decoded: decodeIndexBlock(uncommitted, draftSlot, regions),
});
writeBin("index-final-magic-block.bin", uncommittedSource);
writeJson("index-final-magic-block.json", {
  note: "Step 17 image: byte-identical to index-uncommitted.bin except bytes 0..4, which carry the little-endian validity magic 'STIX' (0x53544958). This single 512-byte block write is the commit point.",
  differsFromUncommittedAtBytes: [...uncommittedSource].map((b, i) => (b === uncommitted[i] ? -1 : i)).filter((i) => i >= 0),
  decoded: decodeIndexBlock(uncommittedSource, draftSlot, regions),
});

/* ---------- 6. four-stem song sectors (real WAV fixtures) ---------- */

const wavInputs = NAMES.map((name) => ({ name, filename: `${name}.wav`, buffer: decodeWavFixture(name) }));
const wavSong = await prepareCanonicalSong(wavInputs, {
  metadata: { title: "Fixture Song", artist: "Stem Tape Tests", bpm: 96, downbeatSeconds: 0.25 },
  make: offlineStub,
});
const wavSectors = encodeSong(wavSong);
const wavImage = new Uint8Array(wavSectors.length * SECTOR_BYTES);
wavSectors.forEach((s, i) => wavImage.set(s, i * SECTOR_BYTES));
writeBin("song-sectors-four-stem.bin", wavImage);
writeJson("song-sectors-four-stem.json", {
  note: "Canonical 8 KiB logical sectors for one four-stem song, exactly as transmitted (32-byte sector header + 340 frames of 24 bytes).",
  source: NAMES.map((n) => `src/sp1/__tests__/fixtures/${n}.wav`),
  frames: wavSong.frames,
  sampleRate: wavSong.sampleRate,
  channels: wavSong.channels,
  pcmDepth: wavSong.pcmDepth,
  sectorCount: wavSectors.length,
  sectorsForFrames: sectorsForFrames(wavSong.frames),
  bytes: wavImage.length,
  songChecksum: wavSong.checksum,
  stems: wavSong.stems.map((s) => ({ name: s.name, originalFrames: s.originalFrames, checksum: s.checksum })),
});

/* ---------- 7. failure transcripts ---------- */

async function failureTranscript(
  name: string,
  note: string,
  inject: (mock: MockSp1) => void,
): Promise<void> {
  const mock = new MockSp1({ stemTape: true, sectorsPerSong: 8 });
  {
    const { t } = await attach(mock);
    await t.initialiseLibrary();
    const r = await t.uploadSong({ song: songOne });
    if (!r.ok) throw new Error(`baseline failed for ${name}`);
  }
  const rec = new Recorder();
  const { t } = await attach(mock, rec);
  inject(mock);
  const res = await t.uploadSong({ song: songTwo });
  delete mock.opts.onWrite;
  delete mock.opts.onFlush;
  delete mock.opts.onRead;

  const after = mock.reboot();
  const { t: t2 } = await attach(after);
  const lib = (await t2.readLibrary())!;
  writeTranscript(name, {
    note,
    uploadResult: { ok: res.ok, outcome: res.outcome, detail: res.detail, failure: res.failure },
    afterReconnect: {
      status: lib.status,
      requiresInitialization: lib.requiresInitialization,
      activeIndexSlot: lib.activeIndexSlot,
      activeSongSlot: lib.activeSongSlot,
      generation: lib.generation,
      title: lib.active?.title ?? null,
      explanation: lib.explanation,
    },
    sha256: rec.sha256(),
    entries: rec.entries,
  });
}

function onMagic(mock: MockSp1, action: WriteAction) {
  mock.opts.onWrite = ({ data }) => (isMagic(data) ? action : undefined);
}

await failureTranscript(
  "interrupted-before-magic.json",
  "Connection lost while writing the uncommitted index, before any validity magic was sent. Previous generation stays active.",
  (mock) => {
    mock.opts.onWrite = ({ data, blk }) =>
      blk <= 1 && !isMagic(data) ? { apply: "none", ack: "none", disconnect: true } : undefined;
  },
);

await failureTranscript(
  "magic-applied-ack-lost.json",
  "The complete final magic block reached storage but the acknowledgement was lost. After reconnect the new generation is detected as committed.",
  (mock) => onMagic(mock, { apply: "full", ack: "none", disconnect: true }),
);

await failureTranscript(
  "torn-invalid-magic.json",
  "A torn program cycle landed the magic but corrupted the record body: CRC fails, the record is rejected, the previous generation stays active.",
  (mock) =>
    onMagic(mock, {
      apply: "partial",
      partialBytes: 200,
      ack: "ok",
      disconnect: true,
      mangle: (d) => {
        d[100] = d[100]! ^ 0xff;
        return d;
      },
    }),
);

/* ---------- 8. corrupt-newest-index fallback ---------- */

{
  const mock = new MockSp1({ stemTape: true, sectorsPerSong: 8 });
  const { t } = await attach(mock);
  await t.initialiseLibrary();
  await t.uploadSong({ song: songOne });
  const r2 = await t.uploadSong({ song: songTwo });
  const blk = caps.index[r2.targetIndexSlot!].start;
  const bad = mock.block(blk).slice(0);
  bad[IX_OFF.crc32] = bad[IX_OFF.crc32]! ^ 0xff;
  mock.blocks.set(blk, bad);
  const after = mock.reboot();
  const { t: t2 } = await attach(after);
  const lib = (await t2.readLibrary())!;
  writeTranscript("corrupt-newest-index-fallback.json", {
    note: "The newest committed index record is corrupted (one CRC byte flipped). The shared selector falls back to the immediately previous complete song. No initialization is required.",
    corruptedIndexSlot: r2.targetIndexSlot,
    corruptedGeneration: r2.generation,
    selected: {
      status: lib.status,
      requiresInitialization: lib.requiresInitialization,
      activeIndexSlot: lib.activeIndexSlot,
      activeSongSlot: lib.activeSongSlot,
      generation: lib.generation,
      title: lib.active?.title ?? null,
      explanation: lib.explanation,
    },
  });
}

/* ---------- 9. manifests ---------- */

files.sort((a, b) => a.path.localeCompare(b.path));
const sha = files
  .map((f) => `${createHash("sha256").update(Buffer.from(f.bytes)).digest("hex")}  ${f.path}`)
  .join("\n");
writeFileSync(`${OUT}/SHA256SUMS.txt`, sha + "\n");
const crc = files
  .map((f) => `${crc32(f.bytes).toString(16).padStart(8, "0")}  ${f.path}`)
  .join("\n");
writeFileSync(`${OUT}/CRC32SUMS.txt`, crc + "\n");

console.log(`${files.length} fixture files written under ${OUT}`);
console.log(sha);
