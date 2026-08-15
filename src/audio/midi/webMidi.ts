/**
 * Web MIDI adapter (desktop / Android Chrome).
 *
 * Emits the same `StemMidiEvent` shape as the native CoreMIDI bridge, so the
 * rest of the app cannot tell the transports apart. Access is only requested
 * from an explicit user gesture ("Connect MIDI" in CueStatus).
 *
 * Checkpoint 1: this adapter delivers events to subscribers only. It never
 * touches the AudioEngine and emits no audio commands.
 */

import { allNotesOffEvent, normalizeMidiBytes, type StemMidiEvent } from "./contract";

type MidiPort = {
  id: string;
  name: string | null;
  state: "connected" | "disconnected";
  type: "input" | "output";
  onmidimessage?: ((e: { data: Uint8Array; timeStamp: number }) => void) | null;
};
type MidiAccess = {
  inputs: Map<string, MidiPort>;
  onstatechange: ((e: { port: MidiPort }) => void) | null;
};

export type MidiListener = (ev: StemMidiEvent) => void;

export type WebMidiState = {
  supported: boolean;
  status: "idle" | "requesting" | "connected" | "denied" | "unsupported";
  devices: { id: string; name: string }[];
  error: string | null;
};

export class WebMidiAdapter {
  private access: MidiAccess | null = null;
  private listeners = new Set<MidiListener>();
  private changeListeners = new Set<(s: WebMidiState) => void>();
  private bound = new Set<string>();
  private state: WebMidiState = {
    supported: typeof navigator !== "undefined" && "requestMIDIAccess" in navigator,
    status: typeof navigator !== "undefined" && "requestMIDIAccess" in navigator ? "idle" : "unsupported",
    devices: [],
    error: null,
  };

  subscribe(fn: MidiListener): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  onStateChange(fn: (s: WebMidiState) => void): () => void {
    this.changeListeners.add(fn);
    fn(this.snapshot());
    return () => this.changeListeners.delete(fn);
  }

  snapshot(): WebMidiState {
    return { ...this.state, devices: this.state.devices.map((d) => ({ ...d })) };
  }

  /** Must be called from a user gesture. */
  async connect(): Promise<WebMidiState> {
    if (!this.state.supported) return this.publish({ status: "unsupported" });
    this.publish({ status: "requesting", error: null });
    try {
      const req = (navigator as unknown as {
        requestMIDIAccess: (o?: { sysex: boolean }) => Promise<MidiAccess>;
      }).requestMIDIAccess;
      const access = await req.call(navigator, { sysex: false });
      this.access = access;
      access.onstatechange = () => this.rescan();
      this.rescan();
      return this.publish({ status: "connected", error: null });
    } catch (err) {
      return this.publish({ status: "denied", error: err instanceof Error ? err.message : String(err) });
    }
  }

  /** Enumerate inputs and (re)bind handlers. Safe to call repeatedly — hot-plug. */
  rescan(): void {
    const access = this.access;
    if (!access) return;
    const devices: { id: string; name: string }[] = [];
    const seen = new Set<string>();
    access.inputs.forEach((port) => {
      if (port.state !== "connected") return;
      const name = port.name ?? "MIDI input";
      seen.add(port.id);
      devices.push({ id: port.id, name });
      if (!this.bound.has(port.id)) {
        this.bound.add(port.id);
        port.onmidimessage = (e) => this.handle(port, e);
      }
    });
    // Disconnected ports: normalize to allNotesOff, exactly like the native side.
    for (const id of [...this.bound]) {
      if (seen.has(id)) continue;
      this.bound.delete(id);
      this.emit(
        allNotesOffEvent({
          timestampMs: typeof performance !== "undefined" ? performance.now() : Date.now(),
          source: "webmidi",
          deviceId: id,
          deviceName: this.state.devices.find((d) => d.id === id)?.name ?? "MIDI input",
        }),
      );
    }
    this.publish({ devices });
  }

  private handle(port: MidiPort, e: { data: Uint8Array; timeStamp: number }): void {
    const ev = normalizeMidiBytes(e.data, {
      timestampMs: Number.isFinite(e.timeStamp) ? e.timeStamp : performance.now(),
      source: "webmidi",
      deviceId: port.id,
      deviceName: port.name ?? "MIDI input",
    });
    if (ev) this.emit(ev);
  }

  /** Test/browser-proof seam: inject a raw message as if it came from a port. */
  injectMessage(data: number[], port: { id: string; name: string }, timeStamp: number): void {
    this.handle({ id: port.id, name: port.name, state: "connected", type: "input" }, {
      data: Uint8Array.from(data),
      timeStamp,
    });
  }

  private emit(ev: StemMidiEvent): void {
    for (const fn of this.listeners) fn(ev);
  }

  private publish(patch: Partial<WebMidiState>): WebMidiState {
    this.state = { ...this.state, ...patch };
    const snap = this.snapshot();
    for (const fn of this.changeListeners) fn(snap);
    return snap;
  }
}

export const webMidi = new WebMidiAdapter();
