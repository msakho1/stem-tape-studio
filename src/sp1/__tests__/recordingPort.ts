/**
 * Transcript-recording wrapper around MockSp1's port. Both the golden Tape
 * Looper companion and the React adapter are driven through this identical
 * wrapper, so the captured transcripts are directly comparable.
 */
import { createHash } from "node:crypto";
import type { SerialLikePort } from "../protocol";

export interface TranscriptEntry {
  dir: "tx" | "rx" | "op";
  hex?: string;
  note?: string;
}

export class Recorder {
  readonly entries: TranscriptEntry[] = [];
  tx(bytes: Uint8Array) {
    this.entries.push({ dir: "tx", hex: hex(bytes) });
  }
  rx(bytes: Uint8Array) {
    this.entries.push({ dir: "rx", hex: hex(bytes) });
  }
  op(note: string) {
    this.entries.push({ dir: "op", note });
  }
  /** TX-only view: the authoritative "what did we put on the wire" comparison. */
  get txHex(): string[] {
    return this.entries.filter((e) => e.dir === "tx").map((e) => e.hex!);
  }
  get opNotes(): string[] {
    return this.entries.filter((e) => e.dir === "op").map((e) => e.note!);
  }
  sha256(): string {
    return createHash("sha256").update(JSON.stringify(this.entries)).digest("hex");
  }
}

export function hex(bytes: Uint8Array): string {
  let s = "";
  for (const b of bytes) s += b.toString(16).padStart(2, "0");
  return s;
}

export function recordPort(port: SerialLikePort, rec: Recorder): SerialLikePort {
  return {
    readable: {
      getReader() {
        const r = port.readable.getReader();
        return {
          async read() {
            const out = await r.read();
            if (out.value && out.value.length) rec.rx(out.value);
            return out;
          },
          cancel: () => r.cancel(),
          releaseLock: () => {
            rec.op("reader.releaseLock");
            r.releaseLock();
          },
        };
      },
    },
    writable: {
      getWriter() {
        const w = port.writable.getWriter();
        return {
          async write(v: Uint8Array) {
            rec.tx(v);
            return w.write(v);
          },
          releaseLock: () => {
            rec.op("writer.releaseLock");
            w.releaseLock();
          },
        };
      },
    },
    open: (o) => port.open(o),
    close: () => {
      rec.op("port.close");
      return port.close();
    },
    ...(port.setSignals ? { setSignals: (s: { dataTerminalReady: boolean; requestToSend: boolean }) => port.setSignals!(s) } : {}),
  };
}
