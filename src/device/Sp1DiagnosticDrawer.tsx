/**
 * SP-1 DIAGNOSTIC — evidence-driven flight recorder UI.
 *
 * HARD RULES:
 *  • This file RECONSTRUCTS NOTHING. Every trace record is written at its own
 *    decision point (sp1Surface, webMidi, chordArbiter, commandTrace,
 *    useAudioEngine, useDeviceSurface, firmwareConsole). The drawer only reads
 *    the ring. It never drains a log, replays history or re-emits records.
 *  • Collection and display are separate. STOP CAPTURE stops collection;
 *    FREEZE VIEW only freezes what is shown.
 *  • Closed drawer + no background capture = no interval, no DOM probe, no
 *    trace subscription, no React work.
 *  • It reports; it never corrects.
 */

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { LedFrame, SurfaceState } from "@/machine/surface";
import { deriveLeds } from "@/machine/surface";
import { GLOBAL_SCRUB_SPEEDS } from "@/audio/inertia";
import { webMidi, type WebMidiState } from "@/audio/midi/webMidi";
import { sp1Surface } from "@/audio/midi/sp1Surface";
import type { ChordArbiter } from "@/machine/chordArbiter";
import { trace, traceNow, type TraceRecord, type TraceStage, type TraceStats } from "@/diagnostics/trace";
import {
  EXPECTED_ARTIFACT,
  firmwareConsole,
  type FirmwareConsoleState,
} from "@/diagnostics/firmwareConsole";
import { CURRENT_FIRMWARE_PROFILE, FIRMWARE_PROFILES } from "@/diagnostics/firmwareProfiles";
import { BEHAVIOR_CONTRACT_VERSION, evaluateContract } from "@/diagnostics/contract";
import { SEGMENT_DEFINITIONS, segmentRunner } from "@/diagnostics/segments";
import { resolveSp1LedFrame, sp1LedStateFrom, formatSp1Frame } from "@/leds/sp1LedEngine";
import type { Sp1LedFrameHandle } from "@/leds/useSp1LedFrame";
import { ledTransport, type LedTransportState } from "@/diagnostics/ledTransport";
import { resolvePhysicalFrame, formatPhysicalFrame } from "@/diagnostics/physicalFrame";
import {
  inspectPhysicalLeds,
  inspectWebOnlyIndicators,
  probeDom,
  type LedInspectionRow,
} from "@/diagnostics/leds";
import {
  M0_CAPABILITIES,
  M0_LED_COVERAGE,
  PHYSICAL_LED_COUNT,
  type ObservationKind,
} from "@/diagnostics/physical";
import { SP1_BUTTON_PHASES, SP1_MIDI_CONTRACT, SP1_NOTATION_WARNING } from "@/diagnostics/midiContract";
import {
  buildReport,
  reportToText,
  type DiagnosticReport,
  type EventRates,
  type StateSnapshot,
} from "@/diagnostics/report";

const BUTTONS = [
  "track-button-1",
  "track-button-2",
  "track-button-3",
  "track-button-4",
  "play",
  "function",
  "volume-plus",
  "volume-minus",
  "rocker-fwd",
  "rocker-rwd",
] as const;

const STAGE_FILTERS: { key: string; stages: TraceStage[] }[] = [
  { key: "all", stages: [] },
  { key: "serial", stages: ["serial.raw", "serial.parsed"] },
  { key: "midi", stages: ["midi.raw", "midi.device.recognized"] },
  { key: "decode", stages: ["surface.decoded"] },
  { key: "resync", stages: ["connection.resync", "surface.suppressed"] },
  { key: "held", stages: ["surface.held"] },
  {
    key: "arbitration",
    stages: ["gesture.candidate", "gesture.arbitration", "gesture.owner", "gesture.rejected"],
  },
  { key: "commands", stages: ["command.surface", "command.engine"] },
  { key: "ack", stages: ["engine.ack"] },
  { key: "state", stages: ["state.transport", "state.mixer", "state.fx"] },
  { key: "led", stages: ["led.derived", "led.transmitted", "firmware.led.reported"] },
  { key: "capture", stages: ["capture.control"] },
];

/** Visible trace re-renders are bounded to 10 Hz, however fast events arrive. */
const VIEW_REFRESH_MS = 100;

interface Props {
  state: SurfaceState;
  leds: LedFrame;
  arbiter: ChordArbiter;
  /** Authoritative resolved physical frame from the single LED engine. */
  sp1?: Sp1LedFrameHandle;
}

function Row({ k, v }: { k: string; v: React.ReactNode }) {
  return (
    <div className="flex justify-between gap-3 border-b border-[var(--bench-line)] py-0.5">
      <span className="text-[var(--ink-faint)]">{k}</span>
      <span className="text-right text-[var(--ink)]">{v}</span>
    </div>
  );
}

function LedRows({ rows }: { rows: LedInspectionRow[] }) {
  return (
    <>
      {rows.map((l) => (
        <div key={l.id} className="border-b border-[var(--bench-line)] py-1">
          <div className="flex justify-between gap-2">
            <span>
              {l.index + 1}. {l.id}
            </span>
            <span className="text-[var(--ink-faint)]">
              period {l.periodMs ?? "—"}ms · phase {l.phaseAnchor} · {l.animation}
            </span>
          </div>
          <div className="text-[var(--ink-faint)]">1 expected contract: {l.columnExpected}</div>
          <div className="text-[var(--ink-faint)]">2 web logical: {l.columnWebLogical} · p{l.priority}</div>
          <div className="text-[var(--ink-faint)]">3 rendered dom/css: {l.columnDom}</div>
          <div className="text-[var(--ink-faint)]">4 transmitted/firmware: {l.columnTransmitted}</div>
          <div className="text-[var(--ink-faint)]">physical observation: {l.physicalObservation}</div>
          {l.mismatch && (
            <div className="text-[var(--signal)]">
              mismatch: {l.mismatch}
              {l.divergence ? ` · category: ${l.divergence}` : ""}
            </div>
          )}
        </div>
      ))}
    </>
  );
}

export function Sp1DiagnosticDrawer({ state, leds, arbiter, sp1 }: Props) {
  const [open, setOpen] = useState(false);
  /** Background capture: keeps collecting while the drawer is closed. */
  const [background, setBackground] = useState(false);
  const [capturing, setCapturing] = useState(false);
  const [followLive, setFollowLive] = useState(true);
  const [frozen, setFrozen] = useState(false);
  const [verboseTransport, setVerboseTransport] = useState(false);
  const [viewTick, setViewTick] = useState(0);
  const [console_, setConsole] = useState<FirmwareConsoleState>(() => firmwareConsole.snapshot());
  const [midi, setMidi] = useState<WebMidiState>(() => webMidi.snapshot());
  const [led, setLed] = useState<LedTransportState>(() => ledTransport.snapshot());
  const [physicalRows, setPhysicalRows] = useState<LedInspectionRow[]>([]);
  /**
   * The authoritative resolved frame. When the surface hook passes its live
   * handle we read the SAME sampled frame the DOM and the MIDI sink saw;
   * otherwise we resolve once for display only.
   */
  const authoritative = sp1
    ? sp1.sample()
    : resolveSp1LedFrame(sp1LedStateFrom(state, typeof performance !== "undefined" ? performance.now() : 0), 0);
  const [webRows, setWebRows] = useState<LedInspectionRow[]>([]);
  const [filter, setFilter] = useState("all");
  const [copied, setCopied] = useState<string | null>(null);
  const [stats, setStats] = useState<TraceStats>(() => trace.stats());
  const [segmentTick, setSegmentTick] = useState(0);
  const [frozenRecords, setFrozenRecords] = useState<TraceRecord[] | null>(null);

  const traceRef = useRef<HTMLPreElement | null>(null);
  /** Cumulative counters survive a quiet last second. */
  const totals = useRef({ raw: 0, surface: 0, reducer: 0, engine: 0, faderMsgs: 0, faderCmds: 0 });

  const active = open || (background && capturing);

  // ---- collection ---------------------------------------------------------
  const startCapture = useCallback(() => {
    trace.startCapture();
    setCapturing(true);
    setStats(trace.stats());
  }, []);

  const stopCapture = useCallback(() => {
    trace.stopCapture();
    setCapturing(false);
    setStats(trace.stats());
  }, []);

  // Opening the drawer arms capture; closing it stops capture unless the user
  // explicitly asked for background capture.
  useEffect(() => {
    if (open) {
      if (!trace.enabled) startCapture();
      return;
    }
    if (!background) {
      trace.stopCapture();
      setCapturing(false);
      void firmwareConsole.disconnect();
    }
  }, [open, background, startCapture]);

  useEffect(() => {
    ledTransport.verboseTransport = verboseTransport;
  }, [verboseTransport]);

  // ---- subscriptions (only while the panel is active) ---------------------
  useEffect(() => {
    if (!active) return;
    const offMidi = webMidi.onStateChange(setMidi);
    const offConsole = firmwareConsole.subscribe(setConsole);
    const offLed = ledTransport.subscribe(setLed);
    const offSeg = segmentRunner.subscribe(() => setSegmentTick((n) => n + 1));
    return () => {
      offMidi();
      offConsole();
      offLed();
      offSeg();
    };
  }, [active]);

  // ---- bounded view refresh (10 Hz max) -----------------------------------
  useEffect(() => {
    if (!open) return;
    const id = window.setInterval(() => {
      setStats(trace.stats());
      if (!frozen) setViewTick((t) => t + 1);
    }, VIEW_REFRESH_MS);
    return () => window.clearInterval(id);
  }, [open, frozen]);

  // ---- LED inspector probe (1 Hz, view-only, writes no trace records) -----
  useEffect(() => {
    if (!open) return;
    const probe = () => {
      const dom = probeDom();
      const expected = deriveLeds(state, traceNow() + 1000);
      const tx = { transmitted: ledTransport.snapshot().lastFrame, firmwareReported: null };
      setPhysicalRows(inspectPhysicalLeds(leds, expected, dom, {}, tx));
      setWebRows(inspectWebOnlyIndicators(leds, expected, dom));
    };
    probe();
    const id = window.setInterval(probe, 1000);
    return () => window.clearInterval(id);
  }, [open, leds, state]);

  // ---- cumulative + per-second rates, read from the ring's own stats ------
  const [rates, setRates] = useState({ raw: 0, surface: 0, reducer: 0, engine: 0 });
  useEffect(() => {
    if (!open) return;
    let last = { raw: 0, surf: 0, red: 0, eng: 0 };
    const id = window.setInterval(() => {
      const by = trace.stats().byStage;
      const raw = by["midi.raw"] ?? 0;
      const surf = by["surface.decoded"] ?? 0;
      const red = by["command.surface"] ?? 0;
      const eng = (by["command.engine"] ?? 0) + (by["engine.ack"] ?? 0);
      totals.current = {
        ...totals.current,
        raw: Math.max(totals.current.raw, raw),
        surface: Math.max(totals.current.surface, surf),
        reducer: Math.max(totals.current.reducer, red),
        engine: Math.max(totals.current.engine, eng),
      };
      setRates({
        raw: Math.max(0, raw - last.raw),
        surface: Math.max(0, surf - last.surf),
        reducer: Math.max(0, red - last.red),
        engine: Math.max(0, eng - last.eng),
      });
      last = { raw, surf, red, eng };
    }, 1000);
    return () => window.clearInterval(id);
  }, [open]);

  const heldControls = state.pressed as unknown as string[];
  /** Last hardware fader positions, kept separate from mixer gain. */
  const [hardwareFaders, setHardwareFaders] = useState<(number | null)[]>([null, null, null, null]);
  useEffect(() => {
    if (!active) return;
    return sp1Surface.subscribe((ev) => {
      if (ev.type !== "fader") return;
      setHardwareFaders((prev) => prev.map((v, i) => (i === ev.index ? ev.value : v)));
    });
  }, [active]);

  const snapshot: StateSnapshot = useMemo(
    () => ({
      webPowered: state.power === "on",
      playing: state.playing,
      globalLoop: state.globalLoop.active ? (state.globalLoop.latched ? "latched" : "momentary") : "off",
      loopDivision: state.globalLoop.division,
      scrubDirection: state.globalScrub === 1 ? "forward" : state.globalScrub === -1 ? "reverse" : "idle",
      scrubSpeedLevel: state.scrubSpeed + 1,
      scrubMultiplier: GLOBAL_SCRUB_SPEEDS[state.scrubSpeed] ?? 1,
      scrubLatched: state.scrubLatched,
      inertia: state.globalScrub !== 0 ? "shuttling" : state.scrubLatched ? "latched" : "settled / handoff idle",
      activeStem: state.perf.activeStem + 1,
      fxOverlay: state.perf.fxOverlay,
      fxScope: state.perf.fxScope,
      fxBank: state.bank,
      fxMomentary: state.perf.tracks.flatMap((t, i) =>
        Object.entries(t.fx)
          .filter(([, slot]) => slot.momentary)
          .map(([name]) => `${i + 1}:${name}`),
      ),
      fxLatched: state.perf.tracks.flatMap((t, i) =>
        Object.entries(t.fx)
          .filter(([, slot]) => slot.latched)
          .map(([name]) => `${i + 1}:${name}`),
      ),
      muted: state.tracks.map((t) => t.content === "muted"),
      soloed: state.perf.tracks.map((t) => t.soloed),
      linked: state.perf.tracks.map((t) => t.linked),
      heldControls: [...heldControls],
      arbitrationOwner: arbiter.currentOwner(),
      faderHardware: hardwareFaders,
      mixerGain: state.tracks.map((t) => t.volume),
      faderPickup: "not implemented — engine gain follows hardware CC immediately (no baseline, no arming, no crossing)",
      buttons: Object.fromEntries(BUTTONS.map((b) => [b, state.pressed.includes(b)])),
    }),
    [state, arbiter, heldControls, hardwareFaders],
  );

  const contract = useMemo(
    () => evaluateContract(state, (id) => segmentRunner.resultFor(id)),
    [state, segmentTick],
  );

  const eventRates: EventRates = useMemo(
    () => ({
      rawMidiPerSec: rates.raw,
      surfaceEventsPerSec: rates.surface,
      reducerCommandsPerSec: rates.reducer,
      engineCommandsPerSec: rates.engine,
      cumulativeRawMidi: totals.current.raw,
      cumulativeSurfaceEvents: totals.current.surface,
      cumulativeReducerCommands: totals.current.reducer,
      cumulativeEngineCommands: totals.current.engine,
      coalesced: stats.coalesced,
      dropped: stats.dropped,
      generated: stats.generated,
    }),
    [rates, stats],
  );

  const report = useCallback((): DiagnosticReport => {
    // ONE immutable point-in-time snapshot. Continued capture cannot mutate it.
    const snap = trace.snapshot();
    const midiIn = midi.devices.find((d) => /STEM TAPE SP-1/i.test(d.name)) ?? midi.devices[0] ?? null;
    const observation: Record<string, ObservationKind> = {
      "serial console lines": console_.lineCount > 0 ? "browser-observed" : "not-observed",
      "midi input": midiIn ? "browser-observed" : "not-observed",
      "physical led illumination": "not-observed",
      "logical led derivation": "browser-observed",
      "transmitted led frames": led.commits > 0 ? "browser-observed" : "not-observed",
      "behaviour contract expectations": "not-observed",
      "firmware serial content": console_.lineCount > 0 ? "browser-observed" : "not-observed",
      "physical side-row playback index": "not-observed",
    };
    return buildReport({
      contractVersion: BEHAVIOR_CONTRACT_VERSION,
      generatedAt: new Date().toISOString(),
      expectedArtifact: EXPECTED_ARTIFACT,
      firmwareProfiles: FIRMWARE_PROFILES.map((p) => ({ ...p })),
      midiContract: { rows: SP1_MIDI_CONTRACT, warning: SP1_NOTATION_WARNING },
      ledModel: M0_LED_COVERAGE,
      device: {
        midiInputName: midiIn?.name ?? null,
        midiInputId: midiIn?.id ?? null,
        midiOutputName: led.outputName,
        midiOutputId: led.outputId,
        midiState: midi.status,
        consoleState: console_.status,
        // NEVER back-filled from expected metadata.
        reportedFirmwareVersion: console_.status === "connected" ? console_.reportedVersion : null,
        capabilities: M0_CAPABILITIES as Record<string, string>,
      },
      ledTransport: {
        status: led.status,
        protocolVersion: led.protocolVersion,
        capabilityQuerySent: led.capabilityQuerySent,
        leaseActive: led.leaseActive,
        commitSequence: led.commitSequence,
        commits: led.commits,
        heartbeats: led.heartbeats,
        stagedMessages: led.stagedMessages,
        lastFrame: led.lastFrame,
        candidateOutputs: led.candidateOutputs,
        error: led.error,
      },
      firmware: {
        watchdog: console_.watchdog,
        ain0: console_.ain0,
        ain1: console_.ain1,
        decodedMask: console_.decodedMask,
        stableMask: console_.stableMask,
        unmeasured: console_.unmeasured,
      },
      state: snapshot,
      rates: eventRates,
      captureStats: snap.stats,
      trace: snap.records,
      contract,
      reproductions: segmentRunner.all(),
      physicalLeds: physicalRows,
      webOnlyIndicators: webRows,
      // A failure exists ONLY for a reproduction that actually ran and failed.
      failures: contract
        .filter((c) => c.reproductionStatus === "failed")
        .map((c) => ({
          id: c.id,
          segmentId: c.segmentId,
          lastGoodStage: segmentRunner.resultFor(c.id)?.lastSuccessfulStage ?? "none",
          firstDivergence: c.reproductionFirstDivergence ?? "unknown",
          category: c.firstDivergence ?? null,
          expected: c.expectedLedSummary,
          actual: c.observed ?? "not derivable from reducer state",
          requiresHardware: c.provenance === "PHYSICAL_OBSERVATION" || c.provenance === "M0_DIAGNOSTIC_ONLY",
          observation: c.observationSource === "live-hardware" ? "physically-observed" : c.observationSource === "browser-injection" ? "browser-observed" : c.observationSource === "mocked" ? "mocked" : "not-observed",
        })),
      unverified: contract
        .filter((c) => c.status === "unverified" || c.provenance === "UNVERIFIED")
        .map((c) => `${c.id}: ${c.citation.title}`),
      observation,
    });
  }, [midi, console_, led, snapshot, eventRates, contract, physicalRows, webRows]);

  const doCopy = useCallback(async () => {
    try {
      await navigator.clipboard.writeText(reportToText(report()));
      setCopied("copied");
    } catch {
      setCopied("clipboard blocked");
    }
    window.setTimeout(() => setCopied(null), 2000);
  }, [report]);

  const doDownload = useCallback(() => {
    const blob = new Blob([JSON.stringify(report(), null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = "sp1-diagnostic.json";
    a.click();
    URL.revokeObjectURL(url);
  }, [report]);

  const activeFilter = STAGE_FILTERS.find((f) => f.key === filter) ?? STAGE_FILTERS[0]!;
  const liveRecords = useMemo(() => {
    void viewTick;
    return trace.recent(200);
  }, [viewTick]);
  const shown = (frozen ? (frozenRecords ?? liveRecords) : liveRecords)
    .filter((r) => activeFilter.stages.length === 0 || activeFilter.stages.includes(r.stage))
    .slice(0, 120);
  const t0 = shown[shown.length - 1]?.t ?? 0;

  // Follow Live off: never move the user's scroll position.
  useEffect(() => {
    if (!followLive || frozen || !traceRef.current) return;
    traceRef.current.scrollTop = 0;
  }, [followLive, frozen, viewTick]);

  const toggleFreeze = useCallback(() => {
    setFrozen((f) => {
      if (!f) setFrozenRecords(trace.recent(200));
      else setFrozenRecords(null);
      return !f;
    });
  }, []);

  const currentSegment = segmentRunner.current();

  return (
    <div className="mt-4 border-t border-[var(--bench-line)] pt-3" data-testid="sp1-diagnostic">
      <button
        type="button"
        data-testid="sp1-diagnostic-toggle"
        aria-expanded={open}
        onClick={() => setOpen((v) => !v)}
        className="w-full border border-[var(--bench-line)] px-2 py-1.5 text-left font-mono text-[10px] uppercase tracking-[0.18em] text-[var(--ink-dim)] hover:text-[var(--signal)]"
      >
        {open ? "▾" : "▸"} sp-1 diagnostic
      </button>

      {!open && capturing && background && (
        <div
          className="mt-1 flex items-center justify-between border border-[var(--signal)] px-2 py-1 font-mono text-[10px] uppercase tracking-[0.14em] text-[var(--signal)]"
          data-testid="capture-indicator"
        >
          <span>● background capture · {stats.stored}/500</span>
          <button type="button" data-testid="stop-capture" onClick={stopCapture}>
            stop capture
          </button>
        </div>
      )}

      {open && (
        <div className="mt-2 space-y-3 font-mono text-[10px] leading-relaxed" data-testid="sp1-diagnostic-body">
          {/* ---------- capture controls ---------- */}
          <section className="border border-[var(--bench-line)] p-2" data-testid="capture-controls">
            <p className="text-[var(--signal)]">capture</p>
            <div className="mt-1 flex flex-wrap gap-1">
              <button
                type="button"
                data-testid="start-capture"
                onClick={startCapture}
                className="border border-[var(--bench-line)] px-2 py-1 uppercase tracking-[0.14em]"
              >
                start capture
              </button>
              <button
                type="button"
                data-testid="stop-capture"
                onClick={stopCapture}
                className="border border-[var(--bench-line)] px-2 py-1 uppercase tracking-[0.14em]"
              >
                stop capture
              </button>
              <button
                type="button"
                data-testid="clear-capture"
                onClick={() => {
                  trace.clear();
                  setFrozenRecords(null);
                  setStats(trace.stats());
                }}
                className="border border-[var(--bench-line)] px-2 py-1 uppercase tracking-[0.14em]"
              >
                clear
              </button>
              <button
                type="button"
                data-testid="follow-live"
                onClick={() => setFollowLive((v) => !v)}
                className={`border px-2 py-1 uppercase tracking-[0.14em] ${followLive ? "border-[var(--signal)] text-[var(--signal)]" : "border-[var(--bench-line)]"}`}
              >
                follow live {followLive ? "on" : "off"}
              </button>
              <button
                type="button"
                data-testid="freeze-view"
                onClick={toggleFreeze}
                className={`border px-2 py-1 uppercase tracking-[0.14em] ${frozen ? "border-[var(--signal)] text-[var(--signal)]" : "border-[var(--bench-line)]"}`}
              >
                {frozen ? "resume view" : "freeze view"}
              </button>
              <button
                type="button"
                data-testid="verbose-led-transport"
                onClick={() => setVerboseTransport((v) => !v)}
                className={`border px-2 py-1 uppercase tracking-[0.14em] ${verboseTransport ? "border-[var(--signal)] text-[var(--signal)]" : "border-[var(--bench-line)]"}`}
              >
                verbose led transport {verboseTransport ? "on" : "off"}
              </button>
              <button
                type="button"
                data-testid="background-capture"
                onClick={() => setBackground((v) => !v)}
                className={`border px-2 py-1 uppercase tracking-[0.14em] ${background ? "border-[var(--signal)] text-[var(--signal)]" : "border-[var(--bench-line)]"}`}
              >
                keep capturing when collapsed {background ? "on" : "off"}
              </button>
            </div>
            <Row k="capture" v={stats.running ? `running · id ${stats.captureId}` : "stopped"} />
            <Row k="view" v={frozen ? "frozen" : "live"} />
            <Row k="records stored" v={`${stats.stored}/${stats.capacity}`} />
            <Row k="records generated" v={stats.generated} />
            <Row k="dropped by rollover" v={stats.dropped} />
            <Row k="coalesced" v={stats.coalesced} />
            <Row k="capture duration" v={`${(stats.durationMs / 1000).toFixed(1)}s`} />
          </section>

          {/* ---------- device ---------- */}
          <section className="border border-[var(--bench-line)] p-2">
            <p className="text-[var(--signal)]">device</p>
            <Row k="midi input" v={midi.devices[0]?.name ?? "none"} />
            <Row k="midi input id" v={midi.devices[0]?.id ?? "—"} />
            <Row k="matching midi output" v={led.outputName ?? "none"} />
            <Row k="midi output id" v={led.outputId ?? "—"} />
            <Row k="midi state" v={midi.status} />
            <Row k="firmware console" v={console_.status} />
            <Row
              k="reported firmware"
              v={console_.status === "connected" ? (console_.reportedVersion ?? "not reported") : "unknown (console closed)"}
            />
            <p className="mt-2 text-[var(--ink-faint)]">
              expected artifact metadata — never verified device identity
            </p>
            {Object.entries(EXPECTED_ARTIFACT).map(([k, v]) => (
              <Row key={k} k={k} v={<span className="break-all">{v}</span>} />
            ))}
            <p className="mt-2 text-[var(--ink-faint)]">profile registry</p>
            {FIRMWARE_PROFILES.map((p) => (
              <Row
                key={p.id}
                k={`${p.id}${p.id === CURRENT_FIRMWARE_PROFILE.id ? " (current)" : ""}`}
                v={`${p.firmwareBanner} · led protocol v${p.ledProtocolVersion} · ${p.status}`}
              />
            ))}
            <p className="mt-2 text-[var(--ink-faint)]">capabilities</p>
            {Object.entries(M0_CAPABILITIES).map(([k, v]) => (
              <Row key={k} k={k} v={v} />
            ))}
            <div className="mt-2 flex flex-wrap gap-2">
              <button
                type="button"
                data-testid="connect-firmware-console"
                onClick={() => void firmwareConsole.connect()}
                className="border border-[var(--bench-line)] px-2 py-1 uppercase tracking-[0.14em] hover:text-[var(--signal)]"
              >
                connect firmware console
              </button>
              <button
                type="button"
                onClick={() => void firmwareConsole.disconnect()}
                className="border border-[var(--bench-line)] px-2 py-1 uppercase tracking-[0.14em] hover:text-[var(--signal)]"
              >
                disconnect
              </button>
            </div>
            {console_.error && <Row k="serial parse/connect error" v={console_.error} />}
          </section>

          {/* ---------- host LED link ---------- */}
          <section className="border border-[var(--bench-line)] p-2" data-testid="led-transport">
            <p className="text-[var(--signal)]">host→device led link · channel 16 · protocol v1</p>
            <Row k="midi input connected" v={led.inputConnected ? "yes" : "no"} />
            <Row k="matching output found" v={led.candidateOutputs.length > 0 ? `${led.candidateOutputs.length}` : "no"} />
            <Row k="capability query sent" v={led.capabilityQuerySent ? "yes (CC91=0)" : "no"} />
            <Row
              k="protocol-v1 response"
              v={led.protocolVersion === 1 ? "received (CC91=1)" : led.protocolVersion === 0 ? "legacy / none" : "pending"}
            />
            <Row k="led lease" v={led.leaseActive ? "active" : "inactive"} />
            <Row k="link status" v={led.status} />
            <Row k="commit sequence" v={led.commitSequence} />
            <Row k="commits / heartbeats" v={`${led.commits} / ${led.heartbeats}`} />
            <Row k="staged cc messages" v={led.stagedMessages} />
            <Row k="last transmitted frame" v={led.lastFrame ? led.lastFrame.join(" ") : "none"} />
            {led.candidateOutputs.length > 1 && (
              <div className="mt-1 flex flex-wrap gap-1">
                {led.candidateOutputs.map((o) => (
                  <span key={o.id} className="border border-[var(--bench-line)] px-1">
                    {o.name}
                  </span>
                ))}
              </div>
            )}
            {led.error && <Row k="transmission error" v={led.error} />}
            <p className="mt-1 text-[var(--ink-faint)]">
              a transmitted frame proves only that the browser attempted transmission — never that the firmware
              committed or that an LED is visibly lit
            </p>
          </section>

          {/* ---------- midi contract reference ---------- */}
          <section className="border border-[var(--bench-line)] p-2" data-testid="midi-contract">
            <p className="text-[var(--signal)]">expected m0 midi contract · channel 1</p>
            <p className="text-[var(--ink-faint)]">{SP1_NOTATION_WARNING}</p>
            {SP1_MIDI_CONTRACT.map((r) => (
              <Row
                key={`${r.kind}${r.dec}`}
                k={`${r.kind === "note" ? "note" : "CC"} ${r.dec} (${r.hex})`}
                v={`${r.name}${r.note ? ` · ${r.note}` : ""}`}
              />
            ))}
            {SP1_BUTTON_PHASES.map((p) => (
              <p key={p} className="text-[var(--ink-faint)]">
                {p}
              </p>
            ))}
          </section>

          {/* ---------- live state ---------- */}
          <section className="border border-[var(--bench-line)] p-2">
            <p className="text-[var(--signal)]">live state</p>
            <Row k="web power" v={snapshot.webPowered ? "on" : "off"} />
            <Row k="transport" v={snapshot.playing ? "playing" : "stopped"} />
            <Row k="buttons (10)" v={BUTTONS.map((b) => (state.pressed.includes(b) ? "1" : "0")).join("")} />
            <Row k="mixer gain" v={snapshot.mixerGain.map((f) => f.toFixed(2)).join(" ")} />
            <Row
              k="fader hardware position"
              v={snapshot.faderHardware.map((f) => (f === null ? "—" : f.toFixed(2))).join(" ")}
            />
            <Row k="fader pickup" v={snapshot.faderPickup} />
            <Row k="muted" v={snapshot.muted.map((m) => (m ? "M" : "-")).join("")} />
            <Row k="soloed" v={snapshot.soloed.map((m) => (m ? "S" : "-")).join("")} />
            <Row k="linked" v={snapshot.linked.map((m) => (m ? "L" : "-")).join("")} />
            <Row k="global loop" v={`${snapshot.globalLoop} · 1/${snapshot.loopDivision}`} />
            <Row
              k="scrub"
              v={`${snapshot.scrubDirection} · level ${snapshot.scrubSpeedLevel} (${snapshot.scrubMultiplier}×) · ${snapshot.scrubLatched ? "latched" : "momentary"}`}
            />
            <Row k="inertia" v={snapshot.inertia} />
            <Row k="fx overlay" v={snapshot.fxOverlay ? `open · ${snapshot.fxScope}` : "closed"} />
            <Row k="fx bank" v={snapshot.fxBank + 1} />
            <Row k="fx momentary" v={snapshot.fxMomentary.join(", ") || "none"} />
            <Row k="fx latched" v={snapshot.fxLatched.join(", ") || "none"} />
            <Row k="arbitration owner" v={snapshot.arbitrationOwner ?? "idle (no gesture owns arbitration)"} />
            <Row k="raw midi /s · cumulative" v={`${rates.raw} · ${totals.current.raw}`} />
            <Row k="surface events /s · cumulative" v={`${rates.surface} · ${totals.current.surface}`} />
            <Row k="reducer commands /s · cumulative" v={`${rates.reducer} · ${totals.current.reducer}`} />
            <Row k="engine commands /s · cumulative" v={`${rates.engine} · ${totals.current.engine}`} />
            <Row k="AIN0 / AIN1" v={`${console_.ain0 ?? "—"} / ${console_.ain1 ?? "—"}`} />
            <Row k="decoded / stable mask" v={`${console_.decodedMask ?? "—"} / ${console_.stableMask ?? "—"}`} />
          </section>

          {/* ---------- reproductions ---------- */}
          <section className="border border-[var(--bench-line)] p-2" data-testid="reproductions">
            <p className="text-[var(--signal)]">
              named reproductions {currentSegment ? `· running ${currentSegment.name}` : ""}
            </p>
            <div className="mt-1 flex flex-wrap gap-1">
              {SEGMENT_DEFINITIONS.map((d) => (
                <button
                  key={d.id}
                  type="button"
                  data-segment={d.id}
                  onClick={() =>
                    segmentRunner.begin(
                      d,
                      `playing=${state.playing} loop=${state.globalLoop.active} scrub=${state.globalScrub}`,
                      "live-hardware",
                    )
                  }
                  className="border border-[var(--bench-line)] px-1 uppercase tracking-[0.12em]"
                >
                  begin: {d.name}
                </button>
              ))}
              {currentSegment && (
                <button
                  type="button"
                  data-testid="end-reproduction"
                  onClick={() =>
                    segmentRunner.end(
                      `playing=${state.playing} loop=${state.globalLoop.active} scrub=${state.globalScrub}`,
                    )
                  }
                  className="border border-[var(--signal)] px-1 uppercase tracking-[0.12em] text-[var(--signal)]"
                >
                  end reproduction
                </button>
              )}
            </div>
            {segmentRunner.all().map((r) => (
              <Row
                key={r.segmentId}
                k={`${r.name} (${r.segmentId})`}
                v={`${r.status} · ${r.observationSource}${r.firstMissingStage ? ` · first missing ${r.firstMissingStage}` : ""}`}
              />
            ))}
          </section>

          {/* ---------- physical LED inspector ---------- */}
          <section className="border border-[var(--bench-line)] p-2" data-testid="physical-led-inspector">
            <p className="text-[var(--signal)]">
              physical sp-1 frame · {physicalRows.length || PHYSICAL_LED_COUNT} leds (4 track + 4 side/status) ·
              electrical gpio coverage {M0_LED_COVERAGE.electricalCoverage} · host→device feedback{" "}
              {M0_LED_COVERAGE.hostToDeviceLedFeedback}
            </p>
            <p className="text-[var(--ink-faint)]">
              resolved frame: {formatSp1Frame(authoritative)}
            </p>
            <LedRows rows={physicalRows} />
          </section>

          {/* ---------- authoritative LED parity ---------- */}
          <section className="border border-[var(--bench-line)] p-2" data-testid="led-parity">
            <p className="text-[var(--signal)]">
              led parity · resolveSp1LedFrame (single engine) · transmitted{" "}
              {led.lastFrame ? led.lastFrame.join(",") : "none"} · link {led.status}
            </p>
            {authoritative.leds.map((l) => {
              const el =
                typeof document === "undefined"
                  ? null
                  : document.querySelector<SVGGElement>(`[data-led="${l.id}"] .st-led__core`);
              const domOpacity = el ? (el as unknown as SVGElement).style.opacity || "—" : "—";
              const sent = led.lastFrame?.[l.index] ?? null;
              return (
                <div key={l.id} className="border-b border-[var(--bench-line)] py-0.5">
                  <div className="flex justify-between gap-2">
                    <span>
                      [{l.index}] {l.name}
                    </span>
                    <span className="uppercase text-[var(--signal)]">
                      {l.mode} · {l.brightness}/127
                    </span>
                  </div>
                  <div className="text-[var(--ink-faint)]">
                    owner {l.owner} · precedence {l.precedence} ({l.precedenceKey}) · period{" "}
                    {l.periodMs ?? "—"} · phase {l.phaseAnchor} · {l.direction}
                  </div>
                  <div className="text-[var(--ink-faint)]">
                    provenance {l.provenance} · lost-to {l.lostTo ?? "—"} · restores-to {l.restoreTo ?? "—"}
                  </div>
                  <div className="text-[var(--ink-faint)]">
                    logical {Math.round((l.brightness / 127) * 1000) / 1000} · dom opacity {domOpacity} · transmitted{" "}
                    {sent === null ? "not transmitted" : sent} ·{" "}
                    {sent !== null && sent === l.brightness ? "parity" : "divergent/unsent"}
                  </div>
                </div>
              );
            })}
          </section>

          <section className="border border-[var(--bench-line)] p-2" data-testid="web-only-indicators">
            <p className="text-[var(--signal)]">
              web-only interface indicators — not physical sp-1 leds and never transmitted (the `••` marks and the red
              play triangle are printed artwork)
            </p>
            <LedRows rows={webRows} />
          </section>

          {/* ---------- contract ---------- */}
          <section className="border border-[var(--bench-line)] p-2" data-testid="behavior-contract">
            <p className="text-[var(--signal)]">behaviour contract · {BEHAVIOR_CONTRACT_VERSION} · reference data</p>
            {contract.map((c) => (
              <div key={c.id} className="border-b border-[var(--bench-line)] py-1">
                <div className="flex justify-between gap-2">
                  <span>
                    [{c.group}] {c.name}
                  </span>
                  <span className="uppercase text-[var(--signal)]">
                    audit: {c.implementationStatus} · reproduction: {c.reproductionStatus}
                  </span>
                </div>
                <div className="text-[var(--ink-faint)]">
                  from {c.initiatingState} · {c.sequence} · {c.timing}
                </div>
                <div className="text-[var(--ink-faint)]">
                  owner {c.expectedOwner} → {c.expectedCommand} → {c.expectedEngineResult}
                </div>
                <div className="text-[var(--ink-faint)]">leds: {c.expectedLedSummary}</div>
                <div className="text-[var(--ink-faint)]">
                  observation source: {c.observationSource}
                  {c.segmentId ? ` · segment ${c.segmentId}` : ""} · {c.provenance} ({c.confidence}) ·{" "}
                  {c.citation.title} {c.citation.version} — {c.citation.locator}
                </div>
                {c.observed && <div>observed state: {c.observed}</div>}
                {c.reproductionFirstDivergence && (
                  <div className="text-[var(--signal)]">first divergence: {c.reproductionFirstDivergence}</div>
                )}
                {c.notes && <div className="text-[var(--ink-faint)]">note: {c.notes}</div>}
              </div>
            ))}
          </section>

          {/* ---------- trace ---------- */}
          <section className="border border-[var(--bench-line)] p-2" data-testid="diagnostic-trace">
            <div className="flex items-center justify-between">
              <p className="text-[var(--signal)]">
                trace · {stats.stored}/{stats.capacity} · {stats.running ? "capturing" : "stopped"} ·{" "}
                {frozen ? "view frozen" : "view live"}
              </p>
            </div>
            <div className="mt-1 flex flex-wrap gap-1">
              {STAGE_FILTERS.map((f) => (
                <button
                  key={f.key}
                  type="button"
                  onClick={() => setFilter(f.key)}
                  className={`border px-1 ${filter === f.key ? "border-[var(--signal)] text-[var(--signal)]" : "border-[var(--bench-line)] text-[var(--ink-faint)]"}`}
                >
                  {f.key}
                </button>
              ))}
            </div>
            <pre
              ref={traceRef}
              data-testid="trace-list"
              className="mt-1 max-h-56 overflow-auto whitespace-pre-wrap text-[9px] text-[var(--ink-dim)]"
            >
              {shown
                .map(
                  (r) =>
                    `${(r.t - t0).toFixed(1)}ms ${r.corr ? `#${r.corr}` : "#-"}${r.gesture ? `/g${r.gesture}` : ""}${
                      typeof r.commandId === "number" ? `/c${r.commandId}` : ""
                    }${r.causeId ? `/~${r.causeId}` : ""}  ${r.stage}  ${r.label}${r.detail ? ` — ${r.detail}` : ""}`,
                )
                .join("\n") || "no records yet"}
            </pre>
          </section>

          <div className="flex flex-wrap gap-2">
            <button
              type="button"
              data-testid="copy-diagnostic-report"
              onClick={() => void doCopy()}
              className="border border-[var(--bench-line)] px-2 py-1 uppercase tracking-[0.14em] hover:text-[var(--signal)]"
            >
              copy diagnostic report{copied ? ` · ${copied}` : ""}
            </button>
            <button
              type="button"
              data-testid="download-diagnostic-json"
              onClick={doDownload}
              className="border border-[var(--bench-line)] px-2 py-1 uppercase tracking-[0.14em] hover:text-[var(--signal)]"
            >
              download json
            </button>
          </div>
        </div>
      )}
    </div>
  );
}
