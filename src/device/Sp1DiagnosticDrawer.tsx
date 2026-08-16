/**
 * SP-1 DIAGNOSTIC — evidence-driven flight recorder.
 *
 * Collapsed by default. While closed it arms nothing: the trace ring is
 * disabled, no serial port is opened, no interval runs. Opening it enables the
 * ring buffer, subscribes to the SP-1 surface adapter and starts a 4 Hz sampler
 * for rates and derived state.
 *
 * It reports; it never corrects. No behavioural fix lives in this file.
 */

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { LedFrame, SurfaceState } from "@/machine/surface";
import { deriveLeds } from "@/machine/surface";
import { GLOBAL_SCRUB_SPEEDS } from "@/audio/inertia";
import { sp1Surface, type Sp1SurfaceEvent } from "@/audio/midi/sp1Surface";
import { webMidi, type WebMidiState } from "@/audio/midi/webMidi";
import type { ChordArbiter } from "@/machine/chordArbiter";
import { trace, traceNow, type TraceRecord } from "@/diagnostics/trace";
import {
  EXPECTED_ARTIFACT,
  firmwareConsole,
  type FirmwareConsoleState,
} from "@/diagnostics/firmwareConsole";
import { evaluateContract } from "@/diagnostics/contract";
import { BEHAVIOR_CONTRACT_VERSION } from "@/diagnostics/contract";
import { inspectLeds, probeDom, type LedInspectionRow } from "@/diagnostics/leds";
import { buildReport, reportToText, type DiagnosticReport, type StateSnapshot } from "@/diagnostics/report";

const BUTTONS = [
  "track-button-1",
  "track-button-2",
  "track-button-3",
  "track-button-4",
  "play",
  "volume-plus",
  "volume-minus",
  "rocker-fwd",
  "rocker-rwd",
  "function",
] as const;

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

export function Sp1DiagnosticDrawer({ state, leds, arbiter }: Props) {
  const [open, setOpen] = useState(false);
  const [tick, setTick] = useState(0);
  const [console_, setConsole] = useState<FirmwareConsoleState>(() => firmwareConsole.snapshot());
  const [midi, setMidi] = useState<WebMidiState>(() => webMidi.snapshot());
  const [rates, setRates] = useState({ raw: 0, surface: 0, engine: 0, unmatched: 0, stale: 0, suppressed: 0 });
  const [ledRows, setLedRows] = useState<LedInspectionRow[]>([]);
  const [copied, setCopied] = useState<string | null>(null);

  const counters = useRef({ raw: 0, surface: 0, engine: 0, unmatched: 0, stale: 0, suppressed: 0 });
  const faders = useRef<number[]>([0, 0, 0, 0]);
  const held = useRef<Set<string>>(new Set());
  const arbSeen = useRef(0);

  // ---- arm / disarm -------------------------------------------------------
  useEffect(() => {
    if (!open) {
      trace.disable();
      void firmwareConsole.disconnect();
      return;
    }
    trace.enable();
    trace.record("state.transport", "diagnostic capture armed");
    const offSurface = sp1Surface.subscribe((ev: Sp1SurfaceEvent) => {
      counters.current.surface += 1;
      if (ev.type === "fader") {
        faders.current[ev.index] = ev.value;
        trace.record("surface.decoded", `FADER ${ev.index + 1} → ${(ev.value * 100).toFixed(0)}%`, {
          index: ev.index,
          value: ev.value,
        }, ev.timestampMs);
        return;
      }
      if (ev.type === "battery") {
        trace.record("surface.decoded", `battery ${ev.value}`, { value: ev.value }, ev.timestampMs);
        return;
      }
      if (ev.type === "down") held.current.add(ev.control);
      else if (!held.current.delete(ev.control)) counters.current.unmatched += 1;
      trace.record("surface.decoded", `${ev.control.toUpperCase()} ${ev.type === "down" ? "DOWN" : "UP"}`, {
        control: ev.control,
        device: ev.deviceName,
      }, ev.timestampMs);
      trace.record("surface.held", `held: ${[...held.current].join(" + ") || "none"}`, {
        held: [...held.current],
      }, ev.timestampMs);
    });
    const offMidi = webMidi.onStateChange(setMidi);
    const offConsole = firmwareConsole.subscribe(setConsole);
    const offTrace = trace.subscribe(() => {
      counters.current.raw = trace.list().filter((r) => r.stage === "midi.raw").length;
    });
    return () => {
      offSurface();
      offMidi();
      offConsole();
      offTrace();
      trace.disable();
      void firmwareConsole.disconnect();
    };
  }, [open]);

  // ---- 4 Hz sampler: rates, arbitration ingestion, LED probe --------------
  useEffect(() => {
    if (!open) return;
    let lastRaw = 0;
    let lastSurface = 0;
    let lastEngine = 0;
    const id = window.setInterval(() => {
      const list = trace.list();
      const raw = list.filter((r) => r.stage === "midi.raw").length;
      const surf = list.filter((r) => r.stage === "surface.decoded").length;
      const eng = list.filter((r) => r.stage === "command.audio" || r.stage === "engine.ack").length;
      setRates({
        raw: Math.max(0, (raw - lastRaw) * 4),
        surface: Math.max(0, (surf - lastSurface) * 4),
        engine: Math.max(0, (eng - lastEngine) * 4),
        unmatched: counters.current.unmatched,
        stale: counters.current.stale,
        suppressed: list.filter((r) => r.stage === "surface.suppressed").length,
      });
      lastRaw = raw;
      lastSurface = surf;
      lastEngine = eng;

      // Arbitration decisions are captured by the arbiter AT the decision
      // point; this only drains its log into the ring in order.
      const log = arbiter.log;
      const fresh = log.slice(0, Math.max(0, log.length - arbSeen.current)).reverse();
      arbSeen.current = log.length;
      for (const rec of fresh) {
        trace.record("gesture.arbitration", `${rec.controls.join(" + ")} → ${rec.intent}`, {
          detail: rec.detail,
          suppressed: rec.suppressed,
        });
        if (rec.intent === "none") {
          trace.record("gesture.rejected", `suppressed ${rec.suppressed.join(" + ")}`, { detail: rec.detail });
        } else {
          trace.record("gesture.owner", `owner ${rec.intent}`, { detail: rec.detail });
        }
      }

      const now = traceNow();
      const rows = inspectLeds(leds, deriveLeds(state, now + 1000), probeDom());
      setLedRows(rows);
      trace.record("led.derived", `frame: ${rows.map((r) => r.actualPattern[0]).join("")}`);
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
      heldControls: [...held.current],
      arbitrationOwner: arbiter.log[0]?.intent ?? "none",
      faders: state.tracks.map((t) => t.volume),
      faderPickup: "engine gain follows hardware CC immediately (no pickup arming)",
      buttons: Object.fromEntries(BUTTONS.map((b) => [b, state.pressed.includes(b)])),
    }),
    // tick keeps the held-set / arbiter refs live without extra state writes
    [state, arbiter, tick],
  );

  const contract = useMemo(() => evaluateContract(state), [state, tick]);

  const report = useCallback((): DiagnosticReport => {
    const midiIn = midi.devices[0] ?? null;
    return buildReport({
      contractVersion: BEHAVIOR_CONTRACT_VERSION,
      generatedAt: new Date().toISOString(),
      expectedArtifact: EXPECTED_ARTIFACT,
      device: {
        midiInputName: midiIn?.name ?? null,
        midiInputId: midiIn?.id ?? null,
        midiOutputName: null,
        midiState: midi.status,
        consoleState: console_.status,
        reportedFirmwareVersion: console_.reportedVersion,
        capabilities: {
          "control input": "supported",
          "CDC diagnostics": "supported",
          "host→device physical LED feedback": "unsupported",
          "direct binary-hash verification": "unsupported",
          "unmeasured resistor-ladder chords": "present",
        },
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
      rates: {
        rawMidiPerSec: rates.raw,
        surfaceEventsPerSec: rates.surface,
        engineCommandsPerSec: rates.engine,
        unmatchedReleases: rates.unmatched,
        staleEvents: rates.stale,
        suppressed: rates.suppressed,
      },
      trace: trace.list(),
      contract,
      leds: ledRows,
      failures: contract
        .filter((c) => c.status === "missing" || c.status === "conflicting")
        .map((c) => ({
          id: c.id,
          lastGoodStage: c.observed ? "reducer state correct" : "command emitted",
          firstDivergence: c.notes ?? "no LED derivation for this state",
        })),
    });
  }, [midi, console_, snapshot, rates, contract, ledRows]);

  const doCopy = useCallback(async () => {
    const text = reportToText(report());
    try {
      await navigator.clipboard.writeText(text);
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

  const recent: TraceRecord[] = trace.recent(60);
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

      {open && (
        <div className="mt-2 space-y-3 font-mono text-[10px] leading-relaxed" data-testid="sp1-diagnostic-body">
          {/* ---------- device ---------- */}
          <section className="border border-[var(--bench-line)] p-2">
            <p className="text-[var(--signal)]">device</p>
            <Row k="midi input" v={midi.devices[0]?.name ?? "none"} />
            <Row k="midi input id" v={midi.devices[0]?.id ?? "—"} />
            <Row k="midi output" v="none (M0 exposes no host→device path)" />
            <Row k="midi state" v={midi.status} />
            <Row k="firmware console" v={console_.status} />
            <Row k="reported banner" v={console_.reportedVersion ?? "not reported"} />
            <p className="mt-2 text-[var(--ink-faint)]">expected artifact metadata — not verified device identity</p>
            {Object.entries(EXPECTED_ARTIFACT).map(([k, v]) => (
              <Row key={k} k={k} v={<span className="break-all">{v}</span>} />
            ))}
            <p className="mt-2 text-[var(--ink-faint)]">capabilities</p>
            <Row k="control input" v="supported" />
            <Row k="CDC diagnostics" v="supported" />
            <Row k="host→device LED feedback" v="unsupported" />
            <Row k="binary-hash verification" v="unsupported" />
            <Row k="unmeasured ladder chords" v="present" />
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
            {!console_.supported && (
              <p className="mt-1 text-[var(--ink-faint)]">
                web serial unavailable in this browser — MIDI control continues without the console
              </p>
            )}
          </section>

          {/* ---------- live physical state ---------- */}
          <section className="border border-[var(--bench-line)] p-2">
            <p className="text-[var(--signal)]">live physical state</p>
            <Row k="buttons" v={BUTTONS.map((b) => (state.pressed.includes(b) ? "1" : "0")).join("")} />
            <Row k="faders" v={faders.current.map((f) => f.toFixed(2)).join(" ")} />
            <Row k="fader pickup" v={snapshot.faderPickup} />
            <Row k="raw midi /s" v={rates.raw} />
            <Row k="surface events /s" v={rates.surface} />
            <Row k="engine commands /s" v={rates.engine} />
            <Row k="unmatched releases" v={rates.unmatched} />
            <Row k="baseline suppressed" v={rates.suppressed} />
            <Row k="stale events" v={rates.stale} />
            <Row k="AIN0 / AIN1" v={`${console_.ain0 ?? "—"} / ${console_.ain1 ?? "—"}`} />
            <Row k="decoded mask" v={console_.decodedMask ?? "—"} />
            <Row k="stable mask" v={console_.stableMask ?? "—"} />
            <Row k="unmeasured count" v={console_.unmeasured ?? "—"} />
            {rates.raw > 40 && <p className="text-[var(--signal)]">excessive idle fader activity ({rates.raw}/s)</p>}
            {held.current.size > 0 && rates.surface === 0 && (
              <p className="text-[var(--signal)]">possible stuck control: {[...held.current].join(" + ")}</p>
            )}
          </section>

          {/* ---------- transport & gestures ---------- */}
          <section className="border border-[var(--bench-line)] p-2">
            <p className="text-[var(--signal)]">transport &amp; gestures</p>
            <Row k="transport" v={snapshot.playing ? "playing" : "stopped"} />
            <Row k="global loop" v={`${snapshot.globalLoop} · 1/${snapshot.loopDivision}`} />
            <Row k="scrub" v={`${snapshot.scrubDirection} · level ${snapshot.scrubSpeedLevel} (${snapshot.scrubMultiplier}×) · ${snapshot.scrubLatched ? "latched" : "momentary"}`} />
            <Row k="inertia" v={snapshot.inertia} />
            <Row k="active stem" v={snapshot.activeStem} />
            <Row k="fx overlay" v={snapshot.fxOverlay ? `open · ${snapshot.fxScope}` : "closed"} />
            <Row k="fx bank" v={snapshot.fxBank + 1} />
            <Row k="fx momentary" v={snapshot.fxMomentary.join(", ") || "none"} />
            <Row k="fx latched" v={snapshot.fxLatched.join(", ") || "none"} />
            <Row k="held controls" v={snapshot.heldControls.join(" + ") || "none"} />
            <Row k="arbitration owner" v={snapshot.arbitrationOwner} />
          </section>

          {/* ---------- LED inspector ---------- */}
          <section className="border border-[var(--bench-line)] p-2" data-testid="led-inspector">
            <p className="text-[var(--signal)]">led inspector (virtual only — M0 cannot receive LED feedback)</p>
            {ledRows.slice(0, 8).map((l) => (
              <div key={l.id} className="border-b border-[var(--bench-line)] py-1">
                <div className="flex justify-between gap-2">
                  <span>{l.id}</span>
                  <span className="text-[var(--ink-faint)]">
                    expected {l.expectedMode} · actual {l.actualMode} · {l.brightness}
                  </span>
                </div>
                <div className="text-[var(--ink-faint)]">
                  owner: {l.owner} · p{l.priority} · {l.animation} · {l.source}
                </div>
                {l.mismatch && <div className="text-[var(--signal)]">mismatch: {l.mismatch}</div>}
              </div>
            ))}
          </section>

          {/* ---------- contract ---------- */}
          <section className="border border-[var(--bench-line)] p-2" data-testid="behavior-contract">
            <p className="text-[var(--signal)]">behaviour contract · {BEHAVIOR_CONTRACT_VERSION}</p>
            {contract.map((c) => (
              <div key={c.id} className="border-b border-[var(--bench-line)] py-1">
                <div className="flex justify-between gap-2">
                  <span>{c.name}</span>
                  <span className="uppercase text-[var(--signal)]">{c.status}</span>
                </div>
                <div className="text-[var(--ink-faint)]">
                  {c.sequence} → {c.expectedCommand} · leds: {c.expectedLeds}
                </div>
                <div className="text-[var(--ink-faint)]">
                  p{c.precedence} · {c.provenance} · {c.reference}
                </div>
                {c.observed && <div>observed: {c.observed}</div>}
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
            <pre className="mt-1 max-h-56 overflow-auto whitespace-pre-wrap text-[9px] text-[var(--ink-dim)]">
              {recent.map((r) => `${(r.t - t0).toFixed(1)}ms  ${r.stage}  ${r.label}${r.detail ? ` — ${r.detail}` : ""}`).join("\n") || "no records yet"}
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
