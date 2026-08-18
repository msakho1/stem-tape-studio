/**
 * Audit harness (not part of the shipped app).
 *
 * Loads the UNMODIFIED production preparation modules, runs them in a real
 * browser against the four committed WAV fixtures using the real
 * AudioContext.decodeAudioData and the real OfflineAudioContext, and exposes
 * the resulting canonical numbers + SHA-256 digests on window for Playwright.
 *
 * Built with `vite build --mode production`, so the code under test is the
 * same production output shape the app ships.
 */
import { prepareCanonicalSong, assertCanonicalSong, type CanonicalSong } from "../../src/sp1/song";
import { encodeSong, decodeSectors } from "../../src/sp1/sector";
import { sha256Hex } from "../../src/sp1/digest";
import { STEM_ORDER, type StemSlotName } from "../../src/sp1/prepare";

declare global {
  interface Window {
    __PREPARE_RESULT__?: unknown;
    __PREPARE_ERROR__?: string;
    runPrepare?: () => Promise<unknown>;
  }
}

async function run() {
  const ac = new AudioContext({ sampleRate: 48000 });
  const inputs = [];
  for (const name of STEM_ORDER as StemSlotName[]) {
    const res = await fetch(`./fixtures/${name}.wav`);
    const bytes = await res.arrayBuffer();
    const buffer = await ac.decodeAudioData(bytes.slice(0));
    inputs.push({ name, filename: `${name}.wav`, buffer });
  }

  const song: CanonicalSong = await prepareCanonicalSong(inputs, {
    metadata: { title: "audit", bpm: 120, beatZeroSeconds: 0 } as never,
  });
  assertCanonicalSong(song);

  const sectors = encodeSong(song);
  const decoded = decodeSectors(sectors, song.frames);

  const stems = [];
  for (const s of song.stems) {
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
    });
  }

  return {
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
    roundTripFrames: decoded.frames ?? song.frames,
    stems,
    audioContextRate: ac.sampleRate,
  };
}

window.runPrepare = run;
run()
  .then((r) => {
    window.__PREPARE_RESULT__ = r;
    document.getElementById("out")!.textContent = "done";
  })
  .catch((e) => {
    window.__PREPARE_ERROR__ = String((e as Error)?.stack ?? e);
    document.getElementById("out")!.textContent = "error";
  });
