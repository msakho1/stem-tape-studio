/**
 * Focused regression coverage for the repaired SP-1 flight recorder.
 *
 * These tests exist because the previous export was untrustworthy: gestures
 * were reconstructed at 4 Hz (15,669 ms of holds inside a 0.6 ms span), the
 * ring flooded with identical `led.derived` records, and static contract audit
 * findings were exported as failed reproductions.
 */

import { describe, expect, it, vi } from "vitest";
import { TraceRing } from "../trace";
import {
  PhysicalLedTransport,
  matchOutputsForInput,
  normalizeMidiIdentity,
  LED_STATUS,
  CC_LED_BASE,
  CC_COMMIT,
  CC_HEARTBEAT,
  CC_CAPABILITY,
  CC_RELEASE,
  type MidiOutLike,
} from "../ledTransport";
import { resolvePhysicalFrame, formatPhysicalFrame } from "../physicalFrame";
import { evaluateContract } from "../contract";
import { SegmentRunner, SEGMENT_DEFINITIONS } from "../segments";
import { inspectPhysicalLeds, inspectWebOnlyIndicators } from "../leds";
import { PHYSICAL_LED_IDS, UI_ONLY_INDICATOR_IDS } from "../physical";
import { Sp1SurfaceAdapter } from "@/audio/midi/sp1Surface";
import { trace } from "../trace";
import type { LedFrame, LedId, LedPattern } from "@/machine/surface";

const ALL_LED_IDS = [...PHYSICAL_LED_IDS, ...UI_ONLY_INDICATOR_IDS] as LedId[];

function frame(overrides: Partial<Record<LedId, LedPattern>> = {}, reason = "idle"): LedFrame {
  const out = {} as LedFrame;
  for (const id of ALL_LED_IDS) {
    out[id] = { pattern: overrides[id] ?? "dark", reason, priority: 1 };
  }
  return out;
}

const body = () => trace.list().filter((r) => r.stage !== "capture.control");

describe("capture epochs", () => {
  it("starting capture never imports events that happened before it", () => {
    const r = new TraceRing(50);
    r.record("midi.raw", "before capture"); // disabled ring: dropped entirely
    expect(r.list()).toHaveLength(0);
    r.startCapture();
    expect(r.list().filter((x) => x.label === "before capture")).toHaveLength(0);
    r.record("midi.raw", "after capture");
    expect(r.list().filter((x) => x.label === "after capture")).toHaveLength(1);
  });

  it("clearing empties the ring, opens a new epoch and replays nothing", () => {
    const r = new TraceRing(50);
    const first = r.startCapture();
    r.record("midi.raw", "old");
    r.clear();
    expect(r.list().filter((x) => x.label === "old")).toHaveLength(0);
    expect(r.currentCaptureId()).toBeGreaterThan(first);
    r.record("midi.raw", "new");
    const list = r.list().filter((x) => x.stage === "midi.raw");
    expect(list).toHaveLength(1);
    expect(list[0]!.label).toBe("new");
  });

  it("STOP CAPTURE causes zero later additions", () => {
    const r = new TraceRing(50);
    r.startCapture();
    r.record("midi.raw", "a");
    const before = r.size();
    r.stopCapture();
    for (let i = 0; i < 100; i++) r.record("midi.raw", `later ${i}`);
    expect(r.size()).toBe(before + 1); // only the "stopped" control record
    expect(r.list().some((x) => x.label.startsWith("later"))).toBe(false);
  });

  it("sequences are strictly increasing", () => {
    const r = new TraceRing(200);
    r.startCapture();
    for (let i = 0; i < 50; i++) r.record("midi.raw", `m${i}`);
    const seqs = r.list().map((x) => x.seq);
    for (let i = 1; i < seqs.length; i++) expect(seqs[i]!).toBeGreaterThan(seqs[i - 1]!);
  });
});

describe("real event-time capture — no reconstruction", () => {
  it("preserves a 1200 ms hold between the real DOWN and UP records", () => {
    trace.startCapture();
    const adapter = new Sp1SurfaceAdapter();
    const device = { id: "in-1", name: "STEM TAPE SP-1" };
    adapter.handleBytes([0x90, 40, 127], device, 1_000);
    adapter.handleBytes([0x80, 40, 0], device, 2_200);
    const decoded = body().filter((r) => r.stage === "surface.decoded");
    expect(decoded).toHaveLength(2);
    expect(decoded[1]!.t - decoded[0]!.t).toBeCloseTo(1200, 3);
    trace.stopCapture();
    trace.clear();
  });

  it("multiple seconds of holds cannot land inside a sub-millisecond span", () => {
    trace.startCapture();
    const adapter = new Sp1SurfaceAdapter();
    const device = { id: "in-1", name: "STEM TAPE SP-1" };
    let t = 0;
    for (const note of [36, 37, 38]) {
      adapter.handleBytes([0x90, note, 127], device, t);
      t += 5_223;
      adapter.handleBytes([0x80, note, 0], device, t);
      t += 10;
    }
    const decoded = body().filter((r) => r.stage === "surface.decoded");
    const span = decoded[decoded.length - 1]!.t - decoded[0]!.t;
    expect(span).toBeGreaterThan(15_000);
    trace.stopCapture();
    trace.clear();
  });

  it("keeps one correlation ID across decode → arbitration → command → ack", () => {
    const r = new TraceRing(50);
    r.startCapture();
    const corr = r.beginCorrelation();
    r.record("surface.decoded", "PLAY DOWN");
    r.record("gesture.arbitration", "play → transport");
    r.record("command.surface", "transport.play", undefined, { commandId: 7 });
    r.record("engine.ack", "transport.play → accepted", undefined, { commandId: 7 });
    const stages = r.list().filter((x) => x.stage !== "capture.control");
    expect(stages.map((x) => x.stage)).toEqual([
      "surface.decoded",
      "gesture.arbitration",
      "command.surface",
      "engine.ack",
    ]);
    for (const rec of stages) expect(rec.corr).toBe(corr);
    expect(stages[2]!.commandId).toBe(7);
    expect(stages[3]!.commandId).toBe(7);
  });
});

describe("idle traffic never floods the ring", () => {
  it("deduplicates identical led.derived frames", () => {
    const r = new TraceRing(500);
    r.startCapture();
    const resolved = resolvePhysicalFrame(frame({ "track-led-3": "solid" }));
    for (let i = 0; i < 40; i++) {
      r.recordIfChanged("led.derived", resolved.signature, "led.derived", formatPhysicalFrame(resolved));
    }
    expect(r.list().filter((x) => x.stage === "led.derived")).toHaveLength(1);
    expect(r.stats().coalesced).toBe(39);
  });

  it("ten seconds of idle 250 ms LED polling adds zero records", () => {
    const r = new TraceRing(500);
    r.startCapture();
    const resolved = resolvePhysicalFrame(frame());
    r.recordIfChanged("led.derived", resolved.signature, "led.derived", formatPhysicalFrame(resolved));
    const after = r.size();
    for (let i = 0; i < 40; i++) {
      r.recordIfChanged("led.derived", resolved.signature, "led.derived", formatPhysicalFrame(resolved));
    }
    expect(r.size()).toBe(after);
  });

  it("animation and heartbeat traffic coalesce and cannot evict gesture records", () => {
    trace.startCapture();
    const sent: number[][] = [];
    const out: MidiOutLike = { id: "o1", name: "STEM TAPE SP-1", send: (b) => sent.push(b) };
    const t = new PhysicalLedTransport();
    t.setInput("STEM TAPE SP-1");
    t.selectOutput(out);
    t.queryCapability();
    t.handleDeviceCc(CC_CAPABILITY, 1);
    trace.record("gesture.owner", "owner transport"); // the record that must survive
    for (let i = 0; i < 300; i++) t.presentAnimationFrame([i % 128, 0, 0, 0, 0, 0, 0, 0]);
    for (let i = 0; i < 300; i++) t.heartbeat();
    t.flushAnimation();
    const records = trace.list();
    expect(records.some((r) => r.label === "owner transport")).toBe(true);
    const summaries = records.filter((r) => r.stage === "led.transmitted" && /coalesced/.test(r.label));
    expect(summaries).toHaveLength(1);
    expect(summaries[0]!.data!["frames"]).toBe(300);
    expect(records.length).toBeLessThan(20);
    trace.stopCapture();
    trace.clear();
  });

  it("renders a readable eight-value frame instead of `physical frame: 8 leds`", () => {
    const resolved = resolvePhysicalFrame(
      frame({ "track-led-3": "solid", "side-led-1": "solid" }, "fx.momentary"),
    );
    expect(formatPhysicalFrame(resolved)).toBe(
      "[T1 0, T2 0, T3 127, T4 0 | S1 127, S2 0, S3 0, S4 0] owner=fx.momentary",
    );
  });
});

describe("immutable export", () => {
  it("snapshot is a point-in-time copy that later capture cannot mutate", () => {
    const r = new TraceRing(50);
    r.startCapture();
    r.record("midi.raw", "one", { bytes: [1, 2, 3] });
    const snap = r.snapshot();
    r.record("midi.raw", "two");
    (snap.records[0]!.data as Record<string, unknown>)["bytes"] = "tampered";
    expect(snap.records.filter((x) => x.stage === "midi.raw")).toHaveLength(1);
    expect(r.list().find((x) => x.label === "one")!.data!["bytes"]).toEqual([1, 2, 3]);
  });
});

describe("host→device LED protocol v1", () => {
  const makeOut = (name: string) => {
    const sent: number[][] = [];
    return { out: { id: `out-${name}`, name, send: (b: number[]) => sent.push(b) }, sent };
  };

  it("matches an output by normalized identity, never by equal IDs", () => {
    expect(normalizeMidiIdentity("STEM TAPE SP-1 MIDI OUT")).toBe("STEM TAPE SP 1");
    const outs = [
      { id: "out-9", name: "STEM TAPE SP-1 MIDI Out" },
      { id: "out-2", name: "Some Other Synth" },
    ];
    const matched = matchOutputsForInput("STEM TAPE SP-1 MIDI In", outs);
    expect(matched.map((o) => o.id)).toEqual(["out-9"]);
    expect(matched[0]!.id).not.toBe("in-1");
  });

  it("gates transmission on the CC91 protocol-v1 response", () => {
    const { out, sent } = makeOut("STEM TAPE SP-1");
    const t = new PhysicalLedTransport();
    t.setInput("STEM TAPE SP-1");
    t.selectOutput(out);
    const resolved = resolvePhysicalFrame(frame({ "track-led-1": "solid" }));
    expect(t.present(resolved)).toBe("blocked");
    t.queryCapability();
    expect(sent[0]).toEqual([LED_STATUS, CC_CAPABILITY, 0]);
    expect(t.present(resolved)).toBe("blocked");
    t.handleDeviceCc(CC_CAPABILITY, 1);
    expect(t.present(resolved)).toBe("sent");
  });

  it("stages all eight channels before the first commit, then only changes", () => {
    const { out, sent } = makeOut("STEM TAPE SP-1");
    const t = new PhysicalLedTransport();
    t.setInput("STEM TAPE SP-1");
    t.selectOutput(out);
    t.queryCapability();
    t.handleDeviceCc(CC_CAPABILITY, 1);
    sent.length = 0;

    t.present(resolvePhysicalFrame(frame({ "track-led-1": "solid" })));
    const staged = sent.filter((m) => m[1]! >= CC_LED_BASE && m[1]! < CC_LED_BASE + 8);
    expect(staged).toHaveLength(8);
    expect(sent[sent.length - 1]).toEqual([LED_STATUS, CC_COMMIT, 1]);

    sent.length = 0;
    t.present(resolvePhysicalFrame(frame({ "track-led-1": "solid", "side-led-2": "solid" })));
    const staged2 = sent.filter((m) => m[1]! >= CC_LED_BASE && m[1]! < CC_LED_BASE + 8);
    expect(staged2).toHaveLength(1);
    expect(staged2[0]![1]).toBe(CC_LED_BASE + 5);
    expect(sent[sent.length - 1]).toEqual([LED_STATUS, CC_COMMIT, 2]);
  });

  it("unchanged frames produce heartbeats but no commits", () => {
    const { out, sent } = makeOut("STEM TAPE SP-1");
    const t = new PhysicalLedTransport();
    t.setInput("STEM TAPE SP-1");
    t.selectOutput(out);
    t.queryCapability();
    t.handleDeviceCc(CC_CAPABILITY, 1);
    const resolved = resolvePhysicalFrame(frame({ "track-led-1": "solid" }));
    t.present(resolved);
    sent.length = 0;
    expect(t.present(resolved)).toBe("unchanged");
    t.heartbeat();
    t.heartbeat();
    expect(sent.filter((m) => m[1] === CC_COMMIT)).toHaveLength(0);
    expect(sent.filter((m) => m[1] === CC_HEARTBEAT)).toHaveLength(2);
    expect(sent.every((m) => m[2] === t.snapshot().commitSequence)).toBe(true);
  });

  it("releases host ownership on detach", () => {
    const { out, sent } = makeOut("STEM TAPE SP-1");
    const t = new PhysicalLedTransport();
    t.setInput("STEM TAPE SP-1");
    t.selectOutput(out);
    t.queryCapability();
    t.handleDeviceCc(CC_CAPABILITY, 1);
    t.present(resolvePhysicalFrame(frame({ "track-led-1": "solid" })));
    sent.length = 0;
    t.release("test");
    expect(sent).toContainEqual([LED_STATUS, CC_RELEASE, 0]);
  });

  it("web-only indicators are structurally impossible to transmit", () => {
    const resolved = resolvePhysicalFrame(frame({ "play-indicator": "solid", "function-led-1": "solid" }));
    expect(resolved.values).toHaveLength(8);
    expect(resolved.leds.map((l) => l.id)).toEqual([...PHYSICAL_LED_IDS]);
    expect(resolved.values.every((v) => v === 0)).toBe(true);
  });
});

describe("inspector column separation", () => {
  it("never exports DOM state as observed physical state", () => {
    const rows = inspectPhysicalLeds(frame({ "track-led-1": "solid" }), frame({ "track-led-1": "solid" }), [
      { id: "track-led-1", className: "st-led st-led--solid", animationName: "none", opacity: "1" },
    ]);
    expect(rows[0]!.columnDom).toContain("class solid");
    expect(rows[0]!.physicalObservation).toBe("unknown");
    expect(rows[0]!.columnTransmitted).toBe("not transmitted");
  });

  it("marks firmware-reported values as PWM command state, not illumination", () => {
    const rows = inspectPhysicalLeds(frame(), frame(), [], {}, {
      transmitted: [127, 0, 0, 0, 0, 0, 0, 0],
      firmwareReported: [127, 0, 0, 0, 0, 0, 0, 0],
    });
    expect(rows[0]!.columnTransmitted).toContain("midi 127");
    expect(rows[0]!.physicalObservation).toBe("firmware-reported (PWM command, not illumination)");
  });

  it("web-only indicators are labelled as never transmitted", () => {
    const rows = inspectWebOnlyIndicators(frame(), frame(), []);
    expect(rows.every((r) => r.columnTransmitted.includes("never transmitted"))).toBe(true);
  });
});

describe("contract reference data vs reproductions", () => {
  it("stays not-run with source-audit observation until a segment executes", () => {
    const results = evaluateContract(null);
    expect(results.every((c) => c.reproductionStatus === "not-run")).toBe(true);
    expect(results.every((c) => c.observationSource === "source-audit")).toBe(true);
    expect(results.every((c) => c.reproductionFirstDivergence === null)).toBe(true);
  });

  it("records a first-divergence stage only for a reproduction that really ran", () => {
    const runner = new SegmentRunner();
    const def = SEGMENT_DEFINITIONS[0]!;
    runner.begin(def, "stopped", "browser-injection");
    runner.observeStage("surface.decoded");
    runner.observeStage("gesture.arbitration");
    const res = runner.end("stopped")!;
    expect(res.status).toBe("failed");
    expect(res.lastSuccessfulStage).toBe("gesture.arbitration");
    expect(res.firstMissingStage).toBe("command.surface");
    const evaluated = evaluateContract(null, (id) => runner.resultFor(id));
    const entry = evaluated.find((c) => c.id === def.contractId);
    if (entry) {
      expect(entry.reproductionStatus).toBe("failed");
      expect(entry.observationSource).toBe("browser-injection");
    }
  });
});

describe("firmware identity is never back-filled", () => {
  it("keeps reportedFirmwareVersion null while the console is closed", async () => {
    const { firmwareConsole } = await import("../firmwareConsole");
    const snap = firmwareConsole.snapshot();
    expect(snap.status).not.toBe("connected");
    expect(snap.reportedVersion).toBeNull();
  });
});

describe("closed drawer performs no diagnostic work", () => {
  it("a disabled ring rejects every record with no listener churn", () => {
    const r = new TraceRing(50);
    const spy = vi.fn();
    r.subscribe(spy);
    for (let i = 0; i < 100; i++) r.record("led.derived", "tick");
    expect(r.size()).toBe(0);
    expect(spy).not.toHaveBeenCalled();
  });
});
