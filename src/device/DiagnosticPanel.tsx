import { useEffect, useRef } from "react";
import { CONTROL_LABELS, FADER_X, type Control } from "@/device/geometry";
import type { LedFrame, LedId, SurfaceState } from "@/machine/surface";
import type { RawInputEvent } from "@/input/gestures";

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
  rawLog: RawInputEvent[];
  gestureLog: { id: number; text: string; t: number }[];
  faderValuesRef: React.MutableRefObject<number[]>;
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

export function DiagnosticPanel({ state, leds, rawLog, gestureLog, faderValuesRef }: Props) {
  return (
    <div className="st-panel">
      <section className="st-section">
        <h2 className="st-section__title">machine state</h2>
        <div className="st-grid">
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
            <span className="st-kv__k">active track</span>
            <span className="st-kv__v">{state.activeTrack + 1}</span>
          </div>
          <div className="st-kv">
            <span className="st-kv__k">master</span>
            <span className="st-kv__v">{state.masterVolume.toFixed(3)}</span>
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
