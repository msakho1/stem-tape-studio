/**
 * SP-1 DIAGNOSTIC — evidence-driven flight recorder.
 *
 * Collapsed by default. While closed it arms nothing: the trace ring is
 * disabled, no serial port is opened, no interval runs — UNLESS the user
 * explicitly started a capture, in which case an active-capture indicator and
 * a STOP control stay visible.
 *
 * It reports; it never corrects. No behavioural fix lives in this file, and it
 * never writes mixer or transport state.
 */

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { LedFrame, SurfaceState } from "@/machine/surface";
import { deriveLeds } from "@/machine/surface";
import { GLOBAL_SCRUB_SPEEDS } from "@/audio/inertia";
import { sp1Surface, type Sp1SurfaceEvent } from "@/audio/midi/sp1Surface";
import { webMidi, type WebMidiState } from "@/audio/midi/webMidi";
import type { ChordArbiter } from "@/machine/chordArbiter";
import { trace, traceNow, type TraceRecord, type TraceStage } from "@/diagnostics/trace";
import {
  EXPECTED_ARTIFACT,
  firmwareConsole,
  type FirmwareConsoleState,
} from "@/diagnostics/firmwareConsole";
import { BEHAVIOR_CONTRACT_VERSION, evaluateContract } from "@/diagnostics/contract";
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
  { key: "serial", stages: ["serial.line"] },
  { key: "midi", stages: ["midi.raw", "midi.recognized"] },
  { key: "decode", stages: ["surface.decoded"] },
  { key: "resync", stages: ["connection.resync", "surface.suppressed"] },
  { key: "held", stages: ["surface.held"] },
  { key: "arbitration", stages: ["gesture.arbitration", "gesture.owner", "gesture.rejected"] },
  { key: "commands", stages: ["command.surface", "command.audio"] },
  { key: "ack", stages: ["engine.ack"] },
  { key: "state", stages: ["state.transport", "state.mixer", "state.fx"] },
  { key: "led", stages: ["led.derived", "led.rendered"] },
];

interface Props {
  state: SurfaceState;
  leds: LedFrame;
  arbiter: ChordArbiter;
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
              expected {l.expectedMode} · actual {l.actualMode} · {l.brightness}
            </span>
          </div>
          <div className="text-[var(--ink-faint)]">
            period {l.periodMs ?? "—"}ms · phase {l.phaseAnchor} · {l.animation} · m0 output{" "}
            {l.m0Driven ? "implemented" : "UNRESOLVED"}
          </div>
          <div className="text-[var(--ink-faint)]">
            owner: {l.owner} · p{l.priority}
            {l.lostTo ? ` · lost to: ${l.lostTo}` : ""} · {l.source}
          </div>
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

export function Sp1DiagnosticDrawer({ state, leds, arbiter }: Props) {
  const [open, setOpen] = useState(false);
  const [capturing, setCapturing] = useState(false);
  const [tick, setTick] = useState(0);
  const [console_, setConsole] = useState<FirmwareConsoleState>(() => firmwareConsole.snapshot());
  const [midi, setMidi] = useState<WebMidiState>(() => webMidi.snapshot());
  const [rates, setRates] = useState({
    raw: 0,
    surface: 0,
    reducer: 0,
    engine: 0,
    unmatched: 0,
    duplicates: 0,
    stale: 0,
    suppressed: 0,
    faderMsgs: 0,
    faderCmds: 0,
  });
  const [physicalRows, setPhysicalRows] = useState<LedInspectionRow[]>([]);
  const [webRows, setWebRows] = useState<LedInspectionRow[]>([]);
  const [filter, setFilter] = useState("all");
  const [copied, setCopied] = useState<string | null>(null);

  const counters = useRef({ unmatched: 0, duplicates: 0, stale: 0, faderMsgs: 0 });
  const faders = useRef<number[]>([0, 0, 0, 0]);
  const battery = useRef<number | null>(null);
  const held = useRef<Set<string>>(new Set());
  const arbSeen = useRef(0);

  const active = open || capturing;

  // ---- arm / disarm -------------------------------------------------------
  useEffect(() => {
    if (!active) {
      trace.disable();
      void firmwareConsole.disconnect();
      return;
    }
    trace.enable();
    trace.record("state.transport", "diagnostic capture armed");
    const offSurface = sp1Surface.subscribe((ev: Sp1SurfaceEvent) => {
      trace.beginCorrelation();
      if (ev.type === "fader") {
        counters.current.faderMsgs += 1;
        faders.current[ev.index] = ev.value;
        trace.record(
          "surface.decoded",
          `FADER ${ev.index + 1} → ${(ev.value * 100).toFixed(0)}% (CC${20 + ev.index} / 0x${(20 + ev.index).toString(16).toUpperCase()})`,
          { index: ev.index, value: ev.value },
          ev.timestampMs,
        );
        return;
      }
      if (ev.type === "battery") {
        battery.current = ev.value;
        trace.record("surface.decoded", `battery CC24 (0x18) = ${ev.value}`, { value: ev.value }, ev.timestampMs);
        return;
      }
      if (ev.type === "down") {
        if (held.current.has(ev.control)) counters.current.duplicates += 1;
        held.current.add(ev.control);
      } else if (!held.current.delete(ev.control)) {
        counters.current.unmatched += 1;
        trace.record("connection.resync", `unmatched release ${ev.control} — baseline or replayed state`, {
          control: ev.control,
        }, ev.timestampMs);
      }
      trace.record(
        "surface.decoded",
        `${ev.control.toUpperCase()} ${ev.type === "down" ? "DOWN" : "UP"}`,
        { control: ev.control, device: ev.deviceName },
        ev.timestampMs,
      );
      trace.record("surface.held", `held: ${[...held.current].join(" + ") || "none"}`, {
        held: [...held.current],
      }, ev.timestampMs);
    });
    const offMidi = webMidi.onStateChange(setMidi);
    const offConsole = firmwareConsole.subscribe(setConsole);
    return () => {
      offSurface();
      offMidi();
      offConsole();
      trace.disable();
      void firmwareConsole.disconnect();
    };
  }, [active]);

  // ---- 4 Hz sampler: rates, arbitration ingestion, LED probe --------------
  useEffect(() => {
    if (!open) return;
    let last = { raw: 0, surf: 0, red: 0, eng: 0, fad: 0 };
    const id = window.setInterval(() => {
      const list = trace.list();
      const count = (fn: (r: TraceRecord) => boolean) => list.filter(fn).length;
      const raw = count((r) => r.stage === "midi.raw");
      const surf = count((r) => r.stage === "surface.decoded");
      const red = count((r) => r.stage === "command.surface");
      const eng = count((r) => r.stage === "command.audio" || r.stage === "engine.ack");
      const fad = count((r) => r.stage === "command.surface" && /volume|fader/i.test(r.label));
      setRates({
        raw: Math.max(0, (raw - last.raw) * 4),
        surface: Math.max(0, (surf - last.surf) * 4),
        reducer: Math.max(0, (red - last.red) * 4),
        engine: Math.max(0, (eng - last.eng) * 4),
        unmatched: counters.current.unmatched,
        duplicates: counters.current.duplicates,
        stale: counters.current.stale,
        suppressed: count((r) => r.stage === "surface.suppressed" || r.stage === "connection.resync"),
        faderMsgs: counters.current.faderMsgs,
        faderCmds: fad,
      });
      last = { raw, surf, red, eng, fad };

      // Arbitration decisions are captured by the arbiter AT the decision
      // point; this only drains its log into the ring in order.
      const log = arbiter.log;
      const fresh = log.slice(0, Math.max(0, log.length - arbSeen.current)).reverse();
      arbSeen.current = log.length;
      for (const rec of fresh) {
        trace.beginGesture();
        trace.record("gesture.arbitration", `${rec.controls.join(" + ")} → ${rec.intent}`, {
          detail: rec.detail,
          suppressed: rec.suppressed,
        });
        if (rec.intent === "none") {
          trace.record("gesture.rejected", `suppressed ${rec.suppressed.join(" + ") || "(all)"}`, {
            detail: rec.detail,
          });
        } else {
          trace.record("gesture.owner", `owner ${rec.intent}`, { detail: rec.detail });
        }
        trace.endGesture();
      }

      const now = traceNow();
      // Trace of surface commands is captured once at the dispatcher boundary
      // (diagnostics/commandTrace); the drawer never re-records them.
      const dom = probeDom();
      // deriveLeds re-run 1 s ahead exposes one-shot windows that never expire.
      const expected = deriveLeds(state, now + 1000);
      setPhysicalRows(inspectPhysicalLeds(leds, expected, dom));
      setWebRows(inspectWebOnlyIndicators(leds, expected, dom));
      trace.record("led.derived", `physical frame: ${PHYSICAL_LED_COUNT} leds`);
      setTick((t) => t + 1);
    }, 250);
    return () => window.clearInterval(id);
  }, [open, arbiter, leds, state]);

  const snapshot: StateSnapshot = useMemo(
    () => ({
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
      mutes: state.tracks.map((t) => t.content === "muted"),
      solos: state.perf.tracks.map((t, i) => `${i + 1}${t.soloed ? "S" : ""}${t.linked ? "L" : ""}`),
      heldControls: [...held.current],
      arbitrationOwner: arbiter.log[0]?.intent ?? "none",
      faders: state.tracks.map((t) => t.volume),
      faderPickup: "not implemented — engine gain follows hardware CC immediately (no baseline, no arming, no crossing)",
      buttons: Object.fromEntries(BUTTONS.map((b) => [b, state.pressed.includes(b)])),
    }),
    // tick keeps the held-set / arbiter refs live without extra state writes
    [state, arbiter, tick],
  );

  const contract = useMemo(() => evaluateContract(state), [state, tick]);

  const eventRates: EventRates = useMemo(
    () => ({
      rawMidiPerSec: rates.raw,
      surfaceEventsPerSec: rates.surface,
      reducerCommandsPerSec: rates.reducer,
      engineCommandsPerSec: rates.engine,
      unmatchedReleases: rates.unmatched,
      duplicatePresses: rates.duplicates,
      staleEvents: rates.stale,
      suppressed: rates.suppressed,
      faderMessages: rates.faderMsgs,
      faderReducerCommands: rates.faderCmds,
    }),
    [rates],
  );

  const report = useCallback((): DiagnosticReport => {
    const midiIn = midi.devices[0] ?? null;
    const observation: Record<string, ObservationKind> = {
      "serial console lines": console_.lineCount > 0 ? "browser-observed" : "not-observed",
      "midi input": midiIn ? "browser-observed" : "not-observed",
      "physical led state": "not-observed",
      "logical led derivation": "browser-observed",
      "behaviour contract expectations": "not-observed",
      "firmware serial content": console_.lineCount > 0 ? "browser-observed" : "not-observed",
      "track 1/4 resync sequence": "injected/simulated",
      "physical side-row playback index": "not-observed",
    };
    return buildReport({
      contractVersion: BEHAVIOR_CONTRACT_VERSION,
      generatedAt: new Date().toISOString(),
      expectedArtifact: EXPECTED_ARTIFACT,
      midiContract: { rows: SP1_MIDI_CONTRACT, warning: SP1_NOTATION_WARNING },
      ledModel: M0_LED_COVERAGE,
      device: {
        midiInputName: midiIn?.name ?? null,
        midiInputId: midiIn?.id ?? null,
        midiOutputName: null,
        midiState: midi.status,
        consoleState: console_.status,
        reportedFirmwareVersion: console_.reportedVersion,
        capabilities: M0_CAPABILITIES as Record<string, string>,
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
      trace: trace.list(),
      contract,
      physicalLeds: physicalRows,
      webOnlyIndicators: webRows,
      failures: contract
        .filter((c) => c.status === "missing" || c.status === "conflicting" || c.status === "partial")
        .map((c) => ({
          id: c.id,
          lastGoodStage: c.observed ? "reducer state observed" : "command emitted",
          firstDivergence: c.notes ?? c.firstDivergence ?? "no LED derivation for this state",
          category: c.firstDivergence ?? null,
          expected: c.expectedLedSummary,
          actual: c.observed ?? "not derivable from reducer state",
          requiresHardware: c.provenance === "PHYSICAL_OBSERVATION" || c.provenance === "M0_DIAGNOSTIC_ONLY",
          observation: (c.observed ? "browser-observed" : "not-observed") as ObservationKind,
        })),
      unverified: contract
        .filter((c) => c.status === "unverified" || c.provenance === "UNVERIFIED")
        .map((c) => `${c.id}: ${c.citation.title}`),
      observation,
    });
  }, [midi, console_, snapshot, eventRates, contract, physicalRows, webRows]);

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
  const recent: TraceRecord[] = trace
    .recent(200)
    .filter((r) => activeFilter.stages.length === 0 || activeFilter.stages.includes(r.stage))
    .slice(0, 80);
  const t0 = recent[recent.length - 1]?.t ?? 0;

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

      {!open && capturing && (
        <div
          className="mt-1 flex items-center justify-between border border-[var(--signal)] px-2 py-1 font-mono text-[10px] uppercase tracking-[0.14em] text-[var(--signal)]"
          data-testid="capture-indicator"
        >
          <span>● capture running · {trace.size()}/500</span>
          <button type="button" data-testid="stop-capture" onClick={() => setCapturing(false)}>
            stop capture
          </button>
        </div>
      )}

      {open && (
        <div className="mt-2 space-y-3 font-mono text-[10px] leading-relaxed" data-testid="sp1-diagnostic-body">
          {/* ---------- device ---------- */}
          <section className="border border-[var(--bench-line)] p-2">
            <p className="text-[var(--signal)]">device</p>
            <Row k="midi input" v={midi.devices[0]?.name ?? "none"} />
            <Row k="midi input id" v={midi.devices[0]?.id ?? "—"} />
            <Row k="midi output" v="none (M0 exposes no host→device path)" />
            <Row k="midi state" v={midi.status} />
            <Row k="sp-1 recognition" v={midi.devices[0]?.name ? "matched by name (case-insensitive, suffixes allowed)" : "no port"} />
            <Row k="firmware console" v={console_.status} />
            <Row k="reported banner" v={console_.reportedVersion ?? "not reported"} />
            <Row k="usb identity" v={`VID ${EXPECTED_ARTIFACT.usbVendorId} · PID ${EXPECTED_ARTIFACT.usbProductId}`} />
            <p className="mt-2 text-[var(--ink-faint)]">expected artifact metadata — not verified device identity</p>
            {Object.entries(EXPECTED_ARTIFACT).map(([k, v]) => (
              <Row key={k} k={k} v={<span className="break-all">{v}</span>} />
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
              <button
                type="button"
                data-testid="toggle-capture"
                onClick={() => setCapturing((v) => !v)}
                className="border border-[var(--bench-line)] px-2 py-1 uppercase tracking-[0.14em] hover:text-[var(--signal)]"
              >
                {capturing ? "stop capture" : "keep capturing when collapsed"}
              </button>
            </div>
            <Row k="last valid serial line" v={console_.lineCount ? `#${console_.lineCount}` : "none"} />
            {console_.error && <Row k="serial parse/connect error" v={console_.error} />}
            {!console_.supported && (
              <p className="mt-1 text-[var(--ink-faint)]">
                web serial unavailable in this browser — MIDI control continues without the console
              </p>
            )}
          </section>

          {/* ---------- midi contract reference ---------- */}
          <section className="border border-[var(--bench-line)] p-2" data-testid="midi-contract">
            <p className="text-[var(--signal)]">expected m0 midi contract · channel 1</p>
            <p className="text-[var(--ink-faint)]">{SP1_NOTATION_WARNING}</p>
            {SP1_MIDI_CONTRACT.map((r) => (
              <Row key={`${r.kind}${r.dec}`} k={`${r.kind === "note" ? "note" : "CC"} ${r.dec} (${r.hex})`} v={`${r.name}${r.note ? ` · ${r.note}` : ""}`} />
            ))}
            {SP1_BUTTON_PHASES.map((p) => (
              <p key={p} className="text-[var(--ink-faint)]">
                {p}
              </p>
            ))}
          </section>

          {/* ---------- live physical state ---------- */}
          <section className="border border-[var(--bench-line)] p-2">
            <p className="text-[var(--signal)]">live physical state</p>
            <Row k="buttons (10)" v={BUTTONS.map((b) => (state.pressed.includes(b) ? "1" : "0")).join("")} />
            <Row k="faders" v={faders.current.map((f) => f.toFixed(2)).join(" ")} />
            <Row k="battery CC24" v={battery.current ?? "—"} />
            <Row k="fader pickup" v={snapshot.faderPickup} />
            <Row k="raw midi /s" v={rates.raw} />
            <Row k="surface events /s" v={rates.surface} />
            <Row k="reducer commands /s" v={rates.reducer} />
            <Row k="engine commands /s" v={rates.engine} />
            <Row k="fader messages (total)" v={rates.faderMsgs} />
            <Row k="fader reducer commands" v={rates.faderCmds} />
            <Row k="unmatched releases" v={rates.unmatched} />
            <Row k="duplicate presses" v={rates.duplicates} />
            <Row k="baseline/resync suppressed" v={rates.suppressed} />
            <Row k="stale events" v={rates.stale} />
            <Row k="AIN0 / AIN1" v={`${console_.ain0 ?? "—"} / ${console_.ain1 ?? "—"}`} />
            <Row k="decoded mask" v={console_.decodedMask ?? "—"} />
            <Row k="stable mask" v={console_.stableMask ?? "—"} />
            <Row k="unmeasured count" v={console_.unmeasured ?? "—"} />
            <Row k="held controls" v={[...held.current].join(" + ") || "none"} />
            {rates.raw > 40 && <p className="text-[var(--signal)]">excessive idle fader activity ({rates.raw}/s)</p>}
            {rates.faderMsgs > 0 && rates.faderCmds > 0 && (
              <p className="text-[var(--signal)]">
                fader traffic is changing mixer state with no pickup ownership ({rates.faderCmds} reducer commands)
              </p>
            )}
            {held.current.size > 0 && rates.surface === 0 && (
              <p className="text-[var(--signal)]">possible stuck control: {[...held.current].join(" + ")}</p>
            )}
          </section>

          {/* ---------- transport & gestures ---------- */}
          <section className="border border-[var(--bench-line)] p-2">
            <p className="text-[var(--signal)]">transport, mixer &amp; gestures</p>
            <Row k="transport" v={snapshot.playing ? "playing" : "stopped"} />
            <Row k="global loop" v={`${snapshot.globalLoop} · 1/${snapshot.loopDivision}`} />
            <Row
              k="scrub"
              v={`${snapshot.scrubDirection} · level ${snapshot.scrubSpeedLevel} (${snapshot.scrubMultiplier}×) · ${snapshot.scrubLatched ? "latched" : "momentary"}`}
            />
            <Row k="inertia" v={snapshot.inertia} />
            <Row k="active stem" v={snapshot.activeStem} />
            <Row k="track gains" v={snapshot.faders.map((f) => f.toFixed(2)).join(" ")} />
            <Row k="mutes" v={snapshot.mutes.map((m) => (m ? "M" : "-")).join("")} />
            <Row k="solo / link" v={snapshot.solos.join(" ")} />
            <Row k="fx overlay" v={snapshot.fxOverlay ? `open · ${snapshot.fxScope}` : "closed"} />
            <Row k="fx bank" v={snapshot.fxBank + 1} />
            <Row k="fx momentary" v={snapshot.fxMomentary.join(", ") || "none"} />
            <Row k="fx latched" v={snapshot.fxLatched.join(", ") || "none"} />
            <Row k="arbitration owner" v={snapshot.arbitrationOwner} />
          </section>

          {/* ---------- physical LED inspector ---------- */}
          <section className="border border-[var(--bench-line)] p-2" data-testid="physical-led-inspector">
            <p className="text-[var(--signal)]">
              physical sp-1 frame · {physicalRows.length || PHYSICAL_LED_COUNT} leds (4 track + 4 side/status) ·
              electrical gpio coverage {M0_LED_COVERAGE.electricalCoverage} · stem tape behaviour mapping{" "}
              {M0_LED_COVERAGE.behaviorCoverage} · host→device feedback {M0_LED_COVERAGE.hostToDeviceLedFeedback}
            </p>
            <LedRows rows={physicalRows} />
          </section>

          <section className="border border-[var(--bench-line)] p-2" data-testid="web-only-indicators">
            <p className="text-[var(--signal)]">
              web-only interface indicators — not physical sp-1 leds (the `••` marks and the red play triangle are
              printed artwork)
            </p>
            <LedRows rows={webRows} />
          </section>

          {/* ---------- contract ---------- */}
          <section className="border border-[var(--bench-line)] p-2" data-testid="behavior-contract">
            <p className="text-[var(--signal)]">behaviour contract · {BEHAVIOR_CONTRACT_VERSION}</p>
            {contract.map((c) => (
              <div key={c.id} className="border-b border-[var(--bench-line)] py-1">
                <div className="flex justify-between gap-2">
                  <span>
                    [{c.group}] {c.name}
                  </span>
                  <span className="uppercase text-[var(--signal)]">{c.status}</span>
                </div>
                <div className="text-[var(--ink-faint)]">
                  from {c.initiatingState} · {c.sequence} · {c.timing}
                </div>
                <div className="text-[var(--ink-faint)]">
                  owner {c.expectedOwner} → {c.expectedCommand} → {c.expectedEngineResult}
                </div>
                <div className="text-[var(--ink-faint)]">leds: {c.expectedLedSummary}</div>
                <div className="text-[var(--ink-faint)]">
                  p{c.precedence}
                  {c.competing.length ? ` vs ${c.competing.join(", ")}` : ""} · {c.provenance} ({c.confidence}) ·{" "}
                  {c.citation.title} {c.citation.version} — {c.citation.locator} [{c.citation.evidence}]
                </div>
                {c.observed && <div>observed: {c.observed}</div>}
                {c.firstDivergence && <div className="text-[var(--signal)]">first divergence: {c.firstDivergence}</div>}
                {c.notes && <div className="text-[var(--signal)]">{c.notes}</div>}
              </div>
            ))}
          </section>

          {/* ---------- trace ---------- */}
          <section className="border border-[var(--bench-line)] p-2" data-testid="diagnostic-trace">
            <div className="flex items-center justify-between">
              <p className="text-[var(--signal)]">trace · {trace.size()}/500</p>
              <button type="button" onClick={() => trace.clear()} className="uppercase tracking-[0.14em]">
                clear
              </button>
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
            <pre className="mt-1 max-h-56 overflow-auto whitespace-pre-wrap text-[9px] text-[var(--ink-dim)]">
              {recent
                .map(
                  (r) =>
                    `${(r.t - t0).toFixed(1)}ms ${r.corr ? `#${r.corr}` : "#-"}${r.gesture ? `/g${r.gesture}` : ""}  ${r.stage}  ${r.label}${r.detail ? ` — ${r.detail}` : ""}`,
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
