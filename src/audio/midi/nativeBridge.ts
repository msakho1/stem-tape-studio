/**
 * Native CoreMIDI bridge — web side.
 *
 * The iOS/iPadOS shell delivers BATCHES of already-structured event objects via
 * `webView.callAsyncJavaScript("window.__stemTapeMidi.push(events)", arguments: [...])`.
 * Structured arguments only — the native side never interpolates JSON into an
 * executable string, and this module never evals anything it receives.
 *
 * The queue is installed eagerly so events that arrive before React mounts are
 * buffered rather than dropped.
 *
 * Checkpoint 1: subscribers only. No engine access, no audio commands.
 */

import { midiClock } from "./clock";
import { allNotesOffEvent, normalizeNativeEvent, type StemMidiEvent } from "./contract";

export type NativeBridgeState = {
  present: boolean;
  deviceName: string | null;
  ready: boolean;
};

type BridgeApi = {
  push: (events: unknown[]) => number;
  ready: (info?: { deviceName?: string }) => { perfNowMs: number };
  reanchor: () => { perfNowMs: number };
  disconnect: (info?: { deviceId?: string; deviceName?: string }) => void;
};

declare global {
  interface Window {
    __stemTapeMidi?: BridgeApi;
  }
}

export class NativeMidiBridge {
  private listeners = new Set<(ev: StemMidiEvent) => void>();
  private stateListeners = new Set<(s: NativeBridgeState) => void>();
  private buffer: StemMidiEvent[] = [];
  private state: NativeBridgeState = { present: false, deviceName: null, ready: false };

  install(win: Window & { __stemTapeMidi?: BridgeApi }): void {
    const api: BridgeApi = {
      push: (events) => this.push(events),
      ready: (info) => {
        this.publish({ present: true, ready: true, deviceName: info?.deviceName ?? this.state.deviceName });
        return { perfNowMs: this.now() };
      },
      reanchor: () => ({ perfNowMs: this.now() }),
      disconnect: (info) => {
        this.emit(
          allNotesOffEvent({
            timestampMs: this.now(),
            source: "coremidi-bridge",
            deviceId: info?.deviceId ?? "coremidi",
            deviceName: info?.deviceName ?? this.state.deviceName ?? "CoreMIDI device",
          }),
        );
        this.publish({ deviceName: null });
      },
    };
    win.__stemTapeMidi = api;
  }

  /** Accepts one batch; returns how many events normalized successfully. */
  push(events: unknown[]): number {
    if (!Array.isArray(events)) return 0;
    let n = 0;
    for (const raw of events) {
      const ev = normalizeNativeEvent(raw);
      if (!ev) continue;
      n += 1;
      if (!this.state.present || this.state.deviceName !== ev.deviceName) {
        this.publish({ present: true, deviceName: ev.deviceName });
      }
      this.emit(ev);
    }
    return n;
  }

  subscribe(fn: (ev: StemMidiEvent) => void): () => void {
    this.listeners.add(fn);
    // Drain anything that arrived before the first subscriber existed.
    if (this.buffer.length) {
      const pending = this.buffer;
      this.buffer = [];
      for (const ev of pending) fn(ev);
    }
    return () => this.listeners.delete(fn);
  }

  onStateChange(fn: (s: NativeBridgeState) => void): () => void {
    this.stateListeners.add(fn);
    fn({ ...this.state });
    return () => this.stateListeners.delete(fn);
  }

  snapshot(): NativeBridgeState {
    return { ...this.state };
  }

  private emit(ev: StemMidiEvent): void {
    if (!this.listeners.size) {
      this.buffer.push(ev);
      if (this.buffer.length > 256) this.buffer.shift();
      return;
    }
    for (const fn of this.listeners) fn(ev);
  }

  private publish(patch: Partial<NativeBridgeState>): void {
    this.state = { ...this.state, ...patch };
    const snap = { ...this.state };
    for (const fn of this.stateListeners) fn(snap);
  }

  private now(): number {
    const t = typeof performance !== "undefined" ? performance.now() : Date.now();
    // Keep the clock anchored to the page's perf origin on every handshake.
    if (!midiClock.isAnchored()) midiClock.anchor(t, 0);
    return t;
  }
}

export const nativeMidiBridge = new NativeMidiBridge();

if (typeof window !== "undefined") {
  nativeMidiBridge.install(window);
}
