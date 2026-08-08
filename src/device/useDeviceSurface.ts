import { useCallback, useEffect, useMemo, useReducer, useRef, useState } from "react";
import {
  cyToFaderValue,
  faderValueToCy,
  type Control,
} from "@/device/geometry";
import {
  DEFAULT_TIMINGS,
  GestureEngine,
  describeGesture,
  type Gesture,
  type RawInputEvent,
} from "@/input/gestures";
import { ChordArbiter, type PerfIntent } from "@/machine/chordArbiter";
import {
  applyFader,
  applyGesture,
  applyPerfIntent,
  deriveLeds,
  initialSurfaceState,
  observedRows,

  pressControl,
  releaseControl,
  type SurfaceState,
} from "@/machine/surface";
import { controlBus, type ContinuousChannel } from "@/audio/controlBus";



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
  | { type: "perf"; intent: PerfIntent }
  | { type: "faderCommit"; index: number; value: number; claimed?: ContinuousChannel };


function reducer(state: SurfaceState, action: Action): SurfaceState {
  switch (action.type) {
    case "press":
      return pressControl(state, action.control);
    case "release":
      return releaseControl(state, action.control);

    case "perf":
      // Already arbitrated: exactly one semantic command, nothing to roll back.
      return applyPerfIntent(state, action.intent);
    case "gesture":
      // Every behaviour is a documented Tape Looper v2.6 row. No experimental
      // Stem Tape mappings are dispatched here (phase 4).
      return applyGesture({ ...state, lastGesture: describeGesture(action.gesture) }, action.gesture);
    case "faderCommit":
      return applyFader(state, action.index, action.value, action.claimed);
  }
}


const LOG_LIMIT = 60;

export function useDeviceSurface() {
  const [state, dispatch] = useReducer(reducer, undefined, initialSurfaceState);
  const svgRef = useRef<SVGSVGElement | null>(null);
  const capRefs = useRef<Record<number, SVGCircleElement | null>>({});
  const faderValuesRef = useRef<number[]>([0.78, 0.72, 0.65, 0.7]);
  const dragRef = useRef<{ index: number; pointerId: number; channel: ContinuousChannel } | null>(null);
  const frameRef = useRef<number | null>(null);
  const pendingCyRef = useRef<{ index: number; cy: number; value: number } | null>(null);
  /** Latest state, read by imperative pointer handlers without re-binding them. */
  const stateRef = useRef<SurfaceState>(state);
  stateRef.current = state;
  /** Which fader layer is live (FN = chop window, HEADS = scrub, else volume). */
  const layerRef = useRef<{ fn: boolean; heads: boolean }>({ fn: false, heads: false });
  layerRef.current = { fn: state.functionHeld, heads: state.headsMode };

  /**
   * Application-ready gate. Nothing reaches the gesture engine until the client
   * has mounted and the surface SVG is in the DOM. Pre-hydration presses used to
   * be silently swallowed, which made harness runs report false failures.
   * `document.documentElement[data-app-ready]` and `window.__stemTapeReady` are
   * the public signals a browser harness must wait on.
   */
  const [ready, setReady] = useState(false);
  const readyRef = useRef(false);

  const [rawLog, setRawLog] = useState<RawInputEvent[]>([]);
  const [gestureLog, setGestureLog] = useState<{ id: number; text: string; t: number }[]>([]);
  const gestureId = useRef(0);


  const engine = useMemo(() => new GestureEngine(), []);
  /**
   * Ordered chord arbitration sits BETWEEN raw input and the v2.6 dispatch:
   * a control claimed by a chord never reaches `applyGesture`, so no base
   * Play / Volume / Track command is emitted and then undone.
   */
  const arbiter = useMemo(
    () =>
      new ChordArbiter(() => {
        const perf = stateRef.current.perf;
        return {
          activeStem: perf.activeStem,
          fxOverlay: perf.fxOverlay,
          selectedBank: perf.tracks[perf.activeStem]!.fx12.selectedBank,
        };
      }),
    [],
  );

  const [powerHoldMs, setPowerHoldMsState] = useState(DEFAULT_TIMINGS.powerHoldMs);

  const setPowerHoldMs = useCallback(
    (ms: number) => {
      const clamped = Math.max(200, Math.min(6000, Math.round(ms)));
      engine.timings = { ...engine.timings, powerHoldMs: clamped };
      setPowerHoldMsState(clamped);
    },
    [engine],
  );

  // Application-ready gate: arm input only once mounted and the surface exists.
  useEffect(() => {
    const id = requestAnimationFrame(() => {
      if (!svgRef.current) return;
      readyRef.current = true;
      setReady(true);
      document.documentElement.dataset["appReady"] = "true";
      (window as unknown as { __stemTapeReady?: boolean }).__stemTapeReady = true;
    });
    return () => {
      cancelAnimationFrame(id);
      readyRef.current = false;
      delete document.documentElement.dataset["appReady"];
      (window as unknown as { __stemTapeReady?: boolean }).__stemTapeReady = false;
    };
  }, []);



  useEffect(() => {
    const offIntent = arbiter.onIntent((intent) => {
      dispatch({ type: "perf", intent });
      setGestureLog((prev) =>
        [{ id: ++gestureId.current, text: `arbitrated → ${intent.type}`, t: performance.now() }, ...prev].slice(0, LOG_LIMIT),
      );
    });
    const offRaw = engine.onRaw((e) => {
      // Arbitration first, always, and in raw order.
      arbiter.handle(e);
      setRawLog((prev) => [e, ...prev].slice(0, LOG_LIMIT));
      if (e.phase === "down") dispatch({ type: "press", control: e.control });
      else dispatch({ type: "release", control: e.control });
    });
    const offGesture = engine.onGesture((g) => {
      const control = "control" in g ? g.control : null;
      if (control && arbiter.isClaimed(control)) return; // suppressed before dispatch
      if (g.type === "chordStart" || g.type === "chordRelease") {
        if (g.controls.some((c) => arbiter.isClaimed(c))) return;
      }
      dispatch({ type: "gesture", gesture: g });
      setGestureLog((prev) =>
        [{ id: ++gestureId.current, text: describeGesture(g), t: g.t }, ...prev].slice(0, LOG_LIMIT),
      );
    });
    return () => {
      offRaw();
      offGesture();
      offIntent();
    };
  }, [engine, arbiter]);

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
      if (!readyRef.current || !control || e.repeat || e.metaKey || e.ctrlKey || e.altKey) return;

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

  /**
   * Fader caps are written straight to the DOM in a rAF — React never re-renders
   * on drag — and the SAME rAF pushes one coalesced preview onto the continuous
   * control bus, so the audio moves with the finger instead of waiting for
   * pointer-up. The SVG still never touches an AudioNode.
   */
  /**
   * The fader layer is resolved ONCE, at pointer-down, and owned by that
   * gesture until it ends. HEADS+FUNCTION claims the fader as a scrub gesture,
   * so a modifier released mid-drag cannot turn a scrub into a volume change.
   */
  const resolveChannel = useCallback((): ContinuousChannel => {
    const layer = layerRef.current;
    if (layer.heads) return layer.fn ? "headScrub" : "headLevel";
    return layer.fn ? "window" : "fader";
  }, []);

  const scheduleCap = useCallback((index: number, cy: number, value: number) => {
    pendingCyRef.current = { index, cy, value };
    if (frameRef.current != null) return;
    frameRef.current = requestAnimationFrame(() => {
      frameRef.current = null;
      const pending = pendingCyRef.current;
      if (!pending) return;
      const cap = capRefs.current[pending.index];
      if (cap) cap.setAttribute("cy", String(pending.cy));
      const drag = dragRef.current;
      controlBus.send({
        channel: drag?.channel ?? "fader",
        index: pending.index,
        value: pending.value,
        committed: false,
        pointerId: drag?.pointerId ?? -1,
        phase: "move",
        timestamp: performance.now(),
      });
    });
  }, []);

  const onControlPointerDown = useCallback(
    (control: Control, e: React.PointerEvent) => {
      e.preventDefault();
      if (!readyRef.current) return;
      // Capture is an optimisation, never a gate: if the UA refuses it the
      // gesture must still reach the engine.
      try {
        (e.currentTarget as Element).setPointerCapture(e.pointerId);
      } catch {
        /* capture unavailable for this pointer */
      }
      const p = toUserSpace(e.clientX, e.clientY);
      engine.press(control, e.pointerId, performance.now(), p?.x, p?.y);

      if (control.startsWith("fader-")) {
        const index = Number(control.slice(-1)) - 1;
        const channel = resolveChannel();
        dragRef.current = { index, pointerId: e.pointerId, channel };
        const value = p ? cyToFaderValue(p.y) : (faderValuesRef.current[index] ?? 0);
        // The gesture opens on the bus BEFORE any movement, so the engine can
        // latch the head's current read position as the scrub origin.
        controlBus.send({
          channel,
          index,
          value,
          committed: false,
          pointerId: e.pointerId,
          phase: "start",
          timestamp: performance.now(),
        });
        if (p) {
          faderValuesRef.current[index] = value;
          scheduleCap(index, faderValueToCy(value), value);
        }
      }
    },
    [engine, resolveChannel, scheduleCap, toUserSpace],
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
      scheduleCap(drag.index, faderValueToCy(value), value);
    },
    [engine, scheduleCap, toUserSpace],
  );

  /**
   * Commit: the reducer receives the EXACT value that was last audible, so the
   * committed gain and the last preview gain are the same number.
   */
  const endDrag = useCallback(() => {
    const drag = dragRef.current;
    if (!drag) return;
    dragRef.current = null;
    const value = faderValuesRef.current[drag.index] ?? 0;
    controlBus.send({
      channel: drag.channel,
      index: drag.index,
      value,
      committed: true,
      pointerId: drag.pointerId,
      phase: "end",
      timestamp: performance.now(),
    });
    // The reducer is told which layer the gesture claimed, so a modifier
    // released mid-drag cannot retarget the commit.
    dispatch({ type: "faderCommit", index: drag.index, value, claimed: drag.channel });
  }, []);

  /**
   * Documented cancel rule: a cancelled drag is NOT a commit. Audio is
   * reconciled back to the last committed value (ramped) and the reducer keeps
   * the value it already had.
   */
  const cancelDrag = useCallback(() => {
    const drag = dragRef.current;
    if (!drag) return;
    dragRef.current = null;
    if (drag.channel === "headScrub") {
      // Cancelling a scrub restores the pre-gesture head position; there is no
      // "last committed fader value" to ramp to.
      controlBus.send({
        channel: "headScrub",
        index: drag.index,
        value: faderValuesRef.current[drag.index] ?? 0,
        committed: false,
        pointerId: drag.pointerId,
        phase: "cancel",
        timestamp: performance.now(),
      });
      return;
    }
    const committed = controlBus.reconcile(drag.channel, drag.index, stateRef.current.tracks[drag.index]?.volume ?? 0);
    faderValuesRef.current[drag.index] = committed;
    const cap = capRefs.current[drag.index];
    if (cap) cap.setAttribute("cy", String(faderValueToCy(committed)));
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
      cancelDrag();
    },
    [cancelDrag, engine],
  );

  const leds = useMemo(() => deriveLeds(state), [state]);
  const observed = useMemo(() => observedRows(state, leds), [state, leds]);

  return {
    state,
    leds,
    observed,
    engine,
    arbiter,
    ready,
    powerHoldMs,
    setPowerHoldMs,
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
