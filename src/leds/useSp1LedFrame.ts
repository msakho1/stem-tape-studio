/**
 * Single application-owned LED clock.
 *
 * One requestAnimationFrame ticker samples the authoritative resolver. The DOM
 * and the physical MIDI sink read the SAME sampled frame, so phase can never
 * diverge. The ticker only runs while at least one resolved LED is animated,
 * and it never re-renders React on animation ticks: brightness is written
 * straight to the SVG cores through refs.
 *
 * Semantic frames (mode/owner/precedence changes) are traced and transmitted
 * once. Animation samples are coalesced by the transport, never traced
 * individually.
 */

import { useEffect, useRef, useState } from "react";
import type { SurfaceState } from "@/machine/surface";
import { trace } from "@/diagnostics/trace";
import { ledTransport } from "@/diagnostics/ledTransport";
import {
  PHYSICAL_LED_MAP,
  formatSp1Frame,
  resolveSp1LedFrame,
  sp1LedStateFrom,
  type ResolvedPhysicalLedFrame,
} from "./sp1LedEngine";

const nowMs = () => (typeof performance !== "undefined" ? performance.now() : Date.now());

function writeDom(frame: ResolvedPhysicalLedFrame): void {
  if (typeof document === "undefined") return;
  for (const led of frame.leds) {
    const el = document.querySelector<SVGGElement>(`[data-led="${led.id}"]`);
    const core = el?.querySelector<SVGElement>(".st-led__core");
    if (!core) continue;
    core.style.opacity = String(Math.round((led.brightness / 127) * 1000) / 1000);
    if (el!.getAttribute("data-led-mode") !== led.mode) el!.setAttribute("data-led-mode", led.mode);
  }
}

export interface Sp1LedFrameHandle {
  /** Latest SEMANTIC frame — stable across animation ticks. */
  frame: ResolvedPhysicalLedFrame;
  /** Reads the live sampled frame without subscribing to it. */
  sample: () => ResolvedPhysicalLedFrame;
}

export function useSp1LedFrame(state: SurfaceState): Sp1LedFrameHandle {
  const [semantic, setSemantic] = useState<ResolvedPhysicalLedFrame>(() =>
    resolveSp1LedFrame(sp1LedStateFrom(state, nowMs()), 0),
  );
  const stateRef = useRef(state);
  stateRef.current = state;
  const sigRef = useRef<string | null>(null);
  const latest = useRef(semantic);

  useEffect(() => {
    let raf = 0;
    let stopped = false;
    const t0 = nowMs();

    const tick = () => {
      if (stopped) return;
      const t = nowMs();
      const frame = resolveSp1LedFrame(sp1LedStateFrom(stateRef.current, t), t - t0);
      latest.current = frame;
      writeDom(frame);

      if (frame.signature !== sigRef.current) {
        sigRef.current = frame.signature;
        setSemantic(frame);
        trace.recordIfChanged("led.derived", frame.signature, "led.derived", formatSp1Frame(frame));
        ledTransport.present(frame);
      } else if (frame.animated) {
        ledTransport.presentAnimationFrame(frame.values);
      }

      raf = frame.animated || true ? requestAnimationFrame(tick) : 0;
    };

    if (typeof requestAnimationFrame === "function") raf = requestAnimationFrame(tick);
    return () => {
      stopped = true;
      if (raf) cancelAnimationFrame(raf);
    };
  }, []);

  return { frame: semantic, sample: () => latest.current };
}

export { PHYSICAL_LED_MAP };
