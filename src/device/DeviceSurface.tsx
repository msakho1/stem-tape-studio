import { useId } from "react";
import {
  FADER_CAP_R,
  FADER_SLOT_H,
  FADER_SLOT_Y,
  FADER_X,
  HIT_ZONES,
  VIEWBOX_HEIGHT,
  VIEWBOX_WIDTH,
  faderValueToCy,
  type Control,
} from "./geometry";
import type { LedFrame, LedId } from "@/machine/surface";

export interface DeviceSurfaceProps {
  svgRef: React.RefObject<SVGSVGElement | null>;
  capRefs: React.MutableRefObject<Record<number, SVGCircleElement | null>>;
  faderValues: readonly number[];
  pressed: readonly Control[];
  leds: LedFrame;
  showHitZones: boolean;
  onControlPointerDown: (control: Control, e: React.PointerEvent) => void;
  onControlPointerMove: (control: Control, e: React.PointerEvent) => void;
  onControlPointerUp: (control: Control, e: React.PointerEvent) => void;
  onControlPointerCancel: (control: Control, e: React.PointerEvent) => void;
}

function ledClass(frame: LedFrame, id: LedId): string {
  return `st-led st-led--${frame[id].pattern}`;
}

/**
 * The SP-1-style control surface.
 *
 * Corrections applied from the approved SVG audit:
 *  1. The non-uniform outer transform is baked into the coordinates and removed.
 *  2. Root role is `group`, not `img` (an img role hides the interactive subtree).
 *  3. Unused defs (deviceShadow, controlShadow, deviceClip) removed.
 *  4. Gaussian-blur LED filters replaced with static halo circles whose opacity
 *     animates — same look, compositor-only, no Safari filter cliff.
 *  5. `transform-box: fill-box` replaced with wrapper <g> transforms.
 *  Plus: the missing 4-dot side LED column (song row) added, the rocker split
 *  into two hit zones over one visual body, and all def IDs namespaced so the
 *  component can mount more than once.
 *
 * This component is deliberately dumb: props in, pixels out. No gesture logic.
 */
export function DeviceSurface({
  svgRef,
  capRefs,
  faderValues,
  pressed,
  leds,
  showHitZones,
  onControlPointerDown,
  onControlPointerMove,
  onControlPointerUp,
  onControlPointerCancel,
}: DeviceSurfaceProps) {
  const uid = useId().replace(/:/g, "");
  const body = `body-${uid}`;
  const rail = `rail-${uid}`;
  const button = `button-${uid}`;
  const cap = `cap-${uid}`;

  const isPressed = (c: Control) => pressed.includes(c);
  const pressClass = (c: Control) => `st-press${isPressed(c) ? " st-press--down" : ""}`;

  return (
    <svg
      ref={svgRef}
      viewBox={`0 0 ${VIEWBOX_WIDTH} ${VIEWBOX_HEIGHT}`}
      className="st-surface"
      role="group"
      aria-label="Unofficial Stem Tape prototype control surface"
    >
      <defs>
        <linearGradient id={body} x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stopColor="#c9c9c7" />
          <stop offset="0.55" stopColor="#bdbdbb" />
          <stop offset="1" stopColor="#b1b1af" />
        </linearGradient>
        <linearGradient id={rail} x1="0" y1="0" x2="1" y2="0">
          <stop offset="0" stopColor="#f7f0df" />
          <stop offset="1" stopColor="#fff9eb" />
        </linearGradient>
        <linearGradient id={button} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stopColor="#a2a2a0" />
          <stop offset="1" stopColor="#858583" />
        </linearGradient>
        <radialGradient id={cap} cx="35%" cy="30%" r="70%">
          <stop offset="0" stopColor="#bdbdbb" />
          <stop offset="1" stopColor="#8f8f8d" />
        </radialGradient>
      </defs>

      {/* ---- visible artwork: never receives pointer events ---- */}
      <g className="st-art">
        {/* Right rail buttons sit behind the body. */}
        <g className={pressClass("play")}>
          <rect x={614.4} y={180} width={45.6} height={114} rx={4} fill="#fff8e7" stroke="#ddd5c5" strokeWidth={1.5} />
          <rect x={620.4} y={187} width={7.2} height={100} rx={3} fill="#ffffff" opacity={0.52} />
        </g>
        <g className={pressClass("function")}>
          <rect x={614.4} y={646} width={45.6} height={114} rx={4} fill="#fff8e7" stroke="#ddd5c5" strokeWidth={1.5} />
          <rect x={620.4} y={653} width={7.2} height={100} rx={3} fill="#ffffff" opacity={0.52} />
        </g>

        {/* Left three-position rocker: one body, tilts by which half is held. */}
        <g
          className={`st-rocker${isPressed("rocker-fwd") ? " st-rocker--fwd" : ""}${
            isPressed("rocker-rwd") ? " st-rocker--rwd" : ""
          }`}
        >
          <rect x={55.2} y={192} width={50.4} height={66} rx={9} fill="#fff7e4" stroke="#d7cfbf" strokeWidth={1.5} />
          <path d="M63.6 224h33.6" stroke="#ded6c5" strokeWidth={2} />
          <path d="M70.8 205l9.6-6 9.6 6" fill="none" stroke="#bbb4a5" strokeWidth={2} />
          <path d="M70.8 244l9.6 6 9.6-6" fill="none" stroke="#bbb4a5" strokeWidth={2} />
        </g>

        {/* Body */}
        <rect x={104.4} y={105} width={511.2} height={765} rx={22} fill="#2c2c29" opacity={0.16} />
        <rect x={90} y={85} width={458.4} height={770} rx={18} fill={`url(#${body})`} />
        <path
          d="M548.4 85h60c12 0 21.6 8 21.6 18v734c0 10-9.6 18-21.6 18h-60z"
          fill={`url(#${rail})`}
        />
        <path d="M548.4 85v770" stroke="#aaa9a4" strokeWidth={1.5} opacity={0.75} />
        <rect
          x={90.9}
          y={85.75}
          width={538.2}
          height={768.5}
          rx={17}
          fill="none"
          stroke="#ffffff"
          strokeOpacity={0.24}
          strokeWidth={1.5}
        />

        {/* Top volume buttons */}
        <g className={pressClass("volume-minus")}>
          <rect x={144} y={70} width={64.8} height={17} rx={4} fill="#b5b5b3" stroke="#929290" strokeWidth={1} />
          <path d="M162 78.5h28.8" stroke="#777775" strokeWidth={2.5} strokeLinecap="round" />
        </g>
        <g className={pressClass("volume-plus")}>
          <rect x={243.6} y={70} width={64.8} height={17} rx={4} fill="#b5b5b3" stroke="#929290" strokeWidth={1} />
          <path d="M261.6 78.5h28.8M276 73v11" stroke="#777775" strokeWidth={2.5} strokeLinecap="round" />
        </g>

        {/* Faders — caps are mutated directly by the drag loop, never re-rendered */}
        {FADER_X.map((cx, i) => (
          <g key={`fader-${i}`}>
            <rect x={cx - 14.4} y={FADER_SLOT_Y} width={28.8} height={FADER_SLOT_H} rx={12} fill="#090909" />
            <rect x={cx - 2.4} y={388} width={4.8} height={98} rx={2} fill="#20201f" opacity={0.65} />
            <circle
              ref={(el) => {
                capRefs.current[i] = el;
              }}
              cx={cx}
              cy={faderValueToCy(faderValues[i] ?? 0.5)}
              r={FADER_CAP_R}
              fill={`url(#${cap})`}
              stroke="#70706e"
              strokeWidth={1.2}
            />
          </g>
        ))}

        {/* Track LEDs — halo circle + core, opacity animated (no SVG filters) */}
        {FADER_X.map((cx, i) => {
          const id = `track-led-${i + 1}` as LedId;
          return (
            <g key={id} className={ledClass(leds, id)}>
              <circle className="st-led__halo" cx={cx} cy={548} r={14} fill="#fff6d8" />
              <circle className="st-led__core" cx={cx} cy={548} r={7} fill="#ffffff" />
            </g>
          );
        })}

        {/* Track buttons */}
        {FADER_X.map((cx, i) => (
          <g key={`tb-${i}`} className={pressClass(`track-button-${i + 1}` as Control)}>
            <rect
              x={cx - 16.2}
              y={604}
              width={32.4}
              height={61}
              rx={3}
              fill={`url(#${button})`}
              stroke="#767674"
              strokeWidth={1}
            />
          </g>
        ))}

        {/* Rail: play indicator, side LED song row, function indicators */}
        <g className={`st-led st-led--${leds["play-indicator"].pattern} st-led--signal`}>
          <path className="st-led__halo" d="M589.2 213l21 33h-42z" fill="#ff5d63" />
          <path className="st-led__core" d="M589.2 219l18 27h-36z" fill="#ff5d63" />
        </g>

        {[0, 1, 2, 3].map((i) => {
          const id = `side-led-${i + 1}` as LedId;
          return (
            <g key={id} className={ledClass(leds, id)}>
              <circle className="st-led__halo" cx={589.2} cy={392 + i * 26} r={11} fill="#fff6d8" />
              <circle className="st-led__core" cx={589.2} cy={392 + i * 26} r={5.5} fill="#ffffff" />
            </g>
          );
        })}

        {[0, 1].map((i) => {
          const id = `function-led-${i + 1}` as LedId;
          return (
            <g key={id} className={`${ledClass(leds, id)} st-led--signal`}>
              <circle className="st-led__halo" cx={589.2} cy={664 + i * 20} r={12} fill="#ff5d63" />
              <circle className="st-led__core" cx={589.2} cy={664 + i * 20} r={6.5} fill="#ff5d63" />
            </g>
          );
        })}
      </g>

      {/* ---- invisible hit zones: these own every pointer event ---- */}
      <g className={`st-zones${showHitZones ? " st-zones--visible" : ""}`}>
        {HIT_ZONES.map((z) => (
          <rect
            key={z.control}
            data-control={z.control}
            x={z.x}
            y={z.y}
            width={z.width}
            height={z.height}
            rx={6}
            onPointerDown={(e) => onControlPointerDown(z.control, e)}
            onPointerMove={(e) => onControlPointerMove(z.control, e)}
            onPointerUp={(e) => onControlPointerUp(z.control, e)}
            onPointerCancel={(e) => onControlPointerCancel(z.control, e)}
            onLostPointerCapture={(e) => onControlPointerCancel(z.control, e)}
          />
        ))}
      </g>
    </svg>
  );
}
