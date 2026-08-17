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
import { isSp1DeviceName, sp1Surface } from "./sp1Surface";
import { trace } from "@/diagnostics/trace";
import { ledTransport, CC_CAPABILITY, type MidiOutLike } from "@/diagnostics/ledTransport";


type MidiPort = {
  id: string;
  name: string | null;
  state: "connected" | "disconnected";
  type: "input" | "output";
  onmidimessage?: ((e: { data: Uint8Array; timeStamp: number }) => void) | null;
};
type MidiOutPort = MidiPort & { send: (bytes: number[]) => void };
type MidiAccess = {
  inputs: Map<string, MidiPort>;
  outputs?: Map<string, MidiOutPort>;
  onstatechange: ((e: { port: MidiPort }) => void) | null;
};

export type MidiListener = (ev: StemMidiEvent) => void;

export type WebMidiState = {
  supported: boolean;
  status: "idle" | "requesting" | "connected" | "denied" | "unsupported";
  devices: { id: string; name: string }[];
  outputs: { id: string; name: string }[];
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
    outputs: [],
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
    return {
      ...this.state,
      devices: this.state.devices.map((d) => ({ ...d })),
      outputs: this.state.outputs.map((d) => ({ ...d })),
    };
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
      const name = this.state.devices.find((d) => d.id === id)?.name ?? "MIDI input";
      if (isSp1DeviceName(name)) {
        // Physical SP-1: release every held control, never a cue allNotesOff.
        sp1Surface.deviceDisconnected(id);
        continue;
      }
      this.emit(
        allNotesOffEvent({
          timestampMs: typeof performance !== "undefined" ? performance.now() : Date.now(),
          source: "webmidi",
          deviceId: id,
          deviceName: name,
        }),
      );
    }
    // Output discovery is INDEPENDENT of input IDs: hosts mint separate IDs per
    // direction, so the LED transport matches on normalized product identity.
    const outs: MidiOutLike[] = [];
    access.outputs?.forEach((port) => {
      if (port.state !== "connected") return;
      const name = port.name ?? "MIDI output";
      outs.push({ id: port.id, name, send: (bytes) => port.send(bytes) });
    });
    const sp1Input = devices.find((d) => isSp1DeviceName(d.name)) ?? null;
    ledTransport.setInput(sp1Input?.name ?? null);
    ledTransport.offerOutputs(outs);
    if (sp1Input && outs.length > 0) ledTransport.queryCapability();
    this.publish({ devices, outputs: outs.map((o) => ({ id: o.id, name: o.name })) });
  }

  private handle(port: MidiPort, e: { data: Uint8Array; timeStamp: number }): void {
    const name = port.name ?? "MIDI input";
    // Every inbound message opens a NEW input correlation, at the moment the
    // browser delivered it. Downstream stages inherit this ID.
    if (trace.enabled) {
      trace.beginCorrelation();
      const bytes = [...e.data];
      trace.record(
        "midi.raw",
        `${bytes.map((b) => b.toString(16).padStart(2, "0")).join(" ")} (${bytes.join(" ")})`,
        { inputId: port.id, device: name, hex: bytes.map((b) => `0x${b.toString(16).padStart(2, "0")}`), dec: bytes },
        { sourceT: Number.isFinite(e.timeStamp) ? e.timeStamp : undefined },
      );
    }
    // The physical Stem Tape SP-1 is a control surface, not an instrument: its
    // messages are consumed here and never reach the cue-learning system.
    if (isSp1DeviceName(name)) {
      // Host LED protocol capability answers are transport, not surface input.
      if ((e.data[0]! & 0xf0) === 0xb0 && e.data[1] === CC_CAPABILITY) {
        ledTransport.handleDeviceCc(e.data[1]!, e.data[2] ?? 0);
        return;
      }
      const consumed = sp1Surface.handleBytes(e.data, { id: port.id, name }, e.timeStamp);
      if (trace.enabled) {
        trace.record(
          consumed ? "midi.device.recognized" : "surface.suppressed",
          consumed ? `SP-1 message accepted (${name})` : `SP-1 device, message outside contract`,
          { device: name, bytes: [...e.data] },
        );
      }
      return;
    }
    const ev = normalizeMidiBytes(e.data, {
      timestampMs: Number.isFinite(e.timeStamp) ? e.timeStamp : performance.now(),
      source: "webmidi",
      deviceId: port.id,
      deviceName: name,
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

// Browser-proof seam: harness runs inject port messages through this handle.
if (typeof window !== "undefined") {
  (window as unknown as { __stemTapeWebMidi?: WebMidiAdapter }).__stemTapeWebMidi = webMidi;
}
