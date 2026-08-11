import { memo, useEffect, useRef } from "react";
import markup from "@/assets/stem-tape-sp1-photo.svg?raw";
import { bootstrapSp1Photo, stripSvgScripts, type StemChannel, type Sp1PhotoApi } from "@/device/sp1Photo";
import type { Control } from "@/device/geometry";
import type { FaderIndex } from "@/input/faderSessions";

/** Asset markup, scripts stripped. Nothing else in the SVG is modified. */
const MARKUP = stripSvgScripts(markup);

export interface InteractiveSP1Props {
  svgRef: React.RefObject<SVGSVGElement | null>;
  /** Authoritative fader values from the Stem Tape state layer (0..1). */
  faderValues: readonly number[];
  /** Authoritative per-lane button state (mute latch) from the state layer. */
  buttonActive: readonly boolean[];
  photo: {
    faderStart: (index: FaderIndex, value: number, pointerId: number) => void;
    faderMove: (index: FaderIndex, value: number, pointerId: number) => void;
    faderEnd: (index: FaderIndex, value: number, pointerId: number, cancelled: boolean) => void;
    press: (control: Control, pointerId: number) => void;
    release: (control: Control, pointerId: number, cancelled: boolean) => void;
  };
}

type WithApi = SVGSVGElement & { stemTape?: Sp1PhotoApi };

/**
 * The photographic SP-1 IS the instrument surface.
 *
 * This wrapper only inlines the supplied asset, keeps its viewBox and aspect
 * ratio, and bridges its interaction events to the existing Stem Tape control
 * layer. It draws nothing of its own and overlays no controls.
 */
export const InteractiveSP1 = memo(function InteractiveSP1({
  svgRef,
  faderValues,
  buttonActive,
  photo,
}: InteractiveSP1Props) {
  const hostRef = useRef<HTMLDivElement | null>(null);
  const photoRef = useRef(photo);
  photoRef.current = photo;
  /** Last value this surface itself emitted, so state echo never fights a drag. */
  const localRef = useRef<number[]>([...faderValues]);
  const rockerRef = useRef<Control | null>(null);

  useEffect(() => {
    const host = hostRef.current;
    const svg = host?.querySelector("svg") as SVGSVGElement | null;
    if (!svg) return;
    svgRef.current = svg;
    svg.style.width = "100%";
    svg.style.height = "auto";
    svg.style.display = "block";
    svg.style.touchAction = "none";
    svg.setAttribute("preserveAspectRatio", "xMidYMid meet");

    const dispose = bootstrapSp1Photo(svg, {
      onFaderStart: (ch, v, id) => {
        localRef.current[ch - 1] = v;
        photoRef.current.faderStart((ch - 1) as FaderIndex, v, id);
      },
      onFaderMove: (ch, v, id) => {
        localRef.current[ch - 1] = v;
        photoRef.current.faderMove((ch - 1) as FaderIndex, v, id);
      },
      onFaderEnd: (ch, v, id, cancelled) => {
        localRef.current[ch - 1] = v;
        photoRef.current.faderEnd((ch - 1) as FaderIndex, v, id, cancelled);
      },
      onButtonDown: (ch, id) => photoRef.current.press(`track-button-${ch}` as Control, id),
      onButtonUp: (ch, id, cancelled) => photoRef.current.release(`track-button-${ch}` as Control, id, cancelled),
      onRocker: (value, id) => {
        const held = rockerRef.current;
        if (value === 0) {
          if (held) photoRef.current.release(held, id, false);
          rockerRef.current = null;
          return;
        }
        const next: Control = value === 1 ? "rocker-fwd" : "rocker-rwd";
        if (held === next) return;
        if (held) photoRef.current.release(held, id, false);
        rockerRef.current = next;
        photoRef.current.press(next, id);
      },
    });
    (window as unknown as Record<string, unknown>)["__sp1boot"] = "api:" + typeof (svg as WithApi).stemTape + ":" + String(svg.isConnected);
    // Seed the photographed caps from the authoritative state.
    const api = (svg as WithApi).stemTape;
    for (let i = 0; i < 4; i++) api?.setFader((i + 1) as StemChannel, faderValues[i] ?? 1);

    return () => {
      dispose();
      if (svgRef.current === svg) svgRef.current = null;
    };
    // Bootstrap once: handlers are read through a ref.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // STATE -> SVG. Only when the value actually differs from what this surface
  // last emitted, so a programmatic change moves the cap but a live drag does not
  // get overwritten by its own echo.
  useEffect(() => {
    const api = (svgRef.current as WithApi | null)?.stemTape;
    if (!api) return;
    for (let i = 0; i < 4; i++) {
      const v = faderValues[i];
      if (v == null) continue;
      if (Math.abs((localRef.current[i] ?? -1) - v) < 0.001) continue;
      localRef.current[i] = v;
      api.setFader((i + 1) as StemChannel, v);
    }
  }, [faderValues, svgRef]);

  useEffect(() => {
    const api = (svgRef.current as WithApi | null)?.stemTape;
    if (!api) return;
    for (let i = 0; i < 4; i++) api.setButton((i + 1) as StemChannel, !!buttonActive[i]);
  }, [buttonActive, svgRef]);

  return (
    <div
      ref={hostRef}
      className="st-sp1-photo"
      data-testid="interactive-sp1"
      dangerouslySetInnerHTML={{ __html: MARKUP }}
    />
  );
});
