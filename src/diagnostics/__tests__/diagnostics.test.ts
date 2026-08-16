import { describe, expect, it, vi } from "vitest";
import {
  applyFirmwareLine,
  FirmwareConsole,
  parseFirmwareLine,
  type FirmwareConsoleState,
  type SerialPortLike,
} from "../firmwareConsole";
import { TraceRing, formatTraceRow } from "../trace";
import { BEHAVIOR_CONTRACT, BEHAVIOR_CONTRACT_VERSION, evaluateContract } from "../contract";
import {
  inspectLeds,
  inspectPhysicalLeds,
  inspectWebOnlyIndicators,
  LED_IDS,
  modeOf,
} from "../leds";
import {
  M0_IMPLEMENTED_LED_OUTPUTS,
  M0_UNRESOLVED_LED_OUTPUTS,
  PHYSICAL_LED_COUNT,
  PHYSICAL_LED_IDS,
  UI_ONLY_INDICATOR_IDS,
} from "../physical";
import { describeMidiBytes, SP1_MIDI_CONTRACT } from "../midiContract";
import { decodeSp1Message } from "@/audio/midi/sp1Surface";
import { redact, reportToText, type DiagnosticReport } from "../report";
import type { LedFrame, LedPattern } from "@/machine/surface";

const BANNER = "Stem Tape M0 v1.0.0  diagnostic target: USB MIDI2 + CDC ACM, no UAC2, eMMC never touched";
const WDT = "wdt: pre_running=0 ours=1 install_rc=0 setup_rc=0 rren=0x0f runstatus=1";
const SAMPLE = "AIN0= 512 AIN1=1023 dec=T1+T4        stable=T1+T4        unmeas=3";

const blankConsole = (): FirmwareConsoleState => ({
  supported: true,
  status: "connected",
  error: null,
  reportedVersion: null,
  target: null,
  watchdog: null,
  ain0: null,
  ain1: null,
  decodedMask: null,
  stableMask: null,
  unmeasured: null,
  lines: [],
  lineCount: 0,
});

describe("firmware console parsing", () => {
  it("parses the banner line", () => {
    const p = parseFirmwareLine(BANNER);
    expect(p.kind).toBe("banner");
    if (p.kind === "banner") {
      expect(p.version).toBe("Stem Tape M0 v1.0.0");
      expect(p.target).toContain("USB MIDI2 + CDC ACM");
    }
  });

  it("parses the watchdog line", () => {
    expect(parseFirmwareLine(WDT)).toMatchObject({
      kind: "watchdog",
      preRunning: false,
      ours: true,
      installRc: 0,
      setupRc: 0,
      rren: 15,
      runstatus: 1,
    });
  });

  it("parses AIN / mask / unmeasured samples", () => {
    expect(parseFirmwareLine(SAMPLE)).toMatchObject({
      kind: "sample",
      ain0: 512,
      ain1: 1023,
      decoded: "T1+T4",
      stable: "T1+T4",
      unmeasured: 3,
    });
  });

  it("parses the UNMEASURED sentinel mask", () => {
    expect(parseFirmwareLine("AIN0=  10 AIN1=  11 dec=UNMEASURED   stable=NONE         unmeas=7")).toMatchObject({
      decoded: "UNMEASURED",
      stable: "NONE",
      unmeasured: 7,
    });
  });

  it("never throws on junk", () => {
    expect(parseFirmwareLine("\u0000garbage\r").kind).toBe("other");
  });

  it("folds lines into console state", () => {
    let s = applyFirmwareLine(blankConsole(), BANNER);
    s = applyFirmwareLine(s, WDT);
    s = applyFirmwareLine(s, SAMPLE);
    expect(s.reportedVersion).toBe("Stem Tape M0 v1.0.0");
    expect(s.watchdog?.ours).toBe(true);
    expect(s.ain0).toBe(512);
    expect(s.stableMask).toBe("T1+T4");
    expect(s.lineCount).toBe(3);
  });

  it("handles partial and multi-line chunks with mixed line endings", () => {
    const c = new FirmwareConsole();
    c.feedChunk(`${BANNER}\r\n${WDT}\nAIN0= 1 AIN1= 2 dec=NONE `);
    expect(c.snapshot().reportedVersion).toBe("Stem Tape M0 v1.0.0");
    expect(c.snapshot().ain0).toBeNull();
    c.feedChunk("stable=NONE unmeas=0\n");
    expect(c.snapshot().ain0).toBe(1);
    expect(c.snapshot().unmeasured).toBe(0);
  });
});

describe("web serial lifecycle", () => {
  function mockPort() {
    const setSignals = vi.fn(async () => {});
    const close = vi.fn(async () => {});
    const cancel = vi.fn(async () => {});
    const releaseLock = vi.fn();
    const reader = { read: vi.fn(async () => new Promise<never>(() => {})), cancel, releaseLock };
    const port: SerialPortLike = {
      open: vi.fn(async () => {}),
      close,
      setSignals,
      readable: { getReader: () => reader } as unknown as ReadableStream<Uint8Array>,
    };
    return { port, setSignals, close, cancel, releaseLock };
  }

  it("asserts DTR on connect and fully tears down on disconnect", async () => {
    const m = mockPort();
    const c = new FirmwareConsole();
    c.serialImpl = { requestPort: async () => m.port };
    await c.connect();
    expect(m.setSignals).toHaveBeenCalledWith({ dataTerminalReady: true });
    expect(c.snapshot().status).toBe("connected");
    await c.disconnect();
    expect(m.cancel).toHaveBeenCalled();
    expect(m.releaseLock).toHaveBeenCalled();
    expect(m.setSignals).toHaveBeenCalledWith({ dataTerminalReady: false });
    expect(m.close).toHaveBeenCalled();
    expect(c.snapshot().status).toBe("closed");
  });

  it("reports unsupported when Web Serial is absent", async () => {
    const c = new FirmwareConsole();
    c.serialImpl = null;
    expect((await c.connect()).status).toBe("unsupported");
  });

  it("records a denied permission request as an error, not a crash", async () => {
    const c = new FirmwareConsole();
    c.serialImpl = {
      requestPort: async () => {
        throw new Error("No port selected by the user.");
      },
    };
    const s = await c.connect();
    expect(s.status).toBe("error");
    expect(s.error).toContain("No port selected");
  });
});

describe("trace ring", () => {
  it("is inert while disabled", () => {
    const r = new TraceRing(10);
    r.record("midi.raw", "x");
    expect(r.size()).toBe(0);
  });

  it("never exceeds its fixed capacity and keeps order", () => {
    const r = new TraceRing(500);
    r.enable();
    for (let i = 0; i < 900; i++) r.record("midi.raw", `m${i}`);
    expect(r.size()).toBe(500);
    const list = r.list();
    expect(list[0]!.label).toBe("m400");
    expect(list[499]!.label).toBe("m899");
    expect(list.every((rec, i) => i === 0 || rec.seq > list[i - 1]!.seq)).toBe(true);
  });

  it("correlates a multi-control gesture across stages", () => {
    const r = new TraceRing(20);
    r.enable();
    r.beginCorrelation();
    r.record("surface.decoded", "FN DOWN");
    r.beginGesture();
    r.beginCorrelation();
    r.record("surface.decoded", "ROCKER FWD DOWN");
    r.record("gesture.rejected", "scrub candidate", { detail: "transport guard" });
    r.endGesture();
    const list = r.list();
    expect(list[0]!.corr).toBe(1);
    expect(list[1]!.corr).toBe(2);
    expect(list[1]!.gesture).toBe(1);
    expect(list[2]!.gesture).toBe(1);
    const rows = list.map((rec) => formatTraceRow(rec, list[0]!.t));
    expect(rows[2]).toContain("scrub candidate — transport guard");
    expect(rows[2]).toContain("/g1");
  });
});

describe("midi contract notation", () => {
  it("exposes decimal and hexadecimal for every row", () => {
    for (const row of SP1_MIDI_CONTRACT) {
      expect(row.hex).toBe(`0x${row.dec.toString(16).toUpperCase().padStart(2, "0")}`);
    }
    const f1 = SP1_MIDI_CONTRACT.find((r) => r.kind === "cc" && r.name === "Fader 1")!;
    expect(f1.dec).toBe(20);
    expect(f1.hex).toBe("0x14");
  });

  it("describes raw bytes with both notations", () => {
    expect(describeMidiBytes([0xb0, 0x14, 0x7f])).toContain("CC20 (0x14) Fader 1 = 127");
    expect(describeMidiBytes([0x90, 0x24, 0x7f])).toContain("note 36 (0x24) Track 1 press");
  });

  it("decodes bytes 0x14-0x17 as faders and rejects 0x20-0x23", () => {
    for (let i = 0; i < 4; i++) {
      expect(decodeSp1Message([0xb0, 0x14 + i, 0x40])).toMatchObject({ type: "fader", index: i });
      expect(decodeSp1Message([0xb0, 0x20 + i, 0x40])).toBeNull();
    }
    expect(decodeSp1Message([0xb0, 0x18, 0x55])).toMatchObject({ type: "battery", value: 0x55 });
    expect(decodeSp1Message([0xb0, 0x7b, 0x00])).toMatchObject({ type: "allOff" });
  });
});

describe("physical LED model", () => {
  it("contains exactly ten physical LEDs and excludes play-indicator", () => {
    expect(PHYSICAL_LED_IDS).toHaveLength(PHYSICAL_LED_COUNT);
    expect(PHYSICAL_LED_COUNT).toBe(10);
    expect(PHYSICAL_LED_IDS).not.toContain("play-indicator" as never);
    expect(UI_ONLY_INDICATOR_IDS).toEqual(["play-indicator"]);
    expect(LED_IDS).toHaveLength(11);
  });

  it("reports the audited M0 driver as 8 of 10 outputs", () => {
    expect(M0_IMPLEMENTED_LED_OUTPUTS).toHaveLength(8);
    expect(M0_UNRESOLVED_LED_OUTPUTS).toEqual(["function-led-1", "function-led-2"]);
  });
});

describe("behaviour contract", () => {
  it("is versioned and every entry is fully specified with provenance", () => {
    expect(BEHAVIOR_CONTRACT_VERSION).toMatch(/^sp1-behavior-contract\/\d+\.\d+\.\d+$/);
    const provenances = [
      "STOCK_SP1_DOCUMENTED",
      "TAPE_LOOPER_SOURCE",
      "PHYSICAL_OBSERVATION",
      "M0_DIAGNOSTIC_ONLY",
      "STEM_TAPE_OVERRIDE",
      "UNVERIFIED",
    ];
    for (const e of BEHAVIOR_CONTRACT) {
      expect(e.sequence.length).toBeGreaterThan(0);
      expect(e.initiatingState.length).toBeGreaterThan(0);
      expect(e.timing.length).toBeGreaterThan(0);
      expect(e.expectedOwner.length).toBeGreaterThan(0);
      expect(e.expectedCommand.length).toBeGreaterThan(0);
      expect(e.expectedEngineResult.length).toBeGreaterThan(0);
      expect(e.expectedLedSummary.length).toBeGreaterThan(0);
      expect(provenances).toContain(e.provenance);
      expect(e.citation.title.length).toBeGreaterThan(0);
      expect(e.citation.version.length).toBeGreaterThan(0);
      expect(["documentary", "pinned source", "physical observation", "inference"]).toContain(e.citation.evidence);
      expect(["high", "medium", "low", "none"]).toContain(e.confidence);
      expect(["implemented", "partial", "missing", "conflicting", "unverified"]).toContain(e.status);
    }
  });

  it("expects exactly ten physical LEDs per frame, never play-indicator", () => {
    for (const e of BEHAVIOR_CONTRACT) {
      const keys = Object.keys(e.expectedLeds);
      expect(keys).toHaveLength(PHYSICAL_LED_COUNT);
      expect(keys.sort()).toEqual([...PHYSICAL_LED_IDS].sort());
      expect(keys).not.toContain("play-indicator");
    }
  });

  it("covers the required gestures and keeps M0 patterns in their own group", () => {
    const ids = BEHAVIOR_CONTRACT.map((e) => e.id);
    for (const id of [
      "track.mute",
      "play.track.solo",
      "stem.active",
      "function.volume",
      "loop.momentary",
      "loop.latch",
      "loop.exit",
      "scrub.speeds",
      "scrub.momentary",
      "scrub.latch",
      "scrub.inertia",
      "fx.scope",
      "fx.loop.precedence",
      "fx.momentary",
      "fx.latch",
      "fx.flash",
      "fx.flash.restore",
      "fx.clearLatches",
      "heads.mode",
      "m0.boot",
      "m0.dfu",
      "m0.function.hold",
      "m0.led.gap",
    ]) {
      expect(ids).toContain(id);
    }
    for (const e of BEHAVIOR_CONTRACT.filter((x) => x.group === "m0-only")) {
      expect(e.provenance).toBe("M0_DIAGNOSTIC_ONLY");
    }
    // No M0 diagnostic pattern may masquerade as stock behaviour.
    expect(BEHAVIOR_CONTRACT.some((e) => e.provenance === "STOCK_SP1_DOCUMENTED")).toBe(false);
  });

  it("marks unsourced stock behaviour unverified", () => {
    const stock = BEHAVIOR_CONTRACT.find((e) => e.id === "stock.reference")!;
    expect(stock.provenance).toBe("UNVERIFIED");
    expect(stock.status).toBe("unverified");
  });

  it("evaluates without a state", () => {
    expect(evaluateContract(null).every((r) => r.observed === null)).toBe(true);
  });
});

function frame(patterns: Partial<Record<string, LedPattern>>, reason = "test"): LedFrame {
  const out = {} as LedFrame;
  for (const id of LED_IDS) out[id] = { pattern: patterns[id] ?? "dark", reason, priority: 1 };
  return out;
}

describe("led inspector", () => {
  it("maps patterns to diagnostic modes", () => {
    expect(modeOf("dark")).toBe("off");
    expect(modeOf("faint")).toBe("dim");
    expect(modeOf("blink")).toBe("blink");
    expect(modeOf("breathe")).toBe("breathe");
    expect(modeOf("pulse")).toBe("pulse");
  });

  it("inspects exactly the ten physical LEDs and the web-only indicator separately", () => {
    const rows = inspectPhysicalLeds(frame({}), frame({}), []);
    expect(rows).toHaveLength(10);
    expect(rows.every((r) => r.physical)).toBe(true);
    expect(rows.map((r) => r.id)).not.toContain("play-indicator");
    const web = inspectWebOnlyIndicators(frame({}), frame({}), []);
    expect(web).toHaveLength(1);
    expect(web[0]!.physical).toBe(false);
  });

  it("flags the two unresolved M0 function outputs", () => {
    const rows = inspectPhysicalLeds(frame({}), frame({}), []);
    const fn = rows.filter((r) => r.id.startsWith("function-led"));
    expect(fn).toHaveLength(2);
    for (const r of fn) {
      expect(r.m0Driven).toBe(false);
      expect(r.divergence).toBe("physical mapping unresolved");
    }
  });

  it("reports a logical/expected mismatch", () => {
    const rows = inspectLeds(frame({ "track-led-1": "blink" }), frame({ "track-led-1": "faint" }), []);
    expect(rows[0]!.mismatch).toContain("logical blink ≠ expected faint");
  });

  it("reports a DOM/CSS mismatch", () => {
    const rows = inspectLeds(frame({ "track-led-1": "solid" }), frame({ "track-led-1": "solid" }), [
      { id: "track-led-1", className: "st-led st-led--faint", animationName: "none", opacity: "0.32" },
    ]);
    expect(rows[0]!.mismatch).toContain("DOM renders faint");
  });

  it("flags one-shot flashes rendered as infinite CSS animation", () => {
    const rows = inspectLeds(
      frame({ "track-led-1": "blink" }, "fx latch flash"),
      frame({ "track-led-1": "blink" }, "fx latch flash"),
      [],
    );
    expect(rows[0]!.mismatch).toContain("infinite CSS animation");
    expect(rows[0]!.animation).toBe("css-only");
    expect(rows[0]!.divergence).toBe("DOM state correct but CSS timing wrong");
  });
});

describe("report redaction and export", () => {
  it("drops filenames, paths and urls", () => {
    const clean = redact({
      filename: "vocals-final.wav",
      note: "loaded /Users/me/Music/take.wav",
      link: "https://example.com/x",
      keep: "PLAY HELD → TRACK 1 DOWN",
      nested: [{ path: "/tmp/a", ok: "fine" }],
    }) as Record<string, unknown>;
    expect(clean["filename"]).toBeUndefined();
    expect(clean["note"]).toBe("[redacted]");
    expect(clean["link"]).toBe("[redacted]");
    expect(clean["keep"]).toBe("PLAY HELD → TRACK 1 DOWN");
    expect((clean["nested"] as Record<string, unknown>[])[0]!["path"]).toBeUndefined();
  });

  it("renders a text report with the 10-LED model and web-only separation", () => {
    const r: DiagnosticReport = {
      contractVersion: BEHAVIOR_CONTRACT_VERSION,
      generatedAt: "2026-01-01T00:00:00.000Z",
      expectedArtifact: {
        product: "Stem Tape SP-1",
        firmwareBanner: "Stem Tape M0 v1.0.0",
        usbVendorId: "0x1915",
        usbProductId: "0x5211",
        binarySha256: "53de4c003047e20b7e18c45034eab89b79d58ba1677651348e62f9a85b257eeb",
        sourceCommit: "ea354f32c8c484c1d48e68804a4f1695a8a7b131",
        note: "expected build metadata",
      } as DiagnosticReport["expectedArtifact"],
      midiContract: { rows: SP1_MIDI_CONTRACT, warning: "decimal CC20-23" },
      ledModel: {
        physicalLeds: 10,
        webIndicators: "10 physical + 1 web-only",
        m0Implemented: 8,
        m0Unresolved: ["function-led-1", "function-led-2"],
        hostToDeviceLedFeedback: "unsupported",
        note: "gap reported only",
      } as DiagnosticReport["ledModel"],
      device: {
        midiInputName: "STEM TAPE SP-1",
        midiInputId: "in-1",
        midiOutputName: null,
        midiState: "ready",
        consoleState: "idle",
        reportedFirmwareVersion: null,
        capabilities: { "host→device physical LED feedback": "unsupported" },
      },
      firmware: {
        watchdog: null,
        ain0: null,
        ain1: null,
        decodedMask: null,
        stableMask: null,
        unmeasured: null,
      },
      state: null,
      rates: {
        rawMidiPerSec: 0,
        surfaceEventsPerSec: 0,
        reducerCommandsPerSec: 0,
        engineCommandsPerSec: 0,
        unmatchedReleases: 0,
        duplicatePresses: 0,
        staleEvents: 0,
        suppressed: 0,
        faderMessages: 0,
        faderReducerCommands: 0,
      },
      trace: [],
      contract: evaluateContract(null),
      physicalLeds: inspectPhysicalLeds(frame({}), frame({}), []),
      webOnlyIndicators: inspectWebOnlyIndicators(frame({}), frame({}), []),
      failures: [
        {
          id: "fx.flash",
          lastGoodStage: "command emitted",
          firstDivergence: "flash never expires",
          category: "timing/clock missing",
          expected: "one-shot 220 ms",
          actual: "infinite blink",
          requiresHardware: false,
          observation: "browser-observed",
        },
      ],
      unverified: ["stock.reference: no source located"],
      observation: { "physical led state": "not-observed" },
    };
    const text = reportToText(r);
    expect(text).toContain("physical SP-1 LEDs: 10");
    expect(text).toContain("implemented in the audited M0 LED driver: 8 of 10");
    expect(text).toContain("WEB-ONLY INDICATORS — NOT PART OF THE 10-LED PHYSICAL FRAME");
    expect(text).toContain("CC   20 (0x14) Fader 1");
    expect(text).toContain("[timing/clock missing]");
    expect(text).toContain("PHYSICAL LED COMPARISON (10 of 10)");
  });
});
