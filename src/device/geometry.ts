/**
 * Baked device geometry.
 *
 * The source artwork wrapped everything in
 *   <g transform="translate(-72 0) scale(1.2 1)">
 * which is a NON-UNIFORM scale: it stretched X by 1.2 while leaving Y alone,
 * turning every circle into an ellipse and sitting between screen coordinates
 * and geometry for all pointer math.
 *
 * Per the approved SVG audit, that transform is baked out. Every X coordinate
 * below is the transformed value  x' = 1.2 * x - 72,  every width is  w * 1.2,
 * and radii stay circular (matching the physical layout reference).
 */

export const VIEWBOX_WIDTH = 720;
export const VIEWBOX_HEIGHT = 940;

/**
 * Smallest width the SVG itself renders at — NOT the viewport width.
 *
 * Measured, not assumed: at a 375 px viewport the page is `px-5` (20 px each
 * side), so getBoundingClientRect() on <svg.st-surface> returns 335.00 px.
 * Sizing hit zones off 375 produced 39.31 px targets — a real 44 px failure
 * that only the measured audit caught. 335 is the number that matters.
 */
export const MIN_RENDER_WIDTH = 334; // 1 px of headroom so 44.00 is never missed by float error
export const MIN_TOUCH_TARGET_PX = 44;

/**
 * 44 CSS px expressed in viewBox user units at MIN_RENDER_WIDTH (~94.57).
 * Still below the 97.2-unit column pitch, so zones stay non-overlapping.
 */
export const HIT_UNITS = (MIN_TOUCH_TARGET_PX * VIEWBOX_WIDTH) / MIN_RENDER_WIDTH;


export type TrackIndex = 0 | 1 | 2 | 3;

export type Control =
  | "play"
  | "function"
  | "volume-minus"
  | "volume-plus"
  | "rocker-fwd"
  | "rocker-rwd"
  | "track-button-1"
  | "track-button-2"
  | "track-button-3"
  | "track-button-4"
  | "fader-1"
  | "fader-2"
  | "fader-3"
  | "fader-4";

export const BUTTON_CONTROLS: Control[] = [
  "play",
  "function",
  "volume-minus",
  "volume-plus",
  "rocker-fwd",
  "rocker-rwd",
  "track-button-1",
  "track-button-2",
  "track-button-3",
  "track-button-4",
];

export const FADER_CONTROLS: Control[] = ["fader-1", "fader-2", "fader-3", "fader-4"];

export const CONTROL_LABELS: Record<Control, string> = {
  play: "play",
  function: "function",
  "volume-minus": "volume −",
  "volume-plus": "volume +",
  "rocker-fwd": "rocker fwd",
  "rocker-rwd": "rocker rwd",
  "track-button-1": "track 1",
  "track-button-2": "track 2",
  "track-button-3": "track 3",
  "track-button-4": "track 4",
  "fader-1": "fader 1",
  "fader-2": "fader 2",
  "fader-3": "fader 3",
  "fader-4": "fader 4",
};

/** Fader column centres, baked. */
export const FADER_X = [157.2, 254.4, 351.6, 448.8] as const;

/** Fader slot: y 381, height 112, cap radius 12 -> travel between these cy values. */
export const FADER_SLOT_Y = 381;
export const FADER_SLOT_H = 112;
export const FADER_CAP_R = 12;
export const FADER_CY_TOP = FADER_SLOT_Y + FADER_CAP_R; // 393 == value 1
export const FADER_CY_BOTTOM = FADER_SLOT_Y + FADER_SLOT_H - FADER_CAP_R; // 481 == value 0
export const FADER_TRAVEL = FADER_CY_BOTTOM - FADER_CY_TOP; // 88

export function faderValueToCy(value: number): number {
  return FADER_CY_BOTTOM - clamp01(value) * FADER_TRAVEL;
}

export function cyToFaderValue(cy: number): number {
  return clamp01((FADER_CY_BOTTOM - cy) / FADER_TRAVEL);
}

export function clamp01(n: number): number {
  return n < 0 ? 0 : n > 1 ? 1 : n;
}

export interface HitZone {
  control: Control;
  x: number;
  y: number;
  width: number;
  height: number;
}

const H = HIT_UNITS;

/**
 * Invisible interaction zones. They own every pointer event; the visible
 * artwork is pointer-events:none so small graphics never gate input.
 * Zones do not overlap: fader/track columns are 97.2 apart, zones are ~84.5.
 */
export const HIT_ZONES: HitZone[] = [
  // Top volume buttons (centres 176.4 / 276.0)
  { control: "volume-minus", x: 176.4 - H / 2, y: 78.5 - H / 2, width: H, height: H },
  { control: "volume-plus", x: 276.0 - H / 2, y: 78.5 - H / 2, width: H, height: H },

  // Left rocker, split into two stacked zones over one visual body.
  { control: "rocker-fwd", x: 80.4 - H / 2, y: 225 - H, width: H, height: H },
  { control: "rocker-rwd", x: 80.4 - H / 2, y: 225, width: H, height: H },

  // Right rail buttons (centre 637.2)
  { control: "play", x: 637.2 - H / 2, y: 180, width: H, height: 114 },
  { control: "function", x: 637.2 - H / 2, y: 646, width: H, height: 114 },

  // Faders
  ...FADER_X.map((cx, i) => ({
    control: `fader-${i + 1}` as Control,
    x: cx - H / 2,
    y: FADER_SLOT_Y - 6,
    width: H,
    height: FADER_SLOT_H + 12,
  })),

  // Track buttons
  ...FADER_X.map((cx, i) => ({
    control: `track-button-${i + 1}` as Control,
    x: cx - H / 2,
    y: 634.5 - H / 2,
    width: H,
    height: H,
  })),
];
