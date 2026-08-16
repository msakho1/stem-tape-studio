import { describe, expect, it, vi } from "vitest";
import {
  applyFirmwareLine,
  FirmwareConsole,
  parseFirmwareLine,
  type SerialPortLike,
} from "../firmwareConsole";
import { TraceRing, formatTraceRow } from "../trace";
import { BEHAVIOR_CONTRACT, BEHAVIOR_CONTRACT_VERSION, evaluateContract } from "../contract";
import { inspectLeds, LED_IDS, modeOf } from "../leds";
import { redact, reportToText, type DiagnosticReport } from "../report";
import type { LedFrame, LedPattern } from "@/machine/surface";

const BANNER = "Stem Tape M0 v1.0.0  diagnostic target: USB MIDI2 + CDC ACM, no UAC2, eMMC never touched";
const WDT = "wdt: pre_running=0 ours=1 install_rc=0 setup_rc=0 rren=0x0f runstatus=1";
const SAMPLE = "AIN0= 512 AIN1=1023 dec=T1+T4        stable=T1+T4        unmeas=3";

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
    const p = parseFirmwareLine(WDT);
    expect(p).toMatchObject({
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
    const p = parseFirmwareLine(SAMPLE);
    expect(p).toMatchObject({ kind: "sample", ain0: 512, ain1: 1023, decoded: "T1+T4", stable: "T1+T4", unmeasured: 3 });
  });

  it("parses the UNMEASURED sentinel mask", () => {
    const p = parseFirmwareLine("AIN0=  10 AIN1=  11 dec=UNMEASURED   stable=NONE         unmeas=7");
    expect(p).toMatchObject({ decoded: "UNMEASURED", stable: "NONE", unmeasured: 7 });
  });

  it("never throws on junk", () => {
    expect(parseFirmwareLine("\u0000garbage\r").kind).toBe("other");
  });

  it("folds lines into console state", () => {
    let s = applyFirmwareLine(
      { supported: true, status: "connected", error: null, reportedVersion: null, target: null, watchdog: null, ain0: null, ain1: null, decodedMask: null, stableMask: null, unmeasured: null, lines: [], lineCount: 0 },
      BANNER,
    );
    s = applyFirmwareLine(s, WDT);
    s = applyFirmwareLine(s, SAMPLE);
    expect(s.reportedVersion).toBe("Stem Tape M0 v1.0.0");
    expect(s.watchdog?.ours).toBe(true);
    expect(s.ain0).toBe(512);
    expect(s.stableMask).toBe("T1+T4");
    expect(s.lineCount).toBe(3);
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
    const s = await c.connect();
    expect(s.status).toBe("unsupported");
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

  it("formats readable sequence rows", () => {
    const r = new TraceRing(5);
    r.enable();
    r.record("surface.decoded", "FN DOWN", undefined, 100);
    r.record("gesture.rejected", "scrub candidate", { detail: "transport guard" }, 110);
    const rows = r.list().map((rec) => formatTraceRow(rec, 100));
    expect(rows[0]).toContain("FN DOWN");
    expect(rows[1]).toContain("scrub candidate — transport guard");
  });
});

describe("behaviour contract", () => {
  it("is versioned and every entry is fully specified", () => {
    expect(BEHAVIOR_CONTRACT_VERSION).toMatch(/^sp1-behavior-contract\/\d+\.\d+\.\d+$/);
    for (const e of BEHAVIOR_CONTRACT) {
      expect(e.sequence.length).toBeGreaterThan(0);
      expect(e.expectedCommand.length).toBeGreaterThan(0);
      expect(e.expectedLeds.length).toBeGreaterThan(0);
      expect(e.reference.length).toBeGreaterThan(0);
      expect(["implemented", "partial", "missing", "conflicting", "unverified"]).toContain(e.status);
    }
  });

  it("covers the required gestures", () => {
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
    ]) {
      expect(ids).toContain(id);
    }
  });

  it("marks undocumented stock behaviour unverified", () => {
    const stock = BEHAVIOR_CONTRACT.find((e) => e.provenance === "undocumented")!;
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
    const rows = inspectLeds(frame({ "track-led-1": "blink" }, "fx latch flash"), frame({ "track-led-1": "blink" }, "fx latch flash"), []);
    expect(rows[0]!.mismatch).toContain("infinite CSS animation");
    expect(rows[0]!.animation).toBe("css-only");
  });
});

describe("report redaction", () => {
  it("drops filenames, paths and urls", () => {
    const dirty = {
      filename: "vocals-final.wav",
      note: "loaded /Users/me/Music/take.wav",
      link: "https://example.com/x",
      keep: "PLAY HELD → TRACK 1 DOWN",
      nested: [{ path: "/tmp/a", ok: "fine" }],
    };
    const clean = redact(dirty) as Record<string, unknown>;
    expect(clean["filename"]).toBeUndefined();
    expect(clean["note"]).toBe("[redacted]");
    expect(clean["link"]).toBe("[redacted]");
    expect(clean["keep"]).toBe("PLAY HELD → TRACK 1 DOWN");
    expect((clean["nested"] as Record<string, unknown>[])[0]!["path"]).toBeUndefined();
  });

  it("renders a text report with contract and trace sections", () => {
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
        note: "expected build metadata — the device cannot transmit or attest its flashed binary",
      },
      device: {
        midiInputName: "STEM TAPE SP-1",
        midiInputId: "in-1",
        midiOutputName: null,
        midiState: "connected",
        consoleState: "connected",
        reportedFirmwareVersion: "Stem Tape M0 v1.0.0",
        capabilities: { "control input": "supported" },
      },
      firmware: { watchdog: null, ain0: 1, ain1: 2, decodedMask: "T1", stableMask: "T1", unmeasured: 0 },
      state: null,
      rates: { rawMidiPerSec: 1, surfaceEventsPerSec: 1, engineCommandsPerSec: 0, unmatchedReleases: 0, staleEvents: 0, suppressed: 0 },
      trace: [{ seq: 1, t: 0, stage: "midi.raw", label: "90 24 7f" }],
      contract: evaluateContract(null),
      leds: inspectLeds(frame({}), frame({}), []),
      failures: [{ id: "loop.momentary", lastGoodStage: "command emitted", firstDivergence: "no LED derivation" }],
    };
    const text = reportToText(r);
    expect(text).toContain("BEHAVIOUR CONTRACT");
    expect(text).toContain("FAILURES / FIRST DIVERGENCE");
    expect(text).toContain("90 24 7f");
    expect(text).not.toMatch(/\.wav|https?:\/\//);
  });
});
