/**
 * Pass L1 acceptance: three-step consumer workflow, filename assignment,
 * title inference, automatic timing, and cacheable chunked preparation.
 */
import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { assignFiles, inferTitle, roleForFilename, stripRoleToken } from "../stemNaming";
import { analyzeTiming, timingLabel, TEMPO_RANGE } from "../autoTiming";
import { fingerprintSources, loadPrepared, memoryStorage, prepareChunked } from "../preparation";
import { prepareCanonicalSong } from "../song";
import { STEM_ORDER, type StemSlotName } from "../prepare";

const route = readFileSync("src/routes/device.tsx", "utf8");

/* -------------------------------------------------- interface guarantees */

describe("device route presents exactly three steps", () => {
  it("has connect, load stems and upload sections only", () => {
    expect(route).toContain('data-testid="step-connect"');
    expect(route).toContain('data-testid="step-stems"');
    expect(route).toContain('data-testid="step-upload"');
    const steps = route.match(/data-testid="step-[a-z]+"/g) ?? [];
    expect(new Set(steps).size).toBe(3);
  });

  it("removes the engineering audit surfaces", () => {
    for (const banned of [
      "song slots",
      "compatibility negotiated",
      "Verify",
      "Show technical detail",
      "Tap tempo",
      "beat zero",
      "generation ·",
      "index blocks",
      "staging ·",
      "Prepare stems",
    ]) {
      expect(route).not.toContain(banned);
    }
  });

  it("keeps the file input off-screen behind a real styled button", () => {
    expect(route).toContain('data-testid="choose-files"');
    expect(route).toContain("Choose files");
    expect(route).toContain("Choose file");
    expect(route).toMatch(/type="file"[\s\S]{0,240}className="sr-only"/);
  });

  it("shows the required success line and failure copy", () => {
    const model = readFileSync("src/device/deviceModel.ts", "utf8");
    expect(route).toContain("Uploaded and verified. Press Play on your SP-1.");
    expect(model).toContain("Upload failed — your existing song is untouched.");
    expect(model).toContain(
      "Reconnect the SP-1 to see which song it is playing.",
    );
    expect(model).toContain("Update the firmware on your SP-1 to continue.");
    expect(route).toContain("Keep the SP-1 connected. Playback is paused while transferring.");
    expect(route).toContain("Uploading replaces the song on the SP-1.");
  });


  it("keeps activity controls", () => {
    const lower = route.toLowerCase();
    expect(lower).toContain("copy activity");
    expect(lower).toContain("diagnostic report");
    expect(lower).toContain("clear activity");
  });

  it("never reads the mock port outside development", () => {
    expect(route).toContain("import.meta.env.DEV");
  });
});

/* ------------------------------------------------------------- filenames */

const file = (name: string, size = 1024, lastModified = 1) =>
  ({ name, size, lastModified }) as unknown as File;

describe("stem assignment", () => {
  it("assigns the four Dilla exports by suffix", () => {
    const names = [
      "Wont do - J Dilla_Vocal.wav",
      "Wont do - J Dilla_Drums.wav",
      "Wont do - J Dilla_Bass.wav",
      "Wont do - J Dilla_Other.wav",
    ];
    const { assigned, ambiguous } = assignFiles(names.map((n) => file(n)));
    expect(ambiguous).toHaveLength(0);
    expect(STEM_ORDER.every((r) => !!assigned[r])).toBe(true);
    expect(assigned.instrument!.name).toContain("Other");
  });

  it("accepts alias spellings", () => {
    const map: [string, StemSlotName][] = [
      ["song vocals.wav", "vocal"],
      ["song-voice.wav", "vocal"],
      ["song drum.wav", "drums"],
      ["song_Bass.wav", "bass"],
      ["song_Instruments.wav", "instrument"],
      ["song music.wav", "instrument"],
    ];
    for (const [name, role] of map) expect(roleForFilename(name)).toBe(role);
  });

  it("reports only the ambiguous file", () => {
    const { assigned, ambiguous } = assignFiles([
      file("x_Vocal.wav"),
      file("x_Drums.wav"),
      file("x_take3.wav"),
    ]);
    expect(Object.keys(assigned).sort()).toEqual(["drums", "vocal"]);
    expect(ambiguous.map((f) => f.name)).toEqual(["x_take3.wav"]);
  });

  it("treats a duplicate claim as ambiguous rather than guessing", () => {
    const { assigned, ambiguous } = assignFiles([file("a_Bass.wav"), file("b_Bass.wav")]);
    expect(assigned.bass).toBeUndefined();
    expect(ambiguous).toHaveLength(2);
  });
});

describe("title inference", () => {
  it("derives the shared prefix", () => {
    expect(
      inferTitle([
        "Wont do - J Dilla_Vocal.wav",
        "Wont do - J Dilla_Drums.wav",
        "Wont do - J Dilla_Bass.wav",
        "Wont do - J Dilla_Other.wav",
      ]),
    ).toBe("Wont do - J Dilla");
  });

  it("strips hyphen and space variants", () => {
    expect(stripRoleToken("Track 2 - Drums.wav")).toBe("Track 2");
    expect(stripRoleToken("Track 2 Vocals.aif")).toBe("Track 2");
  });
});

/* ---------------------------------------------------------------- timing */

function click(bpm: number, seconds: number, rate = 8000, offset = 0.25): Float32Array {
  const out = new Float32Array(Math.round(seconds * rate));
  const period = (60 / bpm) * rate;
  for (let b = 0; ; b++) {
    const start = Math.round(offset * rate + b * period);
    if (start >= out.length) break;
    for (let i = 0; i < 400 && start + i < out.length; i++) {
      out[start + i] = Math.exp(-i / 60) * Math.sin((i / rate) * 2 * Math.PI * 120);
    }
  }
  return out;
}

describe("automatic tempo and downbeat", () => {
  it("detects a fixture tempo within tolerance and is deterministic", () => {
    const drums = click(92, 20);
    const sources = [
      { name: "vocal" as const, channel: new Float32Array(20 * 8000), sampleRate: 8000 },
      { name: "drums" as const, channel: drums, sampleRate: 8000 },
      { name: "bass" as const, channel: new Float32Array(20 * 8000), sampleRate: 8000 },
      { name: "instrument" as const, channel: new Float32Array(20 * 8000), sampleRate: 8000 },
    ];
    const a = analyzeTiming(sources);
    const b = analyzeTiming(sources);
    expect(a).toEqual(b);
    expect(Math.abs(a.bpm - 92)).toBeLessThan(2);
    expect(a.origin).toBe("drums");
    expect(a.downbeatSeconds).toBeGreaterThanOrEqual(0);
    expect(a.bpm).toBeGreaterThanOrEqual(TEMPO_RANGE.min);
    expect(a.bpm).toBeLessThanOrEqual(TEMPO_RANGE.max);
  });

  it("falls back to the combined envelope when drums are silent", () => {
    const t = analyzeTiming([
      { name: "drums", channel: new Float32Array(8000 * 10), sampleRate: 8000 },
      { name: "instrument", channel: click(120, 10), sampleRate: 8000 },
    ]);
    expect(t.origin).toBe("combined");
  });

  it("low confidence still yields a usable value and never blocks", () => {
    const t = analyzeTiming([{ name: "vocal", channel: new Float32Array(64), sampleRate: 8000 }]);
    expect(t.bpm).toBeGreaterThan(0);
    expect(timingLabel(t)).toMatch(/edit if needed|BPM/);
  });

  it("never says beat zero", () => {
    expect(
      timingLabel({
        bpm: 92,
        downbeatSeconds: 0.2,
        confidence: "low",
        origin: "drums",
        edited: false,
      }),
    ).toBe("Tempo estimated at 92 BPM — edit if needed");
    expect(
      timingLabel({
        bpm: 92,
        downbeatSeconds: 0.2,
        confidence: "high",
        origin: "drums",
        edited: false,
      }),
    ).toBe("Detected: 92 BPM");
  });
});

/* ----------------------------------------------------------- preparation */

function buffer(frames: number, rate = 48000, channels = 2): AudioBuffer {
  const data = Array.from({ length: channels }, () => new Float32Array(frames));
  return {
    sampleRate: rate,
    numberOfChannels: channels,
    length: frames,
    duration: frames / rate,
    getChannelData: (c: number) => data[c]!,
  } as unknown as AudioBuffer;
}

async function song(frames = 5000, longer = 7000) {
  return prepareCanonicalSong(
    [
      { name: "vocal", filename: "a_Vocal.wav", buffer: buffer(longer) },
      { name: "drums", filename: "a_Drums.wav", buffer: buffer(frames) },
      { name: "bass", filename: "a_Bass.wav", buffer: buffer(frames) },
      { name: "instrument", filename: "a_Other.wav", buffer: buffer(frames) },
    ],
    { metadata: { title: "a", artist: "", bpm: 92, downbeatSeconds: 0.25 } },
  );
}

describe("chunked preparation and caching", () => {
  it("pads shorter stems to the longest", async () => {
    const s = await song();
    expect(s.frames).toBe(7000);
    for (const stem of s.stems) expect(stem.frames).toBe(7000);
    expect(s.stems.find((x) => x.name === "drums")!.padFrames).toBe(2000);
  });

  it("stores sectors in chunks with per-sector CRCs and reuses them", async () => {
    const storage = memoryStorage();
    const s = await song();
    const fp = fingerprintSources({
      files: { vocal: { name: "a_Vocal.wav", size: 1, lastModified: 1 } },
      timing: { bpm: 92, downbeatSeconds: 0.25 },
      title: "a",
    });
    const m = await prepareChunked({ song: s, fingerprint: fp, storage });
    expect(m.sectorCount).toBeGreaterThan(0);
    expect(m.sectorCrc).toHaveLength(m.sectorCount);
    expect(m.totalBytes).toBe(m.sectorCount * 8192);
    expect(await storage.getChunk(fp, 0)).not.toBeNull();
    expect(await loadPrepared(storage, fp)).toEqual(m);
  });

  it("invalidates the cache when a source file or timing changes", async () => {
    const base = {
      files: { vocal: { name: "a_Vocal.wav", size: 10, lastModified: 5 } },
      timing: { bpm: 92, downbeatSeconds: 0.25 },
      title: "a",
    };
    const fp = fingerprintSources(base);
    expect(fingerprintSources(base)).toBe(fp);
    expect(
      fingerprintSources({
        ...base,
        files: { vocal: { name: "a_Vocal.wav", size: 11, lastModified: 5 } },
      }),
    ).not.toBe(fp);
    expect(fingerprintSources({ ...base, timing: { bpm: 93, downbeatSeconds: 0.25 } })).not.toBe(
      fp,
    );
    expect(fingerprintSources({ ...base, title: "b" })).not.toBe(fp);

    const storage = memoryStorage();
    const s = await song();
    await prepareChunked({ song: s, fingerprint: fp, storage });
    expect(await loadPrepared(storage, fingerprintSources({ ...base, title: "b" }))).toBeNull();
  });
});
