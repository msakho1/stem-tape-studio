/**
 * M0 CDC firmware console (Web Serial).
 *
 * The physical SP-1 running Stem Tape M0 exposes a CDC ACM diagnostic
 * interface next to its class-compliant USB MIDI interface. This module reads
 * that text stream and parses ONLY what the firmware actually prints.
 *
 * IMPORTANT BOUNDARY: the firmware cannot transmit its flashed binary, so the
 * expected artifact metadata below is *expected build metadata*, never a
 * verified device identity. Nothing here can attest what is really on the chip.
 */

import { trace, traceNow } from "./trace";

export const SP1_USB_VENDOR_ID = 0x1915;
export const SP1_USB_PRODUCT_ID = 0x5211;
export const SP1_SERIAL_BAUD = 115200;

export const EXPECTED_ARTIFACT = {
  product: "Stem Tape SP-1",
  firmwareBanner: "Stem Tape M0 v1.0.0",
  usbVendorId: "0x1915",
  usbProductId: "0x5211",
  binarySha256: "53de4c003047e20b7e18c45034eab89b79d58ba1677651348e62f9a85b257eeb",
  sourceCommit: "ea354f32c8c484c1d48e68804a4f1695a8a7b131",
  note: "expected build metadata — the device cannot transmit or attest its flashed binary",
} as const;

export type FirmwareLine =
  | { kind: "banner"; version: string; target: string }
  | {
      kind: "watchdog";
      preRunning: boolean;
      ours: boolean;
      installRc: number;
      setupRc: number;
      rren: number;
      runstatus: number;
    }
  | { kind: "fields"; fields: string[] }
  | { kind: "sample"; ain0: number; ain1: number; decoded: string; stable: string; unmeasured: number }
  | { kind: "other"; text: string };

const BANNER = /^(Stem Tape M0 v[0-9]+\.[0-9]+\.[0-9]+)\s+diagnostic target:\s*(.+)$/;
const WDT =
  /^wdt:\s*pre_running=(-?\d+)\s+ours=(-?\d+)\s+install_rc=(-?\d+)\s+setup_rc=(-?\d+)\s+rren=(0x[0-9a-fA-F]+|\d+)\s+runstatus=(\d+)/;
const FIELDS = /^fields:\s*(.+)$/;
const SAMPLE =
  /^AIN0=\s*(-?\d+)\s+AIN1=\s*(-?\d+)\s+dec=(\S+)\s+stable=(\S+)\s+unmeas=(\d+)/;

/** Pure: one raw console line → one parsed record. Never throws. */
export function parseFirmwareLine(raw: string): FirmwareLine {
  const line = raw.replace(/\r/g, "").trim();
  let m = BANNER.exec(line);
  if (m) return { kind: "banner", version: m[1]!, target: m[2]!.trim() };
  m = WDT.exec(line);
  if (m) {
    return {
      kind: "watchdog",
      preRunning: m[1] !== "0",
      ours: m[2] !== "0",
      installRc: Number(m[3]),
      setupRc: Number(m[4]),
      rren: m[5]!.startsWith("0x") ? parseInt(m[5]!, 16) : Number(m[5]),
      runstatus: Number(m[6]),
    };
  }
  m = FIELDS.exec(line);
  if (m) return { kind: "fields", fields: m[1]!.trim().split(/\s+/) };
  m = SAMPLE.exec(line);
  if (m) {
    return {
      kind: "sample",
      ain0: Number(m[1]),
      ain1: Number(m[2]),
      decoded: m[3]!,
      stable: m[4]!,
      unmeasured: Number(m[5]),
    };
  }
  return { kind: "other", text: line };
}

export interface FirmwareConsoleState {
  supported: boolean;
  status: "idle" | "unsupported" | "requesting" | "connected" | "error" | "closed";
  error: string | null;
  reportedVersion: string | null;
  target: string | null;
  watchdog: Extract<FirmwareLine, { kind: "watchdog" }> | null;
  ain0: number | null;
  ain1: number | null;
  decodedMask: string | null;
  stableMask: string | null;
  unmeasured: number | null;
  lines: string[];
  lineCount: number;
}

const MAX_LINES = 120;

function initialState(): FirmwareConsoleState {
  const supported = typeof navigator !== "undefined" && "serial" in navigator;
  return {
    supported,
    status: supported ? "idle" : "unsupported",
    error: null,
    reportedVersion: null,
    target: null,
    watchdog: null,
    ain0: null,
    ain1: null,
    decodedMask: null,
    stableMask: null,
    unmeasured: null,
    lines: [],
    lineCount: 0,
  };
}

/** Pure reducer so parsing is testable without any Web Serial mock. */
export function applyFirmwareLine(
  state: FirmwareConsoleState,
  raw: string,
): FirmwareConsoleState {
  const parsed = parseFirmwareLine(raw);
  const lines = [...state.lines, raw.replace(/\r/g, "")].slice(-MAX_LINES);
  const next: FirmwareConsoleState = { ...state, lines, lineCount: state.lineCount + 1 };
  if (parsed.kind === "banner") {
    next.reportedVersion = parsed.version;
    next.target = parsed.target;
  } else if (parsed.kind === "watchdog") {
    next.watchdog = parsed;
  } else if (parsed.kind === "sample") {
    next.ain0 = parsed.ain0;
    next.ain1 = parsed.ain1;
    next.decodedMask = parsed.decoded;
    next.stableMask = parsed.stable;
    next.unmeasured = parsed.unmeasured;
  }
  return next;
}

type SerialLike = {
  requestPort: (o: { filters: { usbVendorId: number; usbProductId: number }[] }) => Promise<SerialPortLike>;
};
export type SerialPortLike = {
  open: (o: { baudRate: number }) => Promise<void>;
  close: () => Promise<void>;
  setSignals?: (s: { dataTerminalReady: boolean }) => Promise<void>;
  readable: ReadableStream<Uint8Array> | null;
};

export class FirmwareConsole {
  private state = initialState();
  private listeners = new Set<(s: FirmwareConsoleState) => void>();
  private port: SerialPortLike | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private pending = "";
  private closing = false;

  /** Test seam — injected in place of navigator.serial. */
  serialImpl: SerialLike | null = null;

  snapshot(): FirmwareConsoleState {
    return { ...this.state, lines: [...this.state.lines] };
  }

  subscribe(fn: (s: FirmwareConsoleState) => void): () => void {
    this.listeners.add(fn);
    fn(this.snapshot());
    return () => this.listeners.delete(fn);
  }

  private serial(): SerialLike | null {
    if (this.serialImpl) return this.serialImpl;
    if (typeof navigator === "undefined") return null;
    return ((navigator as unknown as { serial?: SerialLike }).serial) ?? null;
  }

  /** MUST be called from a user gesture — never automatically. */
  async connect(): Promise<FirmwareConsoleState> {
    const serial = this.serial();
    if (!serial) return this.publish({ supported: false, status: "unsupported" });
    this.publish({ supported: true, status: "requesting", error: null });
    try {
      const port = await serial.requestPort({
        filters: [{ usbVendorId: SP1_USB_VENDOR_ID, usbProductId: SP1_USB_PRODUCT_ID }],
      });
      await port.open({ baudRate: SP1_SERIAL_BAUD });
      // M0 only prints while DTR is asserted (diag_open()).
      await port.setSignals?.({ dataTerminalReady: true });
      this.port = port;
      this.closing = false;
      this.publish({ status: "connected", error: null });
      void this.readLoop();
      return this.snapshot();
    } catch (err) {
      return this.publish({ status: "error", error: err instanceof Error ? err.message : String(err) });
    }
  }

  /** Feed one already-decoded line (mock console / smoke harness). */
  ingestLine(line: string): void {
    this.state = applyFirmwareLine(this.state, line);
    trace.record("serial.line", `CDC: ${line.slice(0, 96)}`, { line }, traceNow());
    this.publishRaw();
  }

  /** Feed an arbitrary USB chunk (mock console / smoke harness). */
  feedChunk(text: string): void {
    this.ingestChunk(text);
  }

  private ingestChunk(text: string): void {
    this.pending += text;
    const parts = this.pending.split("\n");
    this.pending = parts.pop() ?? "";
    for (const p of parts) if (p.trim()) this.ingestLine(p);
  }

  private async readLoop(): Promise<void> {
    const port = this.port;
    if (!port?.readable) return;
    const decoder = new TextDecoder();
    const reader = port.readable.getReader();
    this.reader = reader;
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value) this.ingestChunk(decoder.decode(value, { stream: true }));
      }
    } catch (err) {
      if (!this.closing) this.publish({ status: "error", error: err instanceof Error ? err.message : String(err) });
    } finally {
      try {
        reader.releaseLock();
      } catch {
        /* already released */
      }
      if (this.reader === reader) this.reader = null;
    }
  }

  /** Full teardown: cancel reader, release lock, lower DTR, close the port. */
  async disconnect(): Promise<void> {
    this.closing = true;
    const reader = this.reader;
    const port = this.port;
    this.reader = null;
    this.port = null;
    this.pending = "";
    if (reader) {
      try {
        await reader.cancel();
      } catch {
        /* ignore */
      }
      try {
        reader.releaseLock();
      } catch {
        /* ignore */
      }
    }
    if (port) {
      try {
        await port.setSignals?.({ dataTerminalReady: false });
      } catch {
        /* ignore */
      }
      try {
        await port.close();
      } catch {
        /* ignore */
      }
    }
    this.publish({ status: this.state.supported ? "closed" : "unsupported" });
  }

  private publish(patch: Partial<FirmwareConsoleState>): FirmwareConsoleState {
    this.state = { ...this.state, ...patch };
    return this.publishRaw();
  }

  private publishRaw(): FirmwareConsoleState {
    const snap = this.snapshot();
    for (const fn of this.listeners) fn(snap);
    return snap;
  }
}

export const firmwareConsole = new FirmwareConsole();

// Browser-proof seam: the smoke harness feeds mocked CDC lines through this.
if (typeof window !== "undefined") {
  (window as unknown as { __stemTapeFirmwareConsole?: FirmwareConsole }).__stemTapeFirmwareConsole =
    firmwareConsole;
}
