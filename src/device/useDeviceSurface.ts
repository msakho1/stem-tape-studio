import { useCallback, useEffect, useMemo, useReducer, useRef, useState } from "react";
import {
  clamp01,
  cyToFaderValue,
  faderValueToCy,
  type Control,
  type TrackIndex,
} from "@/device/geometry";
import { GestureEngine, describeGesture, type Gesture, type RawInputEvent } from "@/input/gestures";
import { deriveLeds, initialSurfaceState, type SurfaceState } from "@/machine/surface";

const KEY_MAP: Record<string, Control> = {
  KeyQ: "rocker-fwd",
  KeyA: "rocker-rwd",
  Space: "play",
  KeyF: "function",
  Minus: "volume-minus",
  Equal: "volume-plus",
  Digit1: "track-button-1",
  Digit2: "track-button-2",
  Digit3: "track-button-3",
  Digit4: "track-button-4",
};

export const KEY_HINTS: [string, Control][] = Object.entries(KEY_MAP).map(([k, c]) => [
  k.replace("Key", "").replace("Digit", "").replace("Space", "space").replace("Minus", "−").replace("Equal", "+"),
  c,
]);

type Action =
  | { type: "press"; control: Control }
  | { type: "release"; control: Control }
  | { type: "gesture"; gesture: Gesture }
  | { type: "faderCommit"; index: number; value: number };

function reducer(state: SurfaceState, action: Action): SurfaceState {
  switch (action.type) {
    case "press": {
      if (state.pressed.includes(action.control)) return state;
      const pressed = [...state.pressed, action.control];
      return {
        ...state,
        pressed,
        functionHeld: action.control === "function" ? true : state.functionHeld,
        rocker:
          action.control === "rocker-fwd"
            ? "forward"
            : action.control === "rocker-rwd"
              ? "rewind"
              : state.rocker,
      };
    }
    case "release": {
      const pressed = state.pressed.filter((c) => c !== action.control);
      return {
        ...state,
        pressed,
        functionHeld: action.control === "function" ? false : state.functionHeld,
        rocker: action.control.startsWith("rocker") ? "center" : state.rocker,
      };
    }
    case "gesture": {
      const g = action.gesture;
      const next: SurfaceState = { ...state, lastGesture: describeGesture(g) };
      // Phase 2 wires only the unambiguous stock behaviours; everything else is
      // logged for the Mapping Lab rather than guessed at.
      if (g.type === "tap" && g.control === "play" && g.count === 1) {
        next.playing = !state.playing;
      }
      if (g.type === "tap" && g.control.startsWith("track-button")) {
        const i = (Number(g.control.slice(-1)) - 1) as TrackIndex;
        next.activeTrack = i;
        if (state.functionHeld) {
          const tracks = [...state.tracks] as SurfaceState["tracks"];
          tracks[i] = {
            ...tracks[i],
            content: tracks[i].content === "muted" ? "loaded" : "muted",
          };
          next.tracks = tracks;
        }
      }
      if (g.type === "tap" && g.control === "volume-plus") {
        next.masterVolume = clamp01(state.masterVolume + 0.0625);
      }
      if (g.type === "tap" && g.control === "volume-minus") {
        next.masterVolume = clamp01(state.masterVolume - 0.0625);
      }
      return next;
    }
    case "faderCommit": {
      const tracks = [...state.tracks] as SurfaceState["tracks"];
      const slice = tracks[action.index];
      if (!slice) return state;
      tracks[action.index] = { ...slice, volume: action.value };
      return { ...state, tracks };
    }
  }
}

const LOG_LIMIT = 60;

export function useDeviceSurface() {
  const [state, dispatch] = useReducer(reducer, undefined, initialSurfaceState);
  const svgRef = useRef<SVGSVGElement | null>(null);
  const capRefs = useRef<Record<number, SVGCircleElement | null>>({});
  const faderValuesRef = useRef<number[]>([0.78, 0.72, 0.65, 0.7]);
  const dragRef = useRef<{ index: number; pointerId: number } | null>(null);
  const frameRef = useRef<number | null>(null);
  const pendingCyRef = useRef<{ index: number; cy: number } | null>(null);

  const [rawLog, setRawLog] = useState<RawInputEvent[]>([]);
  const [gestureLog, setGestureLog] = useState<{ id: number; text: string; t: number }[]>([]);
  const gestureId = useRef(0);

  const engine = useMemo(() => new GestureEngine(), []);

  useEffect(() => {
    const offRaw = engine.onRaw((e) => {
      setRawLog((prev) => [e, ...prev].slice(0, LOG_LIMIT));
      if (e.phase === "down") dispatch({ type: "press", control: e.control });
      else dispatch({ type: "release", control: e.control });
    });
    const offGesture = engine.onGesture((g) => {
      dispatch({ type: "gesture", gesture: g });
      setGestureLog((prev) =>
        [{ id: ++gestureId.current, text: describeGesture(g), t: g.t }, ...prev].slice(0, LOG_LIMIT),
      );
    });
    return () => {
      offRaw();
      offGesture();
    };
  }, [engine]);

  useEffect(() => () => engine.dispose(), [engine]);

  // Never leave a control stuck down when the tab loses input.
  useEffect(() => {
    const bail = () => engine.releaseAll();
    window.addEventListener("blur", bail);
    document.addEventListener("visibilitychange", bail);
    return () => {
      window.removeEventListener("blur", bail);
      document.removeEventListener("visibilitychange", bail);
    };
  }, [engine]);

  // Keyboard parity for every button control.
  useEffect(() => {
    const down = (e: KeyboardEvent) => {
      const control = KEY_MAP[e.code];
      if (!control || e.repeat || e.metaKey || e.ctrlKey || e.altKey) return;
      const target = e.target as HTMLElement | null;
      if (target && /^(INPUT|TEXTAREA|SELECT)$/.test(target.tagName)) return;
      e.preventDefault();
      engine.press(control, "keyboard");
    };
    const up = (e: KeyboardEvent) => {
      const control = KEY_MAP[e.code];
      if (!control) return;
      engine.release(control, "keyboard");
    };
    window.addEventListener("keydown", down);
    window.addEventListener("keyup", up);
    return () => {
      window.removeEventListener("keydown", down);
      window.removeEventListener("keyup", up);
    };
  }, [engine]);

  /** Screen -> viewBox user units via the inverse CTM (no manual scale math). */
  const toUserSpace = useCallback((clientX: number, clientY: number) => {
    const svg = svgRef.current;
    if (!svg) return null;
    const ctm = svg.getScreenCTM();
    if (!ctm) return null;
    const pt = new DOMPoint(clientX, clientY).matrixTransform(ctm.inverse());
    return { x: pt.x, y: pt.y };
  }, []);

  /** Fader caps are written straight to the DOM in a rAF — React never re-renders on drag. */
  const scheduleCap = useCallback((index: number, cy: number) => {
    pendingCyRef.current = { index, cy };
    if (frameRef.current != null) return;
    frameRef.current = requestAnimationFrame(() => {
      frameRef.current = null;
      const pending = pendingCyRef.current;
      if (!pending) return;
      const cap = capRefs.current[pending.index];
      if (cap) cap.setAttribute("cy", String(pending.cy));
    });
  }, []);

  const onControlPointerDown = useCallback(
    (control: Control, e: React.PointerEvent) => {
      e.preventDefault();
      (e.currentTarget as Element).setPointerCapture(e.pointerId);
      const p = toUserSpace(e.clientX, e.clientY);
      engine.press(control, e.pointerId, performance.now(), p?.x, p?.y);
      if (control.startsWith("fader-")) {
        const index = Number(control.slice(-1)) - 1;
        dragRef.current = { index, pointerId: e.pointerId };
        if (p) {
          const value = cyToFaderValue(p.y);
          faderValuesRef.current[index] = value;
          scheduleCap(index, faderValueToCy(value));
        }
      }
    },
    [engine, scheduleCap, toUserSpace],
  );

  const onControlPointerMove = useCallback(
    (control: Control, e: React.PointerEvent) => {
      const drag = dragRef.current;
      if (!drag || drag.pointerId !== e.pointerId) return;
      const p = toUserSpace(e.clientX, e.clientY);
      if (!p) return;
      engine.markMoved(control);
      const value = cyToFaderValue(p.y);
      faderValuesRef.current[drag.index] = value;
      scheduleCap(drag.index, faderValueToCy(value));
    },
    [engine, scheduleCap, toUserSpace],
  );

  const endDrag = useCallback(() => {
    const drag = dragRef.current;
    if (!drag) return;
    dragRef.current = null;
    dispatch({
      type: "faderCommit",
      index: drag.index,
      value: faderValuesRef.current[drag.index] ?? 0,
    });
  }, []);

  const onControlPointerUp = useCallback(
    (control: Control, e: React.PointerEvent) => {
      engine.release(control, e.pointerId);
      endDrag();
    },
    [endDrag, engine],
  );

  const onControlPointerCancel = useCallback(
    (control: Control, e: React.PointerEvent) => {
      engine.cancel(control, e.pointerId);
      endDrag();
    },
    [endDrag, engine],
  );

  const leds = useMemo(() => deriveLeds(state), [state]);

  return {
    state,
    leds,
    engine,
    svgRef,
    capRefs,
    faderValuesRef,
    rawLog,
    gestureLog,
    handlers: {
      onControlPointerDown,
      onControlPointerMove,
      onControlPointerUp,
      onControlPointerCancel,
    },
  };
}
