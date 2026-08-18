/**
 * Audit helper (not shipped). Runs the SAME production preparation modules in
 * the node runner with the deterministic offline stub, so the browser result
 * produced by tools/prepare-harness can be compared field by field.
 */
import { writeFileSync } from "node:fs";
import { prepareCanonicalSong } from "../src/sp1/song";
import { encodeSong, decodeSectors } from "../src/sp1/sector";
import { sha256Hex } from "../src/sp1/digest";
import { STEM_ORDER } from "../src/sp1/prepare";
import { decodeWavFixture, offlineStub } from "../src/sp1/__tests__/fixtureWav";

const inputs = STEM_ORDER.map((name) => ({
  name,
  filename: `${name}.wav`,
  buffer: decodeWavFixture(name),
}));

const song = await prepareCanonicalSong(inputs, {
  metadata: { title: "audit", artist: "audit", bpm: 120, downbeatSeconds: 0 },
  make: offlineStub,
});
const sectors = encodeSong(song);
const decoded = decodeSectors(sectors, song.frames);

const stems = [];
for (let i = 0; i < song.stems.length; i++) {
  const s = song.stems[i]!;
  stems.push({
    name: s.name,
    sourceRate: s.source.sampleRate,
    sourceChannels: s.source.channels,
    sourceFrames: s.source.frames,
    resampled: s.source.sampleRate !== 48000,
    frames: s.frames,
    originalFrames: s.originalFrames,
    padFrames: s.padFrames,
    pcmBytes: s.pcm24.length,
    peak: s.peak,
    clipped: s.clipped,
    checksum: s.checksum >>> 0,
    sha256: await sha256Hex([s.pcm24]),
    roundTripSha256: await sha256Hex([decoded.stems[i]!]),
  });
}

const out = {
  schemaVersion: song.schemaVersion,
  sampleRate: song.sampleRate,
  channels: song.channels,
  pcmDepth: song.pcmDepth,
  frames: song.frames,
  durationSeconds: song.durationSeconds,
  audioBytes: song.audioBytes,
  lengthSpreadSeconds: song.lengthSpreadSeconds,
  songChecksum: song.checksum >>> 0,
  sectorCount: sectors.length,
  sectorBytes: sectors.reduce((a, s) => a + s.length, 0),
  sectorsSha256: await sha256Hex(sectors),
  roundTripFrames: decoded.frames,
  roundTripBpm: decoded.bpm,
  stems,
};

writeFileSync("/tmp/prepare-expected.json", JSON.stringify(out, null, 1));
console.log(JSON.stringify(out, null, 1));
