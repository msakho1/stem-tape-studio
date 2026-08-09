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
  applyGlobalScrub,
  applyPerfIntent,
  deriveLeds,
  initialSurfaceState,
  observedRows,

  pressControl,
  releaseControl,
  type SurfaceState,
} from "@/machine/surface";
import { controlBus, type ContinuousChannel } from "@/audio/controlBus";
import { getAudioEngine } from "@/audio/engine";
import { FaderSessionManager, type FaderIndex } from "@/input/faderSessions";




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

/**
 * Workstream 1, desktop parity: approved keyboard pairs. Up/down per fader,
 * held simultaneously so a desktop user can move four faders at once.
 */
const FADER_KEYS: Record<string, { index: FaderIndex; dir: 1 | -1 }> = {
  KeyY: { index: 0, dir: 1 },
  KeyH: { index: 0, dir: -1 },
  KeyU: { index: 1, dir: 1 },
  KeyJ: { index: 1, dir: -1 },
  KeyI: { index: 2, dir: 1 },
  KeyK: { index: 2, dir: -1 },
  KeyO: { index: 3, dir: 1 },
  KeyL: { index: 3, dir: -1 },
};

/** Value units per second while a fader key is held. */
const KEY_FADER_RATE = 0.9;

export const KEY_HINTS: [string, Control][] = Object.entries(KEY_MAP).map(([k, c]) => [
  k.replace("Key", "").replace("Digit", "").replace("Space", "space").replace("Minus", "−").replace("Equal", "+"),
  c,
]);

type Action =
  | { type: "press"; control: Control }
  | { type: "release"; control: Control }
  | { type: "gesture"; gesture: Gesture }
  | { type: "perf"; intent: PerfIntent }
  | { type: "globalScrub"; dir: 1 | -1 | null }
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
    case "globalScrub":
      return applyGlobalScrub(state, action.dir, performance.now());
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
  /**
   * Workstream 1: one session per pointer. No singleton drag — a second finger
   * can never steal the first fader, and each pointerup ends only its own.
   */
  const faders = useRef(new FaderSessionManager());
  const frameRef = useRef<number | null>(null);
  /** Faders joined to the shift-click group; they move together as one gesture. */
  const groupRef = useRef<Set<FaderIndex>>(new Set());
  /** Keys currently held, with the frame time each was last integrated at. */
  const keyFadersRef = useRef<Map<string, { index: FaderIndex; dir: 1 | -1; last: number }>>(new Map());
  const keyFrameRef = useRef<number | null>(null);

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

  /** Keys currently down — drives the desktop Keyboard Controls panel lamps. */
  const [heldKeys, setHeldKeys] = useState<string[]>([]);
  const heldKeysRef = useRef<Set<string>>(new Set());
  const scrubKeysRef = useRef<Set<string>>(new Set());
  /** Touch parity: pointers currently holding a rocker zone as the shuttle. */
  const scrubPointersRef = useRef<Map<number, 1 | -1>>(new Map());
  /** Pointer id currently holding the on-screen FUNCTION button, if any. */
  const fnPointerRef = useRef<number | null>(null);
  /** FUNCTION acted as the shuttle modifier: its release must NOT fire a tap. */
  const scrubUsedFnRef = useRef(false);

  const syncHeld = useCallback(() => setHeldKeys([...heldKeysRef.current]), []);

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
    const bail = () => {
      engine.releaseAll();
      heldKeysRef.current.clear();
      scrubKeysRef.current.clear();
      scrubPointersRef.current.clear();
      fnPointerRef.current = null;
      scrubUsedFnRef.current = false;

      syncHeld();
      dispatch({ type: "globalScrub", dir: null });
    };
    window.addEventListener("blur", bail);
    document.addEventListener("visibilitychange", bail);
    return () => {
      window.removeEventListener("blur", bail);
      document.removeEventListener("visibilitychange", bail);
    };
  }, [engine, syncHeld]);

  // Keyboard parity for every button control.
  useEffect(() => {
    const endScrub = () => {
      if (scrubKeysRef.current.size === 0) return;
      scrubKeysRef.current.clear();
      dispatch({ type: "globalScrub", dir: null });
    };
    const down = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement | null;
      if (target && /^(INPUT|TEXTAREA|SELECT)$/.test(target.tagName)) return;
      if (e.code === "Escape") {
        // Safety release: nothing stays latched, the shuttle stops.
        endScrub();
        engine.releaseAll();
        heldKeysRef.current.clear();
        syncHeld();
        return;
      }
      const control = KEY_MAP[e.code];
      if (!readyRef.current || !control || e.repeat || e.metaKey || e.ctrlKey || e.altKey) return;
      e.preventDefault();
      heldKeysRef.current.add(e.code);
      syncHeld();
      // FUNCTION + rocker held = global four-stem shuttle. The rocker is NEVER
      // pressed into the gesture engine here, so no varispeed / step-scrub row
      // can fire underneath the shuttle.
      if ((e.code === "KeyQ" || e.code === "KeyA") && heldKeysRef.current.has("KeyF")) {
        scrubKeysRef.current.add(e.code);
        if (!scrubUsedFnRef.current) {
          scrubUsedFnRef.current = true;
          // FUNCTION is consumed by the shuttle the moment it starts: cancel it
          // so neither its hold (power) nor its release (tap) row can fire.
          engine.cancel("function", "keyboard");
        }
        dispatch({ type: "globalScrub", dir: e.code === "KeyQ" ? 1 : -1 });
        return;
      }
      engine.press(control, "keyboard");
    };
    const up = (e: KeyboardEvent) => {
      const control = KEY_MAP[e.code];
      if (!control) return;
      heldKeysRef.current.delete(e.code);
      syncHeld();
      if (scrubKeysRef.current.delete(e.code)) {
        const remaining = [...scrubKeysRef.current][0];
        if (remaining) dispatch({ type: "globalScrub", dir: remaining === "KeyQ" ? 1 : -1 });
        else dispatch({ type: "globalScrub", dir: null });
        return;
      }
      if (e.code === "KeyF") {
        endScrub();
        if (scrubUsedFnRef.current) {
          scrubUsedFnRef.current = false;
          return; // already cancelled at shuttle start — nothing to release
        }
      }
      engine.release(control, "keyboard");
    };
    window.addEventListener("keydown", down);
    window.addEventListener("keyup", up);
    return () => {
      window.removeEventListener("keydown", down);
      window.removeEventListener("keyup", up);
    };
  }, [engine, syncHeld]);

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

  /**
   * ONE shared frame for every moving fader. Each pending fader is written to
   * the DOM and pushed onto the control bus inside the same rAF, carrying the
   * same batch id, so four fingers land on one future audio frame instead of
   * four staggered ones.
   */
  const flushFrame = useCallback(() => {
    frameRef.current = null;
    const batch = faders.current.flush();
    if (!batch) return;
    const timestamp = performance.now();
    for (const p of batch.previews) {
      const cap = capRefs.current[p.faderIndex];
      if (cap) cap.setAttribute("cy", String(faderValueToCy(p.value)));
      controlBus.send({
        channel: p.channel,
        index: p.faderIndex,
        value: p.value,
        committed: false,
        pointerId: p.pointerId,
        phase: "move",
        timestamp,
        batchFrame: batch.batchFrame,
      });
    }
  }, []);

  const scheduleFlush = useCallback(() => {
    if (frameRef.current != null) return;
    frameRef.current = requestAnimationFrame(flushFrame);
  }, [flushFrame]);

  /**
   * Keyboard fader pairs (Y/H, U/J, I/K, O/L). Any number of keys may be held
   * at once: every held key is integrated on the SAME rAF and flushed in one
   * batch, so keyboard performance is genuinely simultaneous, not sequential.
   */
  const keyFrame = useCallback(() => {
    keyFrameRef.current = null;
    const held = keyFadersRef.current;
    if (held.size === 0) return;
    const now = performance.now();
    const channel = resolveChannel();
    for (const entry of held.values()) {
      const dt = Math.min(0.1, Math.max(0, (now - entry.last) / 1000));
      entry.last = now;
      const current = faderValuesRef.current[entry.index] ?? 0;
      const next = Math.min(1, Math.max(0, current + entry.dir * KEY_FADER_RATE * dt));
      faderValuesRef.current[entry.index] = next;
      faders.current.queue({ faderIndex: entry.index, value: next, channel, pointerId: -1 - entry.index });
    }
    scheduleFlush();
    keyFrameRef.current = requestAnimationFrame(keyFrame);
  }, [resolveChannel, scheduleFlush]);

  useEffect(() => {
    const down = (e: KeyboardEvent) => {
      const spec = FADER_KEYS[e.code];
      if (!readyRef.current || !spec || e.metaKey || e.ctrlKey || e.altKey) return;
      const target = e.target as HTMLElement | null;
      if (target && /^(INPUT|TEXTAREA|SELECT)$/.test(target.tagName)) return;
      e.preventDefault();
      if (e.repeat) return;
      // A pointer already owns this fader: the pointer wins, keys are ignored.
      if (faders.current.owner(spec.index) != null) return;
      keyFadersRef.current.set(e.code, { ...spec, last: performance.now() });
      heldKeysRef.current.add(e.code);
      syncHeld();
      if (keyFrameRef.current == null) keyFrameRef.current = requestAnimationFrame(keyFrame);
    };
    const up = (e: KeyboardEvent) => {
      const spec = FADER_KEYS[e.code];
      if (!spec) return;
      heldKeysRef.current.delete(e.code);
      syncHeld();
      if (!keyFadersRef.current.delete(e.code)) return;
      const value = faderValuesRef.current[spec.index] ?? 0;
      const channel = resolveChannel();
      controlBus.send({
        channel,
        index: spec.index,
        value,
        committed: true,
        pointerId: -1 - spec.index,
        phase: "end",
        timestamp: performance.now(),
      });
      dispatch({ type: "faderCommit", index: spec.index, value, claimed: channel });
      if (keyFadersRef.current.size === 0 && keyFrameRef.current != null) {
        cancelAnimationFrame(keyFrameRef.current);
        keyFrameRef.current = null;
      }
    };
    window.addEventListener("keydown", down);
    window.addEventListener("keyup", up);
    return () => {
      window.removeEventListener("keydown", down);
      window.removeEventListener("keyup", up);
      if (keyFrameRef.current != null) cancelAnimationFrame(keyFrameRef.current);
      keyFrameRef.current = null;
      keyFadersRef.current.clear();
    };
  }, [keyFrame, resolveChannel, syncHeld]);

  /**
   * True rebase: FUNCTION or HEADS changing while faders are still down
   * retargets every live session to the new channel without a value jump, and
   * the old channel stops receiving previews immediately.
   */
  useEffect(() => {
    const channel = layerRef.current.heads
      ? layerRef.current.fn
        ? "headScrub"
        : "headLevel"
      : layerRef.current.fn
        ? "window"
        : "fader";
    for (const session of faders.current.sessions()) {
      if (session.channel === channel) continue;
      const destination = faderValuesRef.current[session.faderIndex] ?? 0;
      const pointerValue = session.lastPreviewValue + session.grabOffset;
      faders.current.rebase(session.pointerId, channel as ContinuousChannel, pointerValue, destination);
      controlBus.send({
        channel: channel as ContinuousChannel,
        index: session.faderIndex,
        value: destination,
        committed: false,
        pointerId: session.pointerId,
        phase: "start",
        timestamp: performance.now(),
      });
    }
  }, [state.functionHeld, state.headsMode]);

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

      // Touch parity for the keyboard F+Q/A shuttle: FUNCTION held (on-screen)
      // + a rocker zone pressed = held four-stem shuttle. The rocker is never
      // pressed into the gesture engine, so no varispeed / step-scrub row can
      // fire underneath it, and FUNCTION is consumed exactly as on keyboard.
      if (
        (control === "rocker-fwd" || control === "rocker-rwd") &&
        (fnPointerRef.current != null || stateRef.current.functionHeld)
      ) {
        const dir = control === "rocker-fwd" ? 1 : -1;
        // Mobile Safari only permits AudioContext creation/resume in the direct
        // pointer event call stack. Starting the unlock here (before React's
        // command effect runs) preserves that user activation; the ordered
        // command stream remains the sole owner of the actual shuttle action.
        void getAudioEngine().unlock();
        scrubPointersRef.current.set(e.pointerId, dir);
        if (!scrubUsedFnRef.current) {
          scrubUsedFnRef.current = true;
          // FUNCTION is consumed by the shuttle: cancel it so neither its hold
          // (power) nor its release (tap) row can fire underneath.
          engine.cancel("function", fnPointerRef.current ?? "keyboard");
        }
        dispatch({ type: "globalScrub", dir });
        return;
      }
      if (control === "function") fnPointerRef.current = e.pointerId;

      engine.press(control, e.pointerId, performance.now(), p?.x, p?.y);



      if (control.startsWith("fader-")) {
        const index = (Number(control.slice(-1)) - 1) as FaderIndex;
        const channel = resolveChannel();
        const current = faderValuesRef.current[index] ?? 0;
        const pointerValue = p ? cyToFaderValue(p.y) : current;
        // Pickup semantics: the cap does NOT jump to the finger. A second
        // finger on an already-owned fader is rejected without disturbing it.
        const session = faders.current.begin({
          pointerId: e.pointerId,
          faderIndex: index,
          userY: p?.y ?? 0,
          pointerValue,
          currentValue: current,
          channel,
          source: e.pointerType === "mouse" ? "mouse" : "touch",
          t: performance.now(),
        });
        if (!session) return;
        // Desktop grouping: shift+click joins this fader to the group, so one
        // mouse drives several faders with the same relative delta.
        if (e.shiftKey) groupRef.current.add(index);
        else if (!groupRef.current.has(index)) groupRef.current.clear();
        // The gesture opens on the bus BEFORE any movement, so the engine can
        // latch the head's current read position as the scrub origin.
        controlBus.send({
          channel,
          index,
          value: current,
          committed: false,
          pointerId: e.pointerId,
          phase: "start",
          timestamp: performance.now(),
        });
      }
    },
    [engine, resolveChannel, toUserSpace],
  );

  const onControlPointerMove = useCallback(
    (control: Control, e: React.PointerEvent) => {
      const session = faders.current.sessionForPointer(e.pointerId);
      if (!session) return;
      const p = toUserSpace(e.clientX, e.clientY);
      if (!p) return;
      engine.markMoved(control);
      const value = faders.current.move(e.pointerId, p.y, cyToFaderValue(p.y), (v) => Math.min(1, Math.max(0, v)));
      if (value == null) return;
      const previous = faderValuesRef.current[session.faderIndex] ?? value;
      faderValuesRef.current[session.faderIndex] = value;
      if (groupRef.current.size > 1 && groupRef.current.has(session.faderIndex)) {
        const delta = value - previous;
        for (const other of groupRef.current) {
          if (other === session.faderIndex) continue;
          if (faders.current.owner(other) != null) continue; // owned by its own pointer
          const next = Math.min(1, Math.max(0, (faderValuesRef.current[other] ?? 0) + delta));
          faderValuesRef.current[other] = next;
          faders.current.queue({
            faderIndex: other,
            value: next,
            channel: session.channel,
            pointerId: session.pointerId,
          });
        }
      }
      scheduleFlush();
    },
    [engine, scheduleFlush, toUserSpace],
  );

  /**
   * Commit: the reducer receives the EXACT value that was last audible, so the
   * committed gain and the last preview gain are the same number. Only the
   * session belonging to THIS pointer ends.
   */
  const endDrag = useCallback((pointerId: number) => {
    const drag = faders.current.end(pointerId);
    if (!drag) return;
    const value = drag.lastPreviewValue;
    faderValuesRef.current[drag.faderIndex] = value;
    controlBus.send({
      channel: drag.channel,
      index: drag.faderIndex,
      value,
      committed: true,
      pointerId: drag.pointerId,
      phase: "end",
      timestamp: performance.now(),
    });
    // The reducer is told which layer the gesture claimed, so a modifier
    // released mid-drag cannot retarget the commit.
    dispatch({ type: "faderCommit", index: drag.faderIndex, value, claimed: drag.channel });
    if (groupRef.current.size > 1 && groupRef.current.has(drag.faderIndex)) {
      for (const other of groupRef.current) {
        if (other === drag.faderIndex) continue;
        const v = faderValuesRef.current[other] ?? 0;
        controlBus.send({
          channel: drag.channel,
          index: other,
          value: v,
          committed: true,
          pointerId: drag.pointerId,
          phase: "end",
          timestamp: performance.now(),
        });
        dispatch({ type: "faderCommit", index: other, value: v, claimed: drag.channel });
      }
    }
  }, []);

  /**
   * Documented cancel rule: a cancelled drag is NOT a commit. Audio is
   * reconciled back to the last committed value (ramped) and the reducer keeps
   * the value it already had. Other live faders are untouched.
   */
  const cancelDrag = useCallback((pointerId: number) => {
    const drag = faders.current.cancel(pointerId);
    if (!drag) return;
    if (drag.channel === "headScrub") {
      // Cancelling a scrub restores the pre-gesture head position; there is no
      // "last committed fader value" to ramp to.
      controlBus.send({
        channel: "headScrub",
        index: drag.faderIndex,
        value: faderValuesRef.current[drag.faderIndex] ?? 0,
        committed: false,
        pointerId: drag.pointerId,
        phase: "cancel",
        timestamp: performance.now(),
      });
      return;
    }
    const committed = controlBus.reconcile(
      drag.channel,
      drag.faderIndex,
      stateRef.current.tracks[drag.faderIndex]?.volume ?? 0,
    );
    faderValuesRef.current[drag.faderIndex] = committed;
    const cap = capRefs.current[drag.faderIndex];
    if (cap) cap.setAttribute("cy", String(faderValueToCy(committed)));
  }, []);


  /**
   * Releasing either half of the touch shuttle: if the other rocker zone is
   * still held the shuttle continues in that direction, otherwise it ends.
   * Releasing FUNCTION ends the shuttle outright, exactly like the keyboard.
   */
  const endTouchScrub = useCallback((pointerId: number) => {
    if (!scrubPointersRef.current.delete(pointerId)) return false;
    const remaining = [...scrubPointersRef.current.values()][0];
    dispatch({ type: "globalScrub", dir: remaining ?? null });
    return true;
  }, []);

  const releaseControlPointer = useCallback(
    (control: Control, e: React.PointerEvent, cancelled: boolean) => {
      if (endTouchScrub(e.pointerId)) return;
      if (control === "function") {
        fnPointerRef.current = null;
        if (scrubPointersRef.current.size) {
          scrubPointersRef.current.clear();
          dispatch({ type: "globalScrub", dir: null });
        }
        if (scrubUsedFnRef.current) {
          // Already cancelled at shuttle start — nothing left to release.
          scrubUsedFnRef.current = false;
          cancelDrag(e.pointerId);
          return;
        }
      }
      if (cancelled) {
        engine.cancel(control, e.pointerId);
        cancelDrag(e.pointerId);
      } else {
        engine.release(control, e.pointerId);
        endDrag(e.pointerId);
      }
    },
    [cancelDrag, endDrag, endTouchScrub, engine],
  );

  const onControlPointerUp = useCallback(
    (control: Control, e: React.PointerEvent) => releaseControlPointer(control, e, false),
    [releaseControlPointer],
  );

  const onControlPointerCancel = useCallback(
    (control: Control, e: React.PointerEvent) => releaseControlPointer(control, e, true),
    [releaseControlPointer],
  );



  // Read-only verification fixture: the ordered command stream and the last
  // gestures, so a browser harness can prove WHICH row fired (and which did not).
  useEffect(() => {
    const w = window as unknown as { __stemTapeSurface?: () => unknown };
    w.__stemTapeSurface = () => ({
      commands: stateRef.current.commands.slice(-20).map((c) => ({ id: c.id, type: c.type, payload: c.payload })),
      lastGesture: stateRef.current.lastGesture,
      globalScrub: stateRef.current.globalScrub,
      playing: stateRef.current.playing,
      power: stateRef.current.power,
      fxOverlay: stateRef.current.perf.fxOverlay,
      headsMode: stateRef.current.headsMode,
    });
    return () => {
      delete w.__stemTapeSurface;
    };
  }, []);

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
    heldKeys,
    globalScrub: state.globalScrub,

    handlers: {
      onControlPointerDown,
      onControlPointerMove,
      onControlPointerUp,
      onControlPointerCancel,
    },
  };
}
