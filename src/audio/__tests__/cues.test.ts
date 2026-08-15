import { describe, expect, it } from "vitest";
import {
  CueStore,
  MIN_CUE_FRAMES,
  RATE_TOLERANCE,
  learnRejection,
  resolveQualifier,
  describeReason,
  type CueEventContext,
  type CueLane,
  type EligibilitySnapshot,
  type QualifierSnapshot,
} from "../cues";
import type { StemMidiEvent } from "../midi/contract";

const SR = 48000;
const HASHES = ["h-vocals", "h-drums", "h-bass", "h-instruments"];

const OK: EligibilitySnapshot = {
  headsActive: false,
  scrubActive: false,
  reverseActive: false,
  loopActive: false,
  transportPlaying: true,
  rate: 1,
};

function ev(
  kind: StemMidiEvent["kind"],
  note: number,
  channel = 0,
  timestampMs = 0,
): StemMidiEvent {
  return {
    kind,
    note,
    velocity: kind === "noteOn" ? 100 : 0,
    channel,
    timestampMs,
    source: "test",
    deviceId: "test",
    deviceName: "test",
  };
}

function ctx(
  frame: number,
  q: Partial<QualifierSnapshot> = {},
  e: Partial<EligibilitySnapshot> = {},
  hashes: readonly string[] = HASHES,
): CueEventContext {
  return {
    frame,
    qualifiers: { functionHeld: false, tracksHeld: [], ...q },
    eligibility: { ...OK, ...e },
    sampleRate: SR,
    contentHashes: hashes,
  };
}

const fn = { functionHeld: true } as Partial<QualifierSnapshot>;
const track = (l: CueLane) => ({ tracksHeld: [l] }) as Partial<QualifierSnapshot>;

describe("qualifier resolution", () => {
  it("FUNCTION held → global learn", () => {
    expect(resolveQualifier({ functionHeld: true, tracksHeld: [] })).toEqual({ kind: "global" });
  });
  it("exactly one Track held → isolated learn on that lane", () => {
    expect(resolveQualifier({ functionHeld: false, tracksHeld: [2] })).toEqual({
      kind: "lane",
      lane: 2,
    });
  });
  it("FUNCTION wins when FUNCTION and Tracks are both held", () => {
    expect(resolveQualifier({ functionHeld: true, tracksHeld: [1, 3] })).toEqual({ kind: "global" });
  });
  it("multiple Tracks without FUNCTION → reject", () => {
    expect(resolveQualifier({ functionHeld: false, tracksHeld: [0, 1] })).toEqual({ kind: "reject" });
  });
  it("duplicate Track entries still count as one lane", () => {
    expect(resolveQualifier({ functionHeld: false, tracksHeld: [3, 3] })).toEqual({
      kind: "lane",
      lane: 3,
    });
  });
  it("no qualifier → none", () => {
    expect(resolveQualifier({ functionHeld: false, tracksHeld: [] })).toEqual({ kind: "none" });
  });
});

describe("learn eligibility", () => {
  it("aligned forward playback is eligible", () => {
    expect(learnRejection(OK)).toBeNull();
  });
  const cases: [string, Partial<EligibilitySnapshot>, string][] = [
    ["Heads", { headsActive: true }, "heads-active"],
    ["scrub", { scrubActive: true }, "scrub-active"],
    ["reverse", { reverseActive: true }, "reverse-active"],
    ["loop", { loopActive: true }, "loop-active"],
    ["stopped transport", { transportPlaying: false }, "transport-stopped"],
    ["off-1x rate", { rate: 1.25 }, "rate-not-1x"],
    ["reverse rate", { rate: -1 }, "rate-not-1x"],
    ["zero rate", { rate: 0 }, "rate-not-1x"],
  ];
  for (const [label, patch, reason] of cases) {
    it(`rejects during ${label} with reason ${reason}`, () => {
      expect(learnRejection({ ...OK, ...patch })).toBe(reason);
      const s = new CueStore();
      const a = s.handle(ev("noteOn", 60), ctx(0, fn, patch));
      expect(a).toEqual({ type: "learn.reject", key: "0:60", reason });
      expect(s.openCaptures()).toEqual([]);
    });
  }
  it("tolerates rate within tolerance", () => {
    expect(learnRejection({ ...OK, rate: 1 + RATE_TOLERANCE / 2 })).toBeNull();
  });
});

describe("capture and commit", () => {
  it("Note On starts a global capture and matching Note Off commits", () => {
    const s = new CueStore(() => 1234);
    expect(s.handle(ev("noteOn", 60), ctx(1000, fn))).toEqual({
      type: "learn.start",
      key: "0:60",
      scope: "global",
      lane: null,
      startFrame: 1000,
    });
    const a = s.handle(ev("noteOff", 60), ctx(1000 + MIN_CUE_FRAMES));
    expect(a.type).toBe("learn.commit");
    const m = s.get("0:60")!;
    expect(m).toMatchObject({
      scope: "global",
      lane: null,
      startFrame: 1000,
      endFrame: 1000 + MIN_CUE_FRAMES,
      sampleRate: SR,
      createdAt: 1234,
      invalidReason: null,
    });
    expect(m.sources.map((x) => x.lane)).toEqual([0, 1, 2, 3]);
    expect(s.openCaptures()).toEqual([]);
  });

  it("isolated capture records only its lane's hash", () => {
    const s = new CueStore();
    s.handle(ev("noteOn", 61), ctx(0, track(2)));
    s.handle(ev("noteOff", 61), ctx(50000));
    expect(s.get("0:61")).toMatchObject({ scope: "lane", lane: 2 });
    expect(s.get("0:61")!.sources).toEqual([{ lane: 2, contentHash: "h-bass" }]);
  });

  it("discards a capture shorter than 1024 frames", () => {
    const s = new CueStore();
    s.handle(ev("noteOn", 62), ctx(0, fn));
    expect(s.handle(ev("noteOff", 62), ctx(MIN_CUE_FRAMES - 1))).toEqual({
      type: "learn.discard",
      key: "0:62",
      reason: "too-short",
    });
    expect(s.get("0:62")).toBeNull();
  });

  it("accepts exactly the minimum length", () => {
    const s = new CueStore();
    s.handle(ev("noteOn", 62), ctx(0, fn));
    expect(s.handle(ev("noteOff", 62), ctx(MIN_CUE_FRAMES)).type).toBe("learn.commit");
  });

  it("relearning the same key overwrites in place", () => {
    const s = new CueStore();
    s.handle(ev("noteOn", 63), ctx(0, fn));
    s.handle(ev("noteOff", 63), ctx(100000));
    s.handle(ev("noteOn", 63), ctx(500000, track(1)));
    s.handle(ev("noteOff", 63), ctx(700000));
    expect(s.list()).toHaveLength(1);
    expect(s.get("0:63")).toMatchObject({ scope: "lane", lane: 1, startFrame: 500000, endFrame: 700000 });
  });

  it("concurrent keys stay independent", () => {
    const s = new CueStore();
    s.handle(ev("noteOn", 64, 0), ctx(0, fn));
    s.handle(ev("noteOn", 64, 1), ctx(2000, track(3)));
    expect(s.openCaptures().sort()).toEqual(["0:64", "1:64"]);
    s.handle(ev("noteOff", 64, 1), ctx(90000));
    expect(s.openCaptures()).toEqual(["0:64"]);
    s.handle(ev("noteOff", 64, 0), ctx(50000));
    expect(s.get("0:64")).toMatchObject({ scope: "global", startFrame: 0, endFrame: 50000 });
    expect(s.get("1:64")).toMatchObject({ scope: "lane", lane: 3, startFrame: 2000, endFrame: 90000 });
  });

  it("a Note Off with no matching capture and no marker is ignored", () => {
    const s = new CueStore();
    expect(s.handle(ev("noteOff", 70), ctx(0))).toEqual({
      type: "ignored",
      key: "0:70",
      reason: "note-off-unmatched",
    });
  });
});

describe("mid-capture invalidation", () => {
  it("discards on the Note Off if a condition began mid-capture", () => {
    const s = new CueStore();
    s.handle(ev("noteOn", 65), ctx(0, fn));
    expect(s.handle(ev("noteOff", 65), ctx(200000, {}, { loopActive: true }))).toEqual({
      type: "learn.discard",
      key: "0:65",
      reason: "loop-active",
    });
    expect(s.get("0:65")).toBeNull();
  });

  it("syncEligibility discards every open capture with the reason", () => {
    const s = new CueStore();
    s.handle(ev("noteOn", 66), ctx(0, fn));
    s.handle(ev("noteOn", 67, 1), ctx(0, track(0)));
    expect(s.syncEligibility(OK)).toEqual([]);
    const out = s.syncEligibility({ ...OK, headsActive: true });
    expect(out.map((a) => (a as { reason: string }).reason)).toEqual(["heads-active", "heads-active"]);
    expect(s.openCaptures()).toEqual([]);
  });

  it("all notes off clears open captures", () => {
    const s = new CueStore();
    s.handle(ev("noteOn", 68), ctx(0, fn));
    expect(s.handle(ev("allNotesOff", 0), ctx(0))).toMatchObject({ reason: "all-notes-off" });
    expect(s.openCaptures()).toEqual([]);
  });

  it("cancelAllCaptures reports each discarded key", () => {
    const s = new CueStore();
    s.handle(ev("noteOn", 69), ctx(0, fn));
    expect(s.cancelAllCaptures()).toEqual([
      { type: "learn.discard", key: "0:69", reason: "cancelled" },
    ]);
  });
});

describe("playback", () => {
  function learn(s: CueStore, note: number, q: Partial<QualifierSnapshot>) {
    s.handle(ev("noteOn", note), ctx(10000, q));
    s.handle(ev("noteOff", note), ctx(100000));
  }

  it("unqualified Note On for a learned key returns a one-shot playback request", () => {
    const s = new CueStore();
    learn(s, 72, fn);
    const a = s.handle(ev("noteOn", 72), ctx(0));
    expect(a.type).toBe("cue.play");
    expect((a as { marker: { startFrame: number } }).marker.startFrame).toBe(10000);
  });

  it("playback Note Off does nothing", () => {
    const s = new CueStore();
    learn(s, 73, fn);
    s.handle(ev("noteOn", 73), ctx(0));
    expect(s.handle(ev("noteOff", 73), ctx(5000))).toEqual({
      type: "ignored",
      key: "0:73",
      reason: "playback-note-off",
    });
    expect(s.get("0:73")).toMatchObject({ startFrame: 10000, endFrame: 100000 });
  });

  it("unqualified Note On for an unlearned key is ignored", () => {
    const s = new CueStore();
    expect(s.handle(ev("noteOn", 74), ctx(0))).toEqual({
      type: "ignored",
      key: "0:74",
      reason: "unlearned",
    });
  });

  it("playback is allowed in states where learning is rejected", () => {
    const s = new CueStore();
    learn(s, 75, fn);
    for (const patch of [
      { loopActive: true },
      { transportPlaying: false },
      { rate: 1.5 },
    ]) {
      expect(s.handle(ev("noteOn", 75), ctx(0, {}, patch)).type).toBe("cue.play");
    }
  });
});

describe("contentHash invalidation", () => {
  function seed() {
    const s = new CueStore();
    s.handle(ev("noteOn", 80), ctx(0, fn));
    s.handle(ev("noteOff", 80), ctx(100000)); // global
    s.handle(ev("noteOn", 81), ctx(0, track(1)));
    s.handle(ev("noteOff", 81), ctx(100000)); // isolated lane 1
    s.handle(ev("noteOn", 82), ctx(0, track(3)));
    s.handle(ev("noteOff", 82), ctx(100000)); // isolated lane 3
    return s;
  }

  it("one changed lane invalidates its isolated cues and every global cue", () => {
    const s = seed();
    const next = [...HASHES];
    next[1] = "h-drums-v2";
    expect(s.revalidate(next).invalidated.sort()).toEqual(["0:80", "0:81"]);
    expect(s.get("0:80")!.invalidReason).toBe("source-replaced");
    expect(s.get("0:81")!.invalidReason).toBe("source-replaced");
    expect(s.get("0:82")!.invalidReason).toBeNull();
  });

  it("invalid markers are retained and reject playback with a reason", () => {
    const s = seed();
    const next = [...HASHES];
    next[1] = "h-drums-v2";
    s.revalidate(next);
    expect(s.list()).toHaveLength(3);
    expect(s.handle(ev("noteOn", 81), ctx(0))).toEqual({
      type: "cue.reject",
      key: "0:81",
      reason: "source-replaced",
    });
  });

  it("revalidate is idempotent and restores when the hash returns", () => {
    const s = seed();
    const next = [...HASHES];
    next[3] = "h-instruments-v2";
    expect(s.revalidate(next).invalidated.sort()).toEqual(["0:80", "0:82"]);
    expect(s.revalidate(next).invalidated).toEqual([]);
    expect(s.revalidate(HASHES).restored.sort()).toEqual(["0:80", "0:82"]);
  });

  it("load restores markers and invalidation runs against them", () => {
    const s = seed();
    const s2 = new CueStore();
    s2.load(s.list());
    expect(s2.list()).toHaveLength(3);
    const next = [...HASHES];
    next[0] = "x";
    expect(s2.revalidate(next).invalidated.sort()).toEqual(["0:80"]);
  });
});

describe("reason text", () => {
  it("names every rejection", () => {
    expect(describeReason("multiple-tracks-held")).toMatch(/one Track button/);
    expect(describeReason("source-replaced")).toBe("source replaced");
    expect(describeReason("too-short")).toMatch(/minimum length/);
    expect(describeReason("heads-active")).toMatch(/Heads/);
  });
});

describe("multiple tracks held", () => {
  it("rejects learning and does not open a capture", () => {
    const s = new CueStore();
    expect(s.handle(ev("noteOn", 90), ctx(0, { tracksHeld: [0, 2] }))).toEqual({
      type: "learn.reject",
      key: "0:90",
      reason: "multiple-tracks-held",
    });
    expect(s.openCaptures()).toEqual([]);
    expect(s.list()).toEqual([]);
  });
});
