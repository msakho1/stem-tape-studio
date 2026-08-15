/**
 * MidiDrawer — MIDI transport instrumentation panel.
 *
 * Shows transport (native CoreMIDI bridge vs Web MIDI vs none), device name,
 * the last normalized event, and its timestamp / stale status. It also carries
 * the single user-gesture "Connect MIDI" control needed for the Web MIDI
 * permission prompt.
 *
 * No pads, no cue editor. Nothing here reaches the audio engine.
 */

import { useCallback, useEffect, useRef, useState } from "react";
import { midiClock } from "@/audio/midi/clock";
import { describeEvent, isStale, STALE_EVENT_MS, type StemMidiEvent } from "@/audio/midi/contract";
import { nativeMidiBridge, type NativeBridgeState } from "@/audio/midi/nativeBridge";
import { webMidi, type WebMidiState } from "@/audio/midi/webMidi";
import { getAudioEngine } from "@/audio/engine";

export function MidiDrawer() {
  // Both snapshots read browser capabilities that the server cannot know
  // (requestMIDIAccess, an installed native bridge), so they are adopted after
  // hydration instead of during the first render.
  const [web, setWeb] = useState<WebMidiState>(() => ({ ...webMidi.snapshot(), supported: false }));
  const [native, setNative] = useState<NativeBridgeState>(() => ({ present: false, deviceName: null, ready: false }));
  const [last, setLast] = useState<StemMidiEvent | null>(null);
  const [count, setCount] = useState(0);
  const [nowMs, setNowMs] = useState(() => (typeof performance !== "undefined" ? performance.now() : 0));
  const [cue, setCue] = useState<{ learned: number; invalid: number; open: number; owned: string; detail: string | null }>(
    { learned: 0, invalid: 0, open: 0, owned: "0000", detail: null },
  );
  const mounted = useRef(false);

  useEffect(() => {
    mounted.current = true;
    setWeb(webMidi.snapshot());
    setNative(nativeMidiBridge.snapshot());
    const onEvent = (ev: StemMidiEvent) => {
      setLast(ev);
      setCount((c) => c + 1);
      setNowMs(performance.now());
    };
    const offA = webMidi.subscribe(onEvent);
    const offB = nativeMidiBridge.subscribe(onEvent);
    const offC = webMidi.onStateChange(setWeb);
    const offD = nativeMidiBridge.onStateChange(setNative);
    // Calibration pair: refreshed here and on visibility change.
    const anchor = () => midiClock.anchor(performance.now(), midiClock.calibration().ctxTime0);
    document.addEventListener("visibilitychange", anchor);
    const id = window.setInterval(() => {
      setNowMs(performance.now());
      const snap = getAudioEngine().cueSnapshot();
      setCue({
        learned: snap.learned,
        invalid: snap.invalid,
        open: snap.openCaptures.length,
        owned: snap.owned.map((o) => (o ? "1" : "0")).join(""),
        detail: snap.lastDetail,
      });
    }, 250);
    return () => {
      offA();
      offB();
      offC();
      offD();
      document.removeEventListener("visibilitychange", anchor);
      window.clearInterval(id);
    };
  }, []);

  /**
   * The permission prompt is the only guaranteed user gesture on this strip, so
   * it also unlocks Web Audio and re-anchors the calibration pair. Without that
   * a learned cue's first frame would be derived from an unanchored clock.
   */
  const connect = useCallback(() => {
    void (async () => {
      const engine = getAudioEngine();
      // unlock() re-anchors midiClock with a context time sampled alongside
      // performance.now(); anchoring here separately would break the pair.
      await engine.unlock();
      await webMidi.connect();
    })();
  }, []);

  const transport = native.present ? "coremidi-bridge" : web.status === "connected" ? "webmidi" : "none";
  const deviceName = native.present
    ? native.deviceName ?? "CoreMIDI device"
    : web.devices[0]?.name ?? "—";
  const stale = last ? isStale(last, nowMs) : false;

  const connected = transport !== "none";

  return (
    <div
      className="border border-[var(--bench-line)] px-2 py-1.5"
      data-testid="midi-drawer"
      data-midi-transport={transport}
      data-midi-device={deviceName}
      data-midi-events={count}
      data-midi-last={last ? describeEvent(last) : ""}
      data-midi-stale={stale ? "true" : "false"}
      data-cue-learned={cue.learned}
      data-cue-invalid={cue.invalid}
      data-cue-open={cue.open}
      data-cue-owned={cue.owned}
      data-cue-detail={cue.detail ?? ""}
      role="status"
    >
      <div className="flex flex-wrap items-center gap-x-3 gap-y-1">
        <span className="font-mono text-[10px] uppercase tracking-[0.28em] text-[var(--signal)]">
          midi{connected ? " ·" : ""}{" "}
          {transport === "coremidi-bridge" ? "native" : transport === "webmidi" ? "connected" : ""}
        </span>
        <span className="min-w-0 truncate font-mono text-[10px] uppercase tracking-[0.14em] text-[var(--ink-dim)]">
          {connected ? deviceName : "not connected"}
        </span>
        <span className="font-mono text-[10px] uppercase tracking-[0.14em] text-[var(--ink-faint)]">
          {last ? describeEvent(last) : "no events"}
        </span>
        <span
          className="font-mono text-[10px] uppercase tracking-[0.14em] text-[var(--ink-faint)]"
          data-testid="midi-drawer-timestamp"
        >
          {last ? `t ${last.timestampMs.toFixed(1)} ms · ${stale ? `stale >${STALE_EVENT_MS}ms` : "fresh"}` : "t —"}
        </span>
        {!native.present && web.status !== "connected" && (
          <button
            type="button"
            onClick={connect}
            disabled={!web.supported || web.status === "requesting"}
            data-testid="midi-connect"
            className="border border-[var(--bench-line)] px-2 py-0.5 font-mono text-[10px] uppercase tracking-[0.18em] text-[var(--ink)] disabled:opacity-40"
          >
            {web.status === "requesting" ? "requesting…" : web.supported ? "connect midi" : "no web midi"}
          </button>
        )}
        <span className="font-mono text-[10px] uppercase tracking-[0.14em] text-[var(--ink-faint)]" data-testid="midi-drawer-markers">
          cues {cue.learned}
          {cue.invalid > 0 ? ` · ${cue.invalid} stale` : ""}
          {cue.open > 0 ? ` · learning ${cue.open}` : ""}
          {cue.owned !== "0000" ? ` · playing ${cue.owned}` : ""}
        </span>
        {cue.detail && (
          <span className="font-mono text-[10px] normal-case tracking-[0.04em] text-[var(--ink-dim)]" data-testid="midi-drawer-detail">
            {cue.detail}
          </span>
        )}
        {web.error && (
          <span className="font-mono text-[10px] uppercase tracking-[0.14em] text-[var(--signal)]">{web.error}</span>
        )}
      </div>
    </div>
  );
}
