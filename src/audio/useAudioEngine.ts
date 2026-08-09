import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { Ack, AudioCommand } from "./commands";
import { controlBus } from "./controlBus";
import { getAudioEngine, type EngineStatus } from "./engine";
import { installPrintCommit } from "./print";


/**
 * Drains the reducer's ordered command stream into the AudioEngine by
 * watermark, feeds the continuous control bus straight to AudioParams, keeps
 * lifecycle honest (a suspended context can never read as "playing") and
 * exposes engine truth for diagnostics.
 */
export function useAudioEngine(commands: AudioCommand[]) {
  const engine = useMemo(() => getAudioEngine(), []);
  const watermark = useRef(0);
  const [acks, setAcks] = useState<Ack[]>([]);
  const [status, setStatus] = useState<EngineStatus>(() => engine.status());
  const [unlockNote, setUnlockNote] = useState<string>("audio locked — press PLAY or enable audio");

  // --- platform budget resolves only after hydration -----------------------
  // SSR cannot know the device, so the engine starts on SSR_BUDGET and the real
  // tier is adopted here. Resolving during render would mismatch hydration.
  useEffect(() => {
    engine.resolveBudget();
    // PRINT persists through the same single-decode ingest path as a user file.
    installPrintCommit(engine);
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
    return () => {
      delete w.__stemTapeScrub;
      delete w.__stemTapeTransport;
    };
  }, [engine]);





  // --- ordered command drain (never a snapshot diff) -----------------------

  useEffect(() => {
    const pending = commands.filter((c) => c.id > watermark.current);
    if (pending.length === 0) return;
    watermark.current = pending[pending.length - 1]!.id;
    const produced: Ack[] = [];
    for (const cmd of pending) {
      if (cmd.type === "transport.play" && !engine.ready) {
        // Unlock, then re-run this exact command — the id is preserved so the
        // ack still maps to the originating gesture.
        void engine.unlock().then((r) => {
          setUnlockNote(r.detail);
          const ack = engine.execute(cmd);
          setAcks((prev) => [ack, ...prev].slice(0, 60));
          setStatus(engine.status());
        });
        continue;
      }
      produced.push(engine.execute(cmd));
    }
    if (produced.length) setAcks((prev) => [...produced.reverse(), ...prev].slice(0, 60));
    setStatus(engine.status());
  }, [commands, engine]);

  // --- continuous control bus → AudioParam / scrub kernel -----------------
  useEffect(() => {
    const off = controlBus.subscribe((e) => {
      const head = e.index as 0 | 1 | 2 | 3;
      if (e.channel === "headScrub") {
        // The whole gesture is audible: start latches the origin, every
        // rAF-coalesced move travels the tape, release lands it exactly.
        const ts = e.timestamp ?? performance.now();
        if (e.phase === "start") engine.beginHeadScrub(head, e.pointerId ?? -1, e.value, ts);
        else if (e.phase === "cancel") engine.cancelHeadScrub(head);
        else if (e.committed || e.phase === "end") engine.endHeadScrub(head, e.value);
        else engine.previewHeadScrub(head, e.value, ts);
        return;
      }
      if (e.channel === "headLevel") {
        if (e.phase !== "start") engine.applyHeadLevel(head, e.value);
        return;
      }
      if (e.channel !== "fader") return;
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
  useEffect(() => {
    const reconcile = () => {
      void engine.reconcileLifecycle().then(() => setStatus(engine.status()));
    };
    document.addEventListener("visibilitychange", reconcile);
    window.addEventListener("pagehide", reconcile);
    return () => {
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

  return { engine, status, acks, unlock, unlockNote };
}
