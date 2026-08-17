import { trace } from "@/diagnostics/trace";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { Ack, AudioCommand } from "./commands";
import { controlBus } from "./controlBus";
import { getAudioEngine, type EngineStatus } from "./engine";



/**
 * Drains the reducer's ordered command stream into the AudioEngine by
 * watermark, feeds the continuous control bus straight to AudioParams, keeps
 * lifecycle honest (a suspended context can never read as "playing") and
 * exposes engine truth for diagnostics.
 */
export function useAudioEngine(commands: AudioCommand[]) {
  const engine = useMemo(() => getAudioEngine(), []);
  const watermark = useRef(0);
  /**
   * Audio unlock is asynchronous on mobile. Keep one promise tail so a quick
   * touch release cannot execute scrub.end before scrub.start's unlock retry.
   * This preserves reducer command order across every async audio boundary.
   */
  const commandTail = useRef<Promise<void>>(Promise.resolve());
  const [acks, setAcks] = useState<Ack[]>([]);
  /** >0 while a heads.enter / heads.exit is still queued or executing. */
  const [headsInFlight, setHeadsInFlight] = useState(0);
  const [status, setStatus] = useState<EngineStatus>(() => engine.status());
  const [unlockNote, setUnlockNote] = useState<string>("audio locked — press PLAY or enable audio");

  // --- platform budget resolves only after hydration -----------------------
  // SSR cannot know the device, so the engine starts on SSR_BUDGET and the real
  // tier is adopted here. Resolving during render would mismatch hydration.
  useEffect(() => {
    engine.resolveBudget();
    setStatus(engine.status());
  }, [engine]);

  /**
   * Browser verification fixture. A harness cannot hear the output, so the
   * engine's own scrub truth is published read-only on the window: live
   * trackers, the kernel's telemetry (actual frame, velocity, gain, mix, RMS)
   * and the ordered gesture log. Read-only — it exposes no way to fake a
   * gesture, so what a harness reads is what the audio path actually did.
   */
  useEffect(() => {
    const w = window as unknown as {
      __stemTapeScrub?: () => unknown;
      __stemTapeTransport?: () => unknown;
    };
    w.__stemTapeScrub = () => {
      const d = engine.status();
      return { scrub: d.scrub, heads: d.heads, headsSummary: d.headsSummary, engineReady: engine.ready };
    };
    /**
     * Transport truth for the browser proofs: the three separate values
     * (musical target, instantaneous inertia rate, movement), the phase, the
     * reversal counter and the ordered timeline-event log. Read-only.
     */
    w.__stemTapeTransport = () => {
      const d = engine.status();
      return {
        phase: d.transportPhase,
        musicalRate: d.targetRate,
        instantaneousRate: d.rate,
        moving: d.actuallyPlaying,
        requested: d.requestedPlaying,
        position: d.position,
        effectiveBpm: d.effectiveBpm,
        windReversals: engine.windReversals,
        globalScrub: engine.globalScrubState(),
        stems: d.tracks.map((t) => ({ id: t.id, decoded: t.decoded, sourceLive: t.sourceLive, gain: t.gain })),
        masterRms: engine.masterRms(),
        fx: d.fx.map((f) => (f ? { active: (f as unknown as Record<string, unknown>)["active"] ?? null, summary: JSON.stringify(f).slice(0, 200) } : null)),
        lastFxRejection: d.lastFxRejection,
        timelineEvents: engine.timelineBus.log.slice(-40),
      };
    };
    /**
     * Heads Mode truth for the browser proof: the four independent head
     * pointers, their play/latch/hold/loop state, the heads-bus RMS (head-path
     * output, isolated from the transport) and the ordered heads event log —
     * alongside the frozen main transport position.
     */
    (w as unknown as { __stemTapeHeads?: () => unknown }).__stemTapeHeads = () => {
      const d = engine.status();
      return {
        active: engine.headLanes.active,
        engineHeadsActive: engine.heads.active,
        contextState: d.contextState,
        transportPosition: d.position,
        transportPlaying: d.actuallyPlaying,
        headsRms: engine.headsRms(),
        source: engine.headLanes.source,
        sourceName: engine.headLanes.sourceName,
        heads: engine.headLanes.snapshot(),
        summary: engine.headLanes.summary(),
        log: engine.headLanes.log.slice(-40),
      };
    };
    /**
     * Stem Instrument Mode truth: learned markers, open captures, which lanes
     * a cue currently owns, the per-lane audible voice count, the gate values
     * and the underlay position each finished cue rejoins. Read-only.
     */
    (w as unknown as { __stemTapeCues?: () => unknown }).__stemTapeCues = () => engine.cueSnapshot();
    return () => {
      delete (w as unknown as { __stemTapeCues?: () => unknown }).__stemTapeCues;
      delete w.__stemTapeScrub;
      delete w.__stemTapeTransport;
      delete (w as unknown as { __stemTapeHeads?: () => unknown }).__stemTapeHeads;
    };
  }, [engine]);





  // --- ordered command drain (never a snapshot diff) -----------------------

  useEffect(() => {
    const pending = commands.filter((c) => c.id > watermark.current);
    if (pending.length === 0) return;
    watermark.current = pending[pending.length - 1]!.id;
    for (const cmd of pending) {
      const isHeadsSwitch = cmd.type === "heads.enter" || cmd.type === "heads.exit";
      if (isHeadsSwitch) setHeadsInFlight((n) => n + 1);
      commandTail.current = commandTail.current.then(async () => {
        const needsAudio =
          cmd.type.startsWith("transport.") ||
          cmd.type.startsWith("stem.") ||
          cmd.type.startsWith("tape.") ||
          cmd.type.startsWith("fx.") ||
          cmd.type.startsWith("heads.") ||
          cmd.type.startsWith("cue.");
        if (needsAudio && !engine.ready) {
          const result = await engine.unlock();
          setUnlockNote(result.detail);
        }
        if (trace.enabled) {
          trace.record(
            "command.engine",
            `${cmd.type}`,
            { id: cmd.id, payload: cmd.payload as unknown },
            { commandId: cmd.id },
          );
        }
        let ack = engine.execute(cmd);
        // Heads entry retries EXACTLY once behind a fresh resume: on mobile the
        // first unlock can still be settling when the triple-tap completes, and
        // a locked context must not read as "heads unavailable".
        if (cmd.type === "heads.enter" && ack.status === "rejected" && /unlock|suspend|locked|closed/i.test(ack.detail)) {
          const retry = await engine.unlock();
          setUnlockNote(retry.detail);
          if (retry.ok) ack = engine.execute(cmd);
        }
        if (trace.enabled) {
          trace.record(
            "engine.ack",
            `${ack.type} → ${ack.status}`,
            { id: ack.id, accepted: ack.status !== "rejected", reason: ack.detail, detail: ack.detail },
            { commandId: ack.id, causeId: "engine" },
          );
        }
        setAcks((prev) => [ack, ...prev].slice(0, 60));
        setStatus(engine.status());
        if (isHeadsSwitch) setHeadsInFlight((n) => Math.max(0, n - 1));
      });
    }
  }, [commands, engine]);


  // --- continuous control bus → AudioParam / scrub kernel -----------------
  useEffect(() => {
    const off = controlBus.subscribe((e) => {
      const head = e.index as 0 | 1 | 2 | 3;
      // Leaving the FUNCTION layer mid-gesture must close any live lane scrub
      // instead of leaving a silenced lane behind.
      if (e.channel !== "laneScrub" && engine.laneFaderScrubState(head)) {
        engine.endLaneFaderScrub(head, e.value);
      }
      if (e.channel === "headScrub") {
        // Heads v2: FUNCTION + fader N is an audible positional scrub of HEAD N
        // over lane N. The release parks the landing and stores it as the next
        // loop start for that head.
        if (e.phase === "start") engine.headLanes.beginScrub(head);
        else if (e.phase === "cancel") engine.headLanes.endScrub(head, e.value);
        else if (e.committed || e.phase === "end") engine.headLanes.endScrub(head, e.value);
        else engine.headLanes.previewScrub(head, e.value);
        return;
      }
      if (e.channel === "laneScrub") {
        // FUNCTION + fader N = audible positional scrub of lane N. Release
        // parks the lane and stores the loop-capture candidate.
        const ts = e.timestamp ?? performance.now();
        if (e.phase === "start") engine.beginLaneFaderScrub(head, e.value, ts);
        else if (e.phase === "cancel") engine.endLaneFaderScrub(head, e.value, true);
        else if (e.committed || e.phase === "end") engine.endLaneFaderScrub(head, e.value);
        else engine.previewLaneFaderScrub(head, e.value, ts);
        return;
      }
      if (e.channel === "headLevel") {
        if (e.phase !== "start") engine.headLanes.setLevel(head, e.value);
        return;
      }
      if (e.channel !== "fader") return;
      // Touching a stem fader targets that stem — it becomes the Heads source.
      engine.lastTargetedTrack = head;
      // In heads mode the faders are the head layer: level/scrub arrive on
      // their own channels, so the continuous bus must not move the track fader.
      if (engine.heads.active) return;
      engine.applyTrackGain(head, e.value);
    });

    return () => {
      off();
    };
  }, [engine]);


  // --- lifecycle: suspension, backgrounding, route changes ----------------
  // Audio is always on. There is no user-facing enable control: the first
  // interaction of any kind unlocks the context, and returning to a
  // backgrounded tab resumes it.
  useEffect(() => {
    const unlockNow = () => {
      void engine.unlock().then((r) => {
        setUnlockNote(r.detail);
        setStatus(engine.status());
      });
    };
    const opts = { capture: true, passive: true } as AddEventListenerOptions;
    const events = ["pointerdown", "touchstart", "keydown", "mousedown"] as const;
    for (const ev of events) window.addEventListener(ev, unlockNow, opts);

    const reconcile = () => {
      void engine.reconcileLifecycle().then(() => {
        // Returning to the foreground must restore audio without any
        // "enable audio" affordance.
        if (document.visibilityState === "visible" && engine.ready) unlockNow();
        else setStatus(engine.status());
      });
    };
    document.addEventListener("visibilitychange", reconcile);
    window.addEventListener("pagehide", reconcile);
    return () => {
      for (const ev of events) window.removeEventListener(ev, unlockNow, opts);
      document.removeEventListener("visibilitychange", reconcile);
      window.removeEventListener("pagehide", reconcile);
    };
  }, [engine]);

  // --- status polling (display only; the playhead itself is derived) ------
  useEffect(() => {
    let raf = 0;
    let last = 0;
    const tick = (t: number) => {
      if (t - last > 100) {
        last = t;
        // Park any unlooped head that has run off the end of its source.
        engine.headLanes.tick();
        setStatus(engine.status());
      }
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, [engine]);

  const unlock = useCallback(async () => {
    const r = await engine.unlock();
    setUnlockNote(r.detail);
    setStatus(engine.status());
    return r;
  }, [engine]);

  return { engine, status, acks, unlock, unlockNote, headsInFlight };
}
