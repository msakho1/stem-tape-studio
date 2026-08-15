import { useEffect, useRef } from "react";
import { CONTROL_LABELS, FADER_X, type Control } from "@/device/geometry";
import type { LedFrame, LedId, SurfaceState } from "@/machine/surface";
import type { RawInputEvent } from "@/input/gestures";
import { V26_GROUP_LABEL, V26_MAP } from "@/machine/v26map";
import { HitZoneAudit } from "@/device/HitZoneAudit";
import { FX_FAMILIES } from "@/machine/stemPerformance";
import { STEM_TAPE_V1_MAP, exportMapJson } from "@/machine/stemTapeV1Map";
import type { ChordArbiter } from "@/machine/chordArbiter";
import type { EngineStatus } from "@/audio/engine";

const LED_ORDER: LedId[] = [
  "track-led-1",
  "track-led-2",
  "track-led-3",
  "track-led-4",
  "play-indicator",
  "side-led-1",
  "function-led-1",
  "function-led-2",
];

interface Props {
  state: SurfaceState;
  leds: LedFrame;
  observed: Record<string, string>;
  ready: boolean;
  powerHoldMs: number;
  setPowerHoldMs: (ms: number) => void;
  rawLog: RawInputEvent[];
  gestureLog: { id: number; text: string; t: number }[];
  faderValuesRef: React.MutableRefObject<number[]>;
  svgRef: React.RefObject<SVGSVGElement | null>;
  arbiter: ChordArbiter;
  audio: EngineStatus;
}



/** Live fader readout, driven by rAF off the same ref the drag loop writes. */
function FaderReadout({ faderValuesRef }: { faderValuesRef: React.MutableRefObject<number[]> }) {
  const cells = useRef<(HTMLSpanElement | null)[]>([]);
  useEffect(() => {
    let raf = 0;
    const tick = () => {
      faderValuesRef.current.forEach((v, i) => {
        const el = cells.current[i];
        if (el) el.textContent = v.toFixed(3);
      });
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, [faderValuesRef]);

  return (
    <div className="st-grid">
      {FADER_X.map((_, i) => (
        <div key={i} className="st-kv">
          <span className="st-kv__k">fader {i + 1}</span>
          <span
            className="st-kv__v"
            ref={(el) => {
              cells.current[i] = el;
            }}
          >
            0.000
          </span>
        </div>
      ))}
    </div>
  );
}

export function DiagnosticPanel({
  state,
  leds,
  observed,
  ready,
  powerHoldMs,
  setPowerHoldMs,
  rawLog,
  gestureLog,
  faderValuesRef,
  svgRef,
  arbiter,
  audio,
}: Props) {
  const satisfied = (r: (typeof V26_MAP)[number]) => (state.coverage[r.id] ?? 0) > 0 || observed[r.id] != null;
  const covered = V26_MAP.filter(satisfied).length;
  const exercisable = V26_MAP.filter((r) => r.status !== "doc" && r.status !== "audio").length;

  return (
    <div className="st-panel">
      <section className="st-section">
        <h2 className="st-section__title">harness gate</h2>
        <div className="st-grid">
          <div className="st-kv">
            <span className="st-kv__k">application ready</span>
            <span className="st-kv__v" data-app-ready-readout={ready}>
              {ready ? "ready — input armed" : "booting — input rejected"}
            </span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">power hold (fn)</span>
            <span className="st-kv__v">
              <button className="st-step" type="button" onClick={() => setPowerHoldMs(powerHoldMs - 100)}>
                −
              </button>
              {powerHoldMs} ms
              <button className="st-step" type="button" onClick={() => setPowerHoldMs(powerHoldMs + 100)}>
                +
              </button>
            </span>
          </div>
        </div>
      </section>

      <section className="st-section" data-testid="phase5c-diagnostics">
        <h2 className="st-section__title">stem performance + fx (phase 5c)</h2>
        <div className="st-grid">
          <div className="st-kv">
            <span className="st-kv__k">layer</span>
            <span className="st-kv__v" data-testid="fx-layer">{state.perf.fxOverlay ? "fx-overlay" : "tape"}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">active stem</span>
            <span className="st-kv__v" data-testid="active-stem">{state.perf.activeStem + 1}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">solo mask</span>
            <span className="st-kv__v">{state.perf.tracks.map((t) => (t.soloed ? "1" : "0")).join("")}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">link mask</span>
            <span className="st-kv__v">{state.perf.tracks.map((t) => (t.linked ? "1" : "0")).join("")}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">tape target</span>
            <span className="st-kv__v">
              {state.perf.tracks[state.perf.activeStem]?.linked
                ? `linked group [${state.perf.tracks.flatMap((t, i) => (t.linked ? [i + 1] : [])).join(" ")}]`
                : `stem ${state.perf.activeStem + 1} only`}
            </span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">bpm source</span>
            <span className="st-kv__v">{audio.baseBpm.toFixed(1)} ({audio.bpmSource})</span>
          </div>
          <div className="st-kv" data-testid="grid-analysis">
            <span className="st-kv__k">song grid</span>
            <span className="st-kv__v">{audio.gridDetail}</span>
          </div>
          <div className="st-kv" data-testid="scrub-candidates">
            <span className="st-kv__k">scrub landings</span>
            <span className="st-kv__v">
              {audio.scrubCandidates.map((c, i) => `${i + 1}:${c == null ? "—" : `${c.toFixed(3)}s`}`).join("  ")}
            </span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">engine source</span>
            <span className="st-kv__v">{audio.enginePreference} · worklet tracks {audio.workletTrackCount}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">rejected activation</span>
            <span className="st-kv__v">{audio.lastFxRejection ?? "none"}</span>
          </div>
        </div>

        <table className="st-table" data-testid="fx-slots">
          <thead>
            <tr>
              <th>stem</th>
              {FX_FAMILIES.map((f) => (
                <th key={f}>{f}</th>
              ))}
              <th>eff. bpm</th>
            </tr>
          </thead>
          <tbody>
            {state.perf.tracks.map((t, i) => (
              <tr key={i}>
                <td>{i + 1}</td>
                {FX_FAMILIES.map((f) => {
                  const slot = t.fx[f];
                  const rack = audio.fx[i];
                  const extra =
                    f === "echo" && rack ? ` ${rack.echo.delayS.toFixed(4)}s` :
                    f === "reverb" && rack ? ` ${rack.reverb.variation}` :
                    f === "filter" && rack ? ` ${rack.filter.layer}` : "";
                  return (
                    <td key={f}>
                      {slot.momentary ? "MOM" : slot.latched ? "LATCH" : "—"} v{slot.variation}
                      {extra}
                    </td>
                  );
                })}
                <td>{audio.effectiveBpm[i]?.toFixed(2) ?? "—"}</td>
              </tr>
            ))}
          </tbody>
        </table>

        <h3 className="st-section__title">recognised ordered chords</h3>
        <ul className="st-log">
          {arbiter.log.slice(0, 12).map((r, i) => (
            <li key={i} className="st-log__line">
              {r.intent} · [{r.controls.join(" + ")}] · suppressed [{r.suppressed.join(" ")}] · {r.detail}
            </li>
          ))}
          {arbiter.log.length === 0 && <li className="st-log__line">no chord recognised yet</li>}
        </ul>

        <button
          className="st-step"
          type="button"
          onClick={() => {
            const blob = new Blob([exportMapJson()], { type: "application/json" });
            const url = URL.createObjectURL(blob);
            const a = document.createElement("a");
            a.href = url;
            a.download = "stem-tape-v1-map.json";
            a.click();
            URL.revokeObjectURL(url);
          }}
        >
          export mapping registry ({STEM_TAPE_V1_MAP.length} rows)
        </button>
      </section>

      <section className="st-section">

        <h2 className="st-section__title">machine state (tape looper v2.6)</h2>
        <div className="st-grid">
          <div className="st-kv">
            <span className="st-kv__k">power</span>
            <span className="st-kv__v">{state.power}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">transport</span>
            <span className="st-kv__v">{state.playing ? "running" : "stopped"}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">function</span>
            <span className="st-kv__v">{state.functionHeld ? "held" : "idle"}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">rocker</span>
            <span className="st-kv__v">{state.rocker}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">speed</span>
            <span className="st-kv__v">
              {state.speed.toFixed(4)}×{state.speedGlide ? " glide" : ""}
            </span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">global loop</span>
            <span className="st-kv__v">
              1/{state.globalLoop.division} bar{" "}
              {state.globalLoop.latched ? "latched" : state.globalLoop.active ? "held" : "off"}
            </span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">window</span>
            <span className="st-kv__v">
              {state.window.start.toFixed(2)} → {state.window.end.toFixed(2)} · shift{" "}
              {state.window.shift.toFixed(2)}
              {state.window.reverse ? " · REVERSE" : ""}
            </span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">filter</span>
            <span className="st-kv__v">
              {state.filter.mode} {state.filter.amount.toFixed(2)}
            </span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">loops</span>
            <span className="st-kv__v">{state.loopMode}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">heads mode</span>
            <span className="st-kv__v">{state.headsMode ? "on" : "off"}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">grid</span>
            <span className="st-kv__v">
              {state.grid.bpm != null ? `${state.grid.bpm.toFixed(1)} BPM (${state.grid.source})` : "none"}
              {state.grid.rejected ? " · REJECTED" : ""}
            </span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">song / bank</span>
            <span className="st-kv__v">
              {state.song + 1}/16 · bank {state.bank + 1}
            </span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">lights</span>
            <span className="st-kv__v">{state.lights}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">master</span>
            <span className="st-kv__v">{state.masterVolume.toFixed(3)}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">active track</span>
            <span className="st-kv__v">{state.activeTrack + 1}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">held</span>
            <span className="st-kv__v">
              {state.pressed.length ? state.pressed.map((c) => CONTROL_LABELS[c as Control]).join(" + ") : "—"}
            </span>
          </div>
        </div>
      </section>

      <section className="st-section">
        <h2 className="st-section__title">
          v2.6 map coverage — {covered}/{V26_MAP.length} rows satisfied ({exercisable} reachable without audio)
        </h2>
        <div className="st-rows">
          {V26_MAP.map((r) => {
            const n = state.coverage[r.id] ?? 0;
            const obs = observed[r.id];
            return (
              <div key={r.id} className="st-row" data-fired={n > 0 || obs != null}>
                <span className="st-row__i">{V26_GROUP_LABEL[r.group].slice(0, 3)}</span>
                <span className="st-row__a">{r.input}</span>
                <span className="st-row__b">{r.command}</span>
                <span className="st-row__d">
                  {r.status} ·{" "}
                  {n > 0
                    ? `×${n}`
                    : obs
                      ? `observed — ${obs}`
                      : r.status === "audio" || r.status === "doc"
                        ? "n/a (no engine)"
                        : "—"}
                </span>
              </div>

            );
          })}
        </div>
      </section>

      <section className="st-section">
        <h2 className="st-section__title">v2.6 commands fired</h2>
        <div className="st-log">
          {state.fired.length === 0 && <p className="st-empty">no documented command has executed yet</p>}
          {state.fired.map((f) => (
            <div key={f.id} className="st-log__line">
              <span className="st-log__t">{(f.t / 1000).toFixed(3)}</span>
              <span>
                {f.rowId} · {f.detail}
              </span>
            </div>
          ))}
        </div>
      </section>

      <HitZoneAudit svgRef={svgRef} />


      <section className="st-section">
        <h2 className="st-section__title">fader values (live, rAF)</h2>
        <FaderReadout faderValuesRef={faderValuesRef} />
      </section>

      <section className="st-section">
        <h2 className="st-section__title">tracks</h2>
        <div className="st-rows">
          {state.tracks.map((t, i) => (
            <div key={i} className="st-row">
              <span className="st-row__i">{i + 1}</span>
              <span className="st-row__a">{t.stem}</span>
              <span className="st-row__b">{t.content}</span>
              <span className="st-row__c">{t.volume.toFixed(2)}</span>
            </div>
          ))}
        </div>
      </section>

      <section className="st-section">
        <h2 className="st-section__title">led arbitration</h2>
        <div className="st-rows">
          {LED_ORDER.map((id) => (
            <div key={id} className="st-row">
              <span className="st-row__a">{id}</span>
              <span className="st-row__b">{leds[id].pattern}</span>
              <span className="st-row__d">{leds[id].reason}</span>
            </div>
          ))}
        </div>
      </section>

      <section className="st-section">
        <h2 className="st-section__title">gesture log</h2>
        <div className="st-log">
          {gestureLog.length === 0 && <p className="st-empty">no gestures yet — touch the surface or press a key</p>}
          {gestureLog.map((g) => (
            <div key={g.id} className="st-log__line">
              <span className="st-log__t">{(g.t / 1000).toFixed(3)}</span>
              <span>{g.text}</span>
            </div>
          ))}
        </div>
      </section>

      <section className="st-section">
        <h2 className="st-section__title">raw pointer events</h2>
        <div className="st-log">
          {rawLog.length === 0 && <p className="st-empty">no input yet</p>}
          {rawLog.map((e) => (
            <div key={e.id} className="st-log__line">
              <span className="st-log__t">{(e.t / 1000).toFixed(3)}</span>
              <span>
                {e.phase} · {e.control} · id {String(e.pointerId)}
                {e.x != null ? ` · ${e.x.toFixed(0)},${e.y?.toFixed(0)}` : ""}
              </span>
            </div>
          ))}
        </div>
      </section>
    </div>
  );
}
