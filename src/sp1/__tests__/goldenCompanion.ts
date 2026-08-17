/**
 * Golden Tape Looper companion loader.
 *
 * This does NOT reimplement anything. It slices the verbatim protocol region
 * out of firmware/web/index.html (the tested Tape Looper companion) and
 * evaluates that exact source text, so conformance tests execute the original
 * bytes of the original implementation next to the React adapter.
 *
 * firmware/web/index.html is read-only here and is never written.
 */
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { resolve } from "node:path";

export const GOLDEN_COMPANION_PATH = "firmware/web/index.html";

/** Marker pair bounding the inherited protocol implementation. */
const START = "class Serial {";
const END = "/* ---- hidden #settings panel";

export interface GoldenCompanion {
  Serial: new (port: unknown) => GoldenSerial;
  parsePing(info: Uint8Array): Record<string, number>;
  handshake(): Promise<Record<string, number>>;
  readBlock(blk: number): Promise<Uint8Array>;
  writeBlock(blk: number, data512: Uint8Array): Promise<void>;
  commitToDevice(): Promise<void>;
  exit(): Promise<void>;
  parseMeta(b: Uint8Array): { magic: number; cur: number; raw: Uint8Array };
  trkBlk(slot: number, t: number): number;
  setIo(io: GoldenSerial | null): void;
  setLayout(l: Record<string, number> | null): void;
  /** Source text actually executed, and its SHA-256. */
  source: string;
  sourceSha256: string;
}

export interface GoldenSerial {
  write(bytes: Uint8Array): Promise<void>;
  read(n: number, timeoutMs?: number): Promise<Uint8Array>;
  drain(): void;
  close(): Promise<void>;
  rxTotal: number;
  closed: boolean;
}

export function goldenCompanionFileSha256(): string {
  return createHash("sha256").update(readFileSync(resolve(process.cwd(), GOLDEN_COMPANION_PATH))).digest("hex");
}

export function loadGoldenCompanion(): GoldenCompanion {
  const html = readFileSync(resolve(process.cwd(), GOLDEN_COMPANION_PATH), "utf8");
  const a = html.indexOf(START);
  const b = html.indexOf(END);
  if (a < 0 || b < 0 || b <= a) throw new Error("golden companion protocol region not found");
  const source = html.slice(a, b);

  // Stubs for the two page-level helpers the region touches. No protocol code
  // is substituted: only DOM logging.
  const factory = new Function(
    "log",
    "$",
    `${source}
    return {
      Serial, parsePing, handshake, readBlock, writeBlock, commitToDevice, exit,
      parseMeta, trkBlk, startKeepalive, stopKeepalive,
      setIo: (v) => { io = v; },
      setLayout: (v) => { layout = v; },
    };`,
  ) as (log: unknown, $: unknown) => Omit<GoldenCompanion, "source" | "sourceSha256">;

  const api = factory(
    () => {},
    () => ({ textContent: "", innerHTML: "", scrollTop: 0, scrollHeight: 0 }),
  );

  return {
    ...api,
    source,
    sourceSha256: createHash("sha256").update(source).digest("hex"),
  };
}
