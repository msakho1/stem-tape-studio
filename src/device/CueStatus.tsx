/**
 * CueStatus — MIDI transport instrumentation strip (Checkpoint 1).
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

export function CueStatus() {
  const [web, setWeb] = useState<WebMidiState>(() => webMidi.snapshot());
  const [native, setNative] = useState<NativeBridgeState>(() => nativeMidiBridge.snapshot());
  const [last, setLast] = useState<StemMidiEvent | null>(null);
  const [count, setCount] = useState(0);
  const [nowMs, setNowMs] = useState(() => (typeof performance !== "undefined" ? performance.now() : 0));
  const mounted = useRef(false);

  useEffect(() => {
    mounted.current = true;
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
    const id = window.setInterval(() => setNowMs(performance.now()), 500);
    return () => {
      offA();
      offB();
      offC();
      offD();
      document.removeEventListener("visibilitychange", anchor);
      window.clearInterval(id);
    };
  }, []);

  const connect = useCallback(() => {
    void webMidi.connect();
  }, []);

  const transport = native.present ? "coremidi-bridge" : web.status === "connected" ? "webmidi" : "none";
  const deviceName = native.present
    ? native.deviceName ?? "CoreMIDI device"
    : web.devices[0]?.name ?? "—";
  const stale = last ? isStale(last, nowMs) : false;

  return (
    <div
      className="mb-3 flex flex-wrap items-center gap-x-3 gap-y-1 border border-[var(--bench-line)] px-2 py-1.5"
      data-testid="cue-status"
      data-midi-transport={transport}
      data-midi-device={deviceName}
      data-midi-events={count}
      data-midi-last={last ? describeEvent(last) : ""}
      data-midi-stale={stale ? "true" : "false"}
      role="status"
    >
      <span className="font-mono text-[10px] uppercase tracking-[0.28em] text-[var(--signal)]">
        midi · {transport === "coremidi-bridge" ? "native" : transport === "webmidi" ? "web midi" : "none"}
      </span>
      <span className="font-mono text-[10px] uppercase tracking-[0.14em] text-[var(--ink-dim)]">
        {deviceName}
      </span>
      <span className="font-mono text-[10px] uppercase tracking-[0.14em] text-[var(--ink-faint)]">
        {last ? describeEvent(last) : "no events"}
      </span>
      <span
        className="font-mono text-[10px] uppercase tracking-[0.14em] text-[var(--ink-faint)]"
        data-testid="cue-status-timestamp"
      >
        {last ? `t ${last.timestampMs.toFixed(1)} ms · ${stale ? `stale >${STALE_EVENT_MS}ms` : "fresh"}` : "t —"}
      </span>
      {!native.present && web.status !== "connected" && (
        <button
          type="button"
          onClick={connect}
          disabled={!web.supported || web.status === "requesting"}
          data-testid="cue-connect-midi"
          className="border border-[var(--bench-line)] px-2 py-0.5 font-mono text-[10px] uppercase tracking-[0.18em] text-[var(--ink)] disabled:opacity-40"
        >
          {web.status === "requesting" ? "requesting…" : web.supported ? "connect midi" : "no web midi"}
        </button>
      )}
      {web.error && (
        <span className="font-mono text-[10px] uppercase tracking-[0.14em] text-[var(--signal)]">{web.error}</span>
      )}
    </div>
  );
}
