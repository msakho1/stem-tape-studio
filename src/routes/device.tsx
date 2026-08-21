/**
 * SP-1 companion uploader — consumer workflow.
 *
 * Exactly three user-facing steps: connect the SP-1, load stems, upload.
 * Everything device-specific (A/B regions, STIX index records, generations,
 * block addressing, capability structures) stays inside the transport and is
 * surfaced only through the Activity log, never in the primary workflow.
 *
 * This route constructs NO command bytes, block addresses, index bytes or CRC
 * records. Physical mutation remains gated by the same capability verdict as
 * before; this pass changed presentation and preparation, not the protocol.
 */
import stemTapeLogo from "@/assets/stemtape-logo.png.asset.json";
import { createFileRoute, Link } from "@tanstack/react-router";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { SupportButton } from "@/components/SupportButton";
import {
  ClipboardCopy,
  Download,
  Info,
  Loader2,
  Trash2,
  Upload,
  Usb,
  X,
} from "lucide-react";
import sp1Outline from "@/assets/stem-tape-sp1-outline.svg";
import { Sp1ConnectLeds } from "@/device/Sp1ConnectLeds";

import { Sp1Transport, Sp1Session, BAUD_RATE, type SerialLikePort } from "@/sp1/protocol";
import { STEM_ORDER, STEM_LABEL, type StemSlotName } from "@/sp1/prepare";
import { prepareCanonicalSong, type CanonicalSong } from "@/sp1/song";
import { parseCapabilities, readOnlyVerdict, type CompatibilityVerdict } from "@/sp1/compatibility";
import { StemTapeTransport, type UploadResult, type BulkTransactionRecord } from "@/sp1/transport";
import { assignFiles, inferTitle } from "@/sp1/stemNaming";
import { analyzeTiming, timingLabel, type SongTiming } from "@/sp1/autoTiming";
import {
  browserStorage,
  fingerprintSources,
  loadPrepared,
  prepareChunked,
  type PrepStorage,
  type PreparedManifest,
} from "@/sp1/preparation";
import { sectorsForFrames } from "@/sp1/stemTapeFormat";
import { assessCapacity, uploadEnabled, type CapabilityQueryState } from "@/sp1/capacity";

export const Route = createFileRoute("/device")({
  component: DevicePage,
  head: () => ({
    meta: [
      { title: "Upload songs to your SP-1 — Stem Tape" },
      {
        name: "description",
        content:
          "Connect your Stem Tape SP-1, drop in four stems, and upload. Tempo and title are detected automatically and nothing ever leaves your device.",
      },
      { property: "og:title", content: "Upload songs to your SP-1 — Stem Tape" },
      {
        property: "og:description",
        content: "Three steps: connect the SP-1, load four stems, upload. All local, no account.",
      },
      { property: "og:type", content: "website" },
      { name: "twitter:card", content: "summary" },
    ],
  }),
});

type Files = Partial<Record<StemSlotName, File>>;
type Decoded = Partial<Record<StemSlotName, AudioBuffer>>;
type LogLevel = "info" | "success" | "warning" | "error";
interface LogEntry {
  at: string;
  level: LogLevel;
  text: string;
}

const fmtMiB = (n: number) => `${(n / 1048576).toFixed(1)} MiB`;
const fmtClock = (s: number) => {
  if (!Number.isFinite(s) || s < 0) return "—";
  const m = Math.floor(s / 60);
  return `${m}:${String(Math.round(s - m * 60)).padStart(2, "0")}`;
};
const fmtDur = (s: number) => {
  const m = Math.floor(s / 60);
  return `${m}:${String(Math.floor(s - m * 60)).padStart(2, "0")}`;
};

function DevicePage() {
  const [supported, setSupported] = useState<boolean | null>(null);
  const [log, setLog] = useState<LogEntry[]>([]);
  const [connected, setConnected] = useState(false);
  const [connecting, setConnecting] = useState(false);
  const [verdict, setVerdict] = useState<CompatibilityVerdict>(() => readOnlyVerdict());
  const [queryState, setQueryState] = useState<CapabilityQueryState>("none");
  const [sectorsPerSong, setSectorsPerSong] = useState<number | null>(null);
  const [mockMode, setMockMode] = useState(false);
  const [bulkCapable, setBulkCapable] = useState(false);

  const [files, setFiles] = useState<Files>({});
  const [decoded, setDecoded] = useState<Decoded>({});
  const [pending, setPending] = useState<File[]>([]);
  const [title, setTitle] = useState("");
  const [titleTouched, setTitleTouched] = useState(false);
  const [timing, setTiming] = useState<SongTiming | null>(null);
  const [editTiming, setEditTiming] = useState(false);

  const [song, setSong] = useState<CanonicalSong | null>(null);
  const [manifest, setManifest] = useState<PreparedManifest | null>(null);
  const [prepState, setPrepState] = useState<"idle" | "working" | "ready" | "error">("idle");
  const [prepDetail, setPrepDetail] = useState("");
  const [prepFraction, setPrepFraction] = useState<number | null>(null);

  const [uploading, setUploading] = useState(false);
  const [progress, setProgress] = useState<{
    fraction: number;
    /** Bytes CONFIRMED from device storage, never bytes merely sent. */
    verifiedBytes: number;
    total: number;
    sectors: number;
    sectorsTotal: number;
    rate: number;
    eta: number;
    retries: number;
    checkpoint: string;
  } | null>(null);
  const [result, setResult] = useState<UploadResult | null>(null);

  const transportRef = useRef<StemTapeTransport | null>(null);
  const abortRef = useRef({ aborted: false });
  /* bulkTransactions lives only on the live Sp1Transport/StemTapeTransport
   * instance, in memory. If the SP-1 drops the connection after a failed
   * upload (or the user reconnects before downloading the report), a fresh
   * instance replaces transportRef.current with an empty bulkTransactions
   * array -- silently discarding exactly the per-sector status/writeMs/
   * ackMs detail a failed transfer's diagnostic report most needs, even
   * though the human-readable `events` log (separate React state, not tied
   * to the transport object) still shows the same responses were received.
   * Snapshot the finished attempt's records here, right when they're still
   * live, so downloadReport() below has them regardless of what happens to
   * the connection afterward. */
  const lastBulkTransactionsRef = useRef<readonly BulkTransactionRecord[]>([]);
  const fileInputRef = useRef<HTMLInputElement | null>(null);
  const replaceRef = useRef<StemSlotName | null>(null);
  const storageRef = useRef<PrepStorage | null>(null);
  const prepTokenRef = useRef(0);

  const say = useCallback((text: string, level: LogLevel = "info") => {
    setLog((l) => [...l.slice(-200), { at: new Date().toLocaleTimeString(), level, text }]);
  }, []);

  useEffect(() => {
    setSupported(
      typeof navigator !== "undefined" && !!(navigator as Navigator & { serial?: unknown }).serial,
    );
    storageRef.current = browserStorage();
  }, []);

  /* ------------------------------------------------------------ step 1 */

  const connect = useCallback(async () => {
    const injected = import.meta.env.DEV
      ? (globalThis as { __SP1_MOCK_PORT__?: SerialLikePort }).__SP1_MOCK_PORT__
      : undefined;
    const nav = navigator as Navigator & { serial?: { requestPort(): Promise<SerialLikePort> } };
    if (!injected && !nav.serial) return;
    let port: SerialLikePort;
    try {
      port = injected ?? (await nav.serial!.requestPort());
    } catch {
      return; // picker dismissed
    }
    setConnecting(true);
    setQueryState("pending");
    say("Opening the USB serial port…");
    try {
      await port.open({ baudRate: BAUD_RATE });
      const io = new Sp1Transport(port);
      const session = new Sp1Session(io);
      try {
        await port.setSignals?.({ dataTerminalReady: true, requestToSend: true });
      } catch {
        /* optional */
      }
      const l = await session.handshake(40);
      const rawCaps = await session.queryCapabilities();
      const caps = rawCaps ? parseCapabilities(rawCaps) : null;
      const t = new StemTapeTransport(session, caps, { kind: injected ? "mock" : "physical" });
      transportRef.current = t;
      setVerdict(t.verdict);
      setQueryState(t.verdict.writable ? "compatible" : "unverified");
      setMockMode(!!injected);
      await t.readIndex();
      const d = t.describe();
      setSectorsPerSong(d.sectorsPerSong);
      setConnected(true);
      say(`Connected · ${l.sampleRate / 1000} kHz · transport ${d.transport}.`, "success");
      const bulk = t.bulkSupported;
      setBulkCapable(bulk);
      say(
        bulk
          ? `Firmware capability: fast verified-sector upload available (STBC, ${session.bulkCaps!.maxSectorBytes} bytes per sector).`
          : "Firmware capability: this SP-1 does not offer the fast verified-sector upload command.",
        bulk ? "success" : "warning",
      );
      const lib = t.library;
      if (lib?.active) {
        say(
          `Active song: generation ${lib.active.generation} · “${lib.active.title || "untitled"}” · song slot ${lib.active.songSlot === 0 ? "A" : "B"} / index slot ${lib.activeIndexSlot === 0 ? "A" : "B"}.`,
        );
      } else {
        say("No song has been written to this SP-1 yet.");
      }
      session.startKeepalive();
    } catch (e) {
      say(`Could not reach the SP-1: ${e instanceof Error ? e.message : String(e)}`, "error");
      await transportRef.current?.disconnect().catch(() => {});
      transportRef.current = null;
      setConnected(false);
      setSectorsPerSong(null);
      setBulkCapable(false);
      setQueryState("none");
    } finally {
      setConnecting(false);
    }
  }, [say]);

  const disconnect = useCallback(async () => {
    const t = transportRef.current;
    if (!t) return;
    await t.disconnect();
    transportRef.current = null;
    setConnected(false);
    setSectorsPerSong(null);
    setBulkCapable(false);
    setQueryState("none");
    setVerdict(readOnlyVerdict());
    say("Disconnected. The SP-1 has resumed normal operation.");
  }, [say]);

  useEffect(() => () => void transportRef.current?.disconnect().catch(() => {}), []);

  /* ------------------------------------------------------------ step 2 */

  const decodeInto = useCallback(
    async (role: StemSlotName, file: File) => {
      setFiles((f) => ({ ...f, [role]: file }));
      setResult(null);
      const ac = new AudioContext();
      try {
        const buf = await ac.decodeAudioData(await file.arrayBuffer());
        setDecoded((d) => ({ ...d, [role]: buf }));
        say(`${STEM_LABEL[role]}: ${file.name} · ${fmtDur(buf.duration)}`);
      } catch (e) {
        setDecoded((d) => {
          const n = { ...d };
          delete n[role];
          return n;
        });
        say(
          `${STEM_LABEL[role]}: could not read ${file.name} — ${e instanceof Error ? e.message : String(e)}`,
          "error",
        );
      } finally {
        void ac.close();
      }
    },
    [say],
  );

  const acceptFiles = useCallback(
    (list: File[]) => {
      if (list.length === 0) return;
      const forced = replaceRef.current;
      replaceRef.current = null;
      if (forced) {
        void decodeInto(forced, list[0]!);
        return;
      }
      const { assigned, ambiguous } = assignFiles(list);
      for (const role of STEM_ORDER) {
        const f = assigned[role];
        if (f) void decodeInto(role, f);
      }
      if (ambiguous.length) {
        setPending((p) => [...p, ...ambiguous]);
        say(`${ambiguous.length} file(s) need a stem chosen by hand.`, "warning");
      }
      if (!titleTouched) {
        const inferred = inferTitle(list.map((f) => f.name));
        if (inferred) setTitle(inferred);
      }
    },
    [decodeInto, say, titleTouched],
  );

  const removeStem = useCallback((role: StemSlotName) => {
    setFiles((f) => {
      const n = { ...f };
      delete n[role];
      return n;
    });
    setDecoded((d) => {
      const n = { ...d };
      delete n[role];
      return n;
    });
    setSong(null);
    setManifest(null);
    setPrepState("idle");
  }, []);

  const allFour = STEM_ORDER.every((n) => !!decoded[n] && !!files[n]);

  /* pre-preparation work that used to look like nothing was happening */
  const decodingStems = STEM_ORDER.some((n) => !!files[n] && !decoded[n]);
  const busyPhase: string | null = decodingStems
    ? "Reading stem files…"
    : allFour && !timing
      ? "Analyzing tempo and downbeat…"
      : null;

  /* automatic timing as soon as the four stems are decoded */
  useEffect(() => {
    if (!allFour) {
      setTiming(null);
      return;
    }
    if (timing?.edited) return;
    const t = analyzeTiming(
      STEM_ORDER.map((name) => {
        const buf = decoded[name]!;
        return { name, channel: buf.getChannelData(0), sampleRate: buf.sampleRate };
      }),
    );
    setTiming(t);
    say(
      `${timingLabel(t)} · first beat at ${t.downbeatSeconds.toFixed(3)}s (${t.origin} analysis, ${t.confidence} confidence)`,
    );
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [allFour, decoded]);

  /* automatic preparation — no button */
  useEffect(() => {
    if (!allFour || !timing) return;
    const token = ++prepTokenRef.current;
    const storage = storageRef.current;
    if (!storage) return;
    const fingerprint = fingerprintSources({
      files: Object.fromEntries(
        STEM_ORDER.filter((r) => files[r]).map((r) => [
          r,
          { name: files[r]!.name, size: files[r]!.size, lastModified: files[r]!.lastModified },
        ]),
      ),
      timing,
      title,
    });

    void (async () => {
      setPrepState("working");
      setPrepDetail("Preparing audio…");
      setPrepFraction(0);
      try {
        const cached = await loadPrepared(storage, fingerprint);
        const prepared = await prepareCanonicalSong(
          STEM_ORDER.map((n) => ({ name: n, filename: files[n]!.name, buffer: decoded[n]! })),
          {
            metadata: {
              title,
              artist: "",
              bpm: timing.bpm,
              downbeatSeconds: timing.downbeatSeconds,
            },
          },
        );
        if (prepTokenRef.current !== token) return;
        setSong(prepared);
        if (prepared.lengthSpreadSeconds > 0.001) {
          say(
            `Stems differ in length by ${prepared.lengthSpreadSeconds.toFixed(2)}s — the shorter ones were padded with silence.`,
            "warning",
          );
        }
        const m =
          cached ??
          (await prepareChunked({
            song: prepared,
            fingerprint,
            storage,
            onProgress: (f) => {
              if (prepTokenRef.current === token) {
                setPrepFraction(f);
                setPrepDetail(`Preparing audio… ${Math.round(f * 100)}%`);
              }
            },
          }));
        if (prepTokenRef.current !== token) return;
        setManifest(m);
        setPrepState("ready");
        setPrepFraction(1);
        setPrepDetail("Ready to upload");
        say(
          cached
            ? `Reused prepared audio for “${title || "untitled"}” (${fmtMiB(m.totalBytes)}).`
            : `Prepared “${title || "untitled"}” · ${fmtDur(m.durationSeconds)} · ${fmtMiB(m.totalBytes)}.`,
          "success",
        );
      } catch (e) {
        if (prepTokenRef.current !== token) return;
        setPrepState("error");
        setSong(null);
        setManifest(null);
        setPrepFraction(null);
        const msg = e instanceof Error ? e.message : String(e);
        setPrepDetail(`Preparation failed: ${msg}`);
        say(`Preparation failed: ${msg}`, "error");
      }
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [allFour, decoded, timing, title]);

  /* ------------------------------------------------------------ step 3 */

  const requiredSectors = song ? sectorsForFrames(song.frames) : 0;
  const capacity = assessCapacity({
    requiredSectors,
    availableSectors: connected ? sectorsPerSong : null,
    queryState: connected ? queryState : queryState === "pending" ? "pending" : "none",
  });

  const canUpload = uploadEnabled({
    deviceConnected: connected,
    capabilitiesNegotiated: verdict.writable,
    bulkCapable,
    capacity: capacity.status,
    songPrepared: prepState === "ready" && !!song,
    transferActive: uploading || connecting,
  });

  const startUpload = useCallback(async () => {
    const t = transportRef.current;
    if (!t || !song || !manifest) return;
    abortRef.current = { aborted: false };
    setUploading(true);
    setResult(null);
    const total = manifest.totalBytes;
    const sectorsTotal = sectorsForFrames(song.frames);
    const started = performance.now();
    setProgress({
      fraction: 0,
      verifiedBytes: 0,
      total,
      sectors: 0,
      sectorsTotal,
      rate: 0,
      eta: NaN,
      retries: 0,
      checkpoint: "starting",
    });
    say(
      `Upload started · “${title || "untitled"}” · ${fmtMiB(total)} · ${sectorsTotal} sectors · fast verified-sector transfer.`,
    );
    let lastStage = "";
    let lastCheckpoint = 0;
    let lastRetries = 0;
    try {
      const out = await t.uploadSong({
        song,
        signal: abortRef.current,
        onProgress: (p) => {
          // Progress only ever advances on sectors the device itself confirmed.
          const verifiedBytes = p.bytesDone ?? Math.round(p.fraction * total);
          const elapsed = (performance.now() - started) / 1000;
          const rate = elapsed > 0 ? verifiedBytes / elapsed : 0;
          const sectors = p.sectorsDone ?? 0;
          setProgress({
            fraction: p.fraction,
            verifiedBytes,
            total,
            sectors,
            sectorsTotal: p.sectorsTotal ?? sectorsTotal,
            rate,
            eta: rate > 0 ? (total - verifiedBytes) / rate : NaN,
            retries: p.retries ?? 0,
            checkpoint: p.detail,
          });
          if (p.stage !== lastStage) {
            lastStage = p.stage;
            if (p.stage === "capacity" || p.stage === "metadata" || p.stage === "committing" || p.stage === "confirming") {
              say(p.detail);
            }
          }
          if (sectors && (sectors - lastCheckpoint >= 64 || sectors === (p.sectorsTotal ?? sectorsTotal))) {
            lastCheckpoint = sectors;
            say(
              `Verified ${sectors}/${p.sectorsTotal ?? sectorsTotal} sectors · ${fmtMiB(verifiedBytes)} confirmed from SP-1 storage.`,
            );
          }
          if ((p.retries ?? 0) > lastRetries) {
            lastRetries = p.retries ?? 0;
            say(`Retry ${lastRetries} — the identical transaction was resent.`, "warning");
          }
        },
      });
      setResult(out);
      for (const tx of t.bulkTransactions.filter((x) => x.status !== 0)) {
        say(
          `Device response · sector ${tx.seq} · block ${tx.destBlock} · ${tx.statusText}${
            tx.status >= 0 ? ` (code ${tx.status}, retryable ${tx.retryable ? "yes" : "no"})` : ""
          }`,
          "error",
        );
      }
      if (out.ok) {
        const secs = out.elapsedMs / 1000;
        say(
          `Song slot ${out.targetSongSlot === 0 ? "A" : "B"} / index slot ${out.targetIndexSlot === 0 ? "A" : "B"} · generation ${out.previousGeneration} → ${out.generation} · validity magic written and flushed.`,
          "success",
        );
        say(
          `Uploaded and verified in ${secs.toFixed(1)}s · ${(total / 1048576 / Math.max(secs, 0.001)).toFixed(2)} MiB/s verified payload · ${out.retries} retries.`,
          "success",
        );
      } else if (out.outcome === "unknown") {
        say(`Outcome unknown — ${out.detail}`, "warning");
      } else {
        say(`Upload stopped — ${out.detail}`, "error");
      }
    } catch (e) {
      say(e instanceof Error ? e.message : String(e), "error");
    } finally {
      // Snapshot regardless of how the attempt ended (success, a clean
      // failure response, or a thrown transport error) -- `t` is still the
      // exact instance that ran this attempt even if transportRef.current
      // has since moved on to a reconnect. See lastBulkTransactionsRef's
      // own doc comment above for why this can't just be read live at
      // download time.
      lastBulkTransactionsRef.current = t.bulkTransactions;
      setUploading(false);
    }
  }, [manifest, say, song, title]);

  const replaceSong = useCallback(() => {
    setResult(null);
    setProgress(null);
    setFiles({});
    setDecoded({});
    setSong(null);
    setManifest(null);
    setTiming(null);
    setPrepState("idle");
    setPrepDetail("");
    setTitleTouched(false);
    setTitle("");
  }, []);

  /* ------------------------------------------------------------ activity */

  const copyActivity = useCallback(() => {
    void navigator.clipboard?.writeText(
      log.map((e) => `${e.at}  ${e.level.toUpperCase()}  ${e.text}`).join("\n"),
    );
  }, [log]);

  const downloadReport = useCallback(() => {
    const report = {
      generatedAt: new Date().toISOString(),
      platform: typeof navigator !== "undefined" ? navigator.userAgent : "unknown",
      mode: mockMode ? "mock" : "physical",
      capability: {
        writable: verdict.writable,
        summary: verdict.summary,
        requirements: verdict.requirements,
        queryState,
      },
      device: { sectorsPerSong, bulkVerifiedSectorUpload: bulkCapable },
      // Prefer the live connection's own records when it has any (the
      // common case: report downloaded right after an attempt, same
      // instance still connected); fall back to the last completed
      // attempt's snapshot otherwise, since a reconnect between that
      // attempt and this download replaces transportRef.current with a
      // fresh, empty-by-construction StemTapeTransport. See
      // lastBulkTransactionsRef's own doc comment for why this matters.
      bulkTransactions:
        transportRef.current?.bulkTransactions?.length
          ? transportRef.current.bulkTransactions
          : lastBulkTransactionsRef.current,
      song: manifest,
      timing,
      capacity,
      result,
      events: log,
    };
    const url = URL.createObjectURL(
      new Blob([JSON.stringify(report, null, 2)], { type: "application/json" }),
    );
    const a = document.createElement("a");
    a.href = url;
    a.download = `stem-tape-diagnostic-${Date.now()}.json`;
    a.click();
    URL.revokeObjectURL(url);
  }, [bulkCapable, capacity, log, manifest, mockMode, queryState, result, sectorsPerSong, timing, verdict]);

  const stemRows = useMemo(
    () => STEM_ORDER.map((name) => ({ name, file: files[name], buf: decoded[name] })),
    [decoded, files],
  );

  const incompatible = connected && !verdict.writable;

  return (
    <div className="min-h-screen">
      <header className="border-b border-[var(--bench-line)]">
        <div className="flex items-start justify-between gap-4 px-4 pt-3 md:px-8">
          <p className="font-mono text-[9px] uppercase leading-relaxed tracking-[0.24em] text-[var(--ink-faint)] md:text-[10px]">
            not affiliated with
            <br />
            teenage engineering
          </p>
          <div className="flex shrink-0 items-center gap-4">
            <nav className="hidden items-center gap-6 lg:flex" aria-label="Site">
              <Link to="/" className="st-tab">
                instrument
              </Link>
              <Link to="/shop" className="st-tab">
                shop
              </Link>
              <Link to="/about" className="st-tab">
                about
              </Link>
              <span className="st-tab" data-on>
                uploader
              </span>
            </nav>
            <SupportButton />
          </div>
        </div>
        <div className="flex flex-wrap items-center gap-x-8 gap-y-3 px-4 pb-5 pt-3 md:px-8">
          <Link to="/" className="flex items-center gap-4">
            <img src={stemTapeLogo.url} alt="Stem Tape logo" width={52} height={52} className="h-[52px] w-[52px] object-contain" />
            <div>
              <h1 className="font-mono text-[28px] leading-none tracking-tight text-[var(--ink)]">
                stem tape uploader
              </h1>
              <p className="mt-2 font-mono text-[12px] tracking-[0.08em] text-[var(--ink-dim)]">
                connect · load stems · upload
              </p>
            </div>
          </Link>
        </div>
      </header>

      <main className="mx-auto grid w-full max-w-[760px] gap-5 px-4 pb-24 pt-6 md:px-8">
        {supported === false && (
          <section className="up-card" data-testid="unsupported">
            <div className="flex gap-4">
              <Info size={22} strokeWidth={1.2} className="mt-0.5 shrink-0 text-[var(--ink-dim)]" />
              <div>
                <p className="font-mono text-[13px] uppercase tracking-[0.18em] text-[var(--ink)]">
                  browser limitation
                </p>
                <p className="mt-2 font-mono text-[12px] leading-relaxed text-[var(--ink-dim)]">
                  This browser cannot talk to the SP-1.
                  <br />
                  Uploading needs Chrome or Edge on a desktop computer,
                  <br />
                  or a Chromium browser on Android.
                  <br />
                  iPhone and iPad cannot transfer songs.
                </p>
              </div>
            </div>
          </section>
        )}

        {mockMode && (
          <p
            className="up-card font-mono text-[11px] uppercase tracking-[0.16em] text-[var(--ink)]"
            data-testid="simulated-badge"
          >
            simulated device — nothing is written to hardware
          </p>
        )}

        {/* 1 — connect */}
        <section className="up-card" data-testid="step-connect">
          <div className="up-card__head">
            <span className="up-card__num">1</span>
            <span className="up-card__title">connect sp-1</span>
            <span className="up-state" data-on={connected}>
              <i />
              {connected ? "connected" : "not connected"}
            </span>
          </div>

          <div className="grid items-end gap-4 sm:grid-cols-[minmax(0,1fr)_minmax(0,300px)]">
            <div>
              {!connected ? (
                <>
                  <p className="font-mono text-[13px] text-[var(--ink-dim)]" data-testid="status">
                    Plug the SP-1 in over USB.
                  </p>
                  <button
                    className="st-btn st-btn--primary mt-8 flex w-full items-center justify-center gap-4 py-4 sm:w-auto sm:px-10"
                    data-testid="connect"
                    disabled={connecting}
                    onClick={() => void connect()}
                  >
                    <Usb size={16} strokeWidth={1.4} />
                    {connecting ? "Connecting…" : "Connect SP-1"}
                  </button>
                </>
              ) : (
                <div className="flex flex-wrap items-center gap-3">
                  <p className="font-mono text-[14px] text-[var(--ink)]" data-testid="status">
                    Stem Tape SP-1 connected
                  </p>
                  <button
                    className="st-btn st-btn--quiet"
                    data-testid="disconnect"
                    onClick={() => void disconnect()}
                  >
                    Disconnect
                  </button>
                </div>
              )}
              {connected && (incompatible || !bulkCapable) && (
                <p
                  className="mt-3 font-mono text-[12px] leading-relaxed text-[var(--ink-dim)]"
                  data-testid="incompatible"
                >
                  Update the Stem Tape firmware to use fast song upload.
                </p>
              )}
            </div>
            <div className="relative hidden w-full sm:block">
              <img
                src={sp1Outline}
                alt="Line illustration of the SP-1"
                className="w-full select-none"
                draggable={false}
              />
              <Sp1ConnectLeds connected={connected} />
            </div>

          </div>
        </section>

        {/* 2 — load stems */}
        <section
          className="up-card"
          data-testid="step-stems"
          onDragOver={(e) => e.preventDefault()}
          onDrop={(e) => {
            e.preventDefault();
            acceptFiles([...e.dataTransfer.files]);
          }}
        >
          <div className="up-card__head">
            <span className="up-card__num">2</span>
            <span className="up-card__title">load stems</span>
          </div>

          <p className="font-mono text-[12px] leading-relaxed text-[var(--ink-dim)]">
            Select all four stem files, or drag them here.
            <br />
            Accepted formats: WAV (44.1 kHz / 16-bit or 24-bit)
          </p>

          <input
            ref={fileInputRef}
            type="file"
            multiple
            accept=".wav,.mp3,.flac,.aif,.aiff,audio/*"
            className="sr-only"
            data-testid="file-input"
            tabIndex={-1}
            aria-hidden
            onChange={(e) => {
              acceptFiles([...(e.target.files ?? [])]);
              e.target.value = "";
            }}
          />

          <div className="up-drop mt-4">
            <span className="flex min-w-0 items-center gap-3">
              <Download size={16} strokeWidth={1.4} className="shrink-0" />
              <span className="truncate">drag &amp; drop stem files here</span>
            </span>
            <button
              type="button"
              className="st-btn shrink-0"
              data-testid="choose-files"
              onClick={() => fileInputRef.current?.click()}
            >
              Choose files
            </button>
          </div>

          <div className="mt-4 grid gap-2">
            {stemRows.map(({ name, file, buf }, i) => (
              <div key={name} className="up-row" data-testid={`stem-${name}`}>
                <span className="up-row__n">{String(i + 1).padStart(2, "0")}</span>
                <span className="up-row__role">{STEM_LABEL[name]}</span>
                <span className="up-row__meta">
                  {file ? file.name : "no file"} ·{" "}
                  <span data-testid={`state-${name}`}>
                    {!file
                      ? "waiting"
                      : !buf
                        ? "reading"
                        : prepState === "ready"
                          ? `ready · ${fmtDur(buf.duration)}`
                          : prepState === "working"
                            ? "preparing"
                            : `loaded · ${fmtDur(buf.duration)}`}
                  </span>
                </span>
                <button
                  className="st-btn st-btn--quiet"
                  onClick={() => {
                    replaceRef.current = name;
                    fileInputRef.current?.click();
                  }}
                >
                  {file ? "Replace file" : "Choose file"}
                </button>
                <button
                  className="st-btn st-btn--quiet"
                  aria-label={`Remove ${STEM_LABEL[name]}`}
                  disabled={!file}
                  onClick={() => removeStem(name)}
                >
                  <X size={14} strokeWidth={1.5} />
                </button>
              </div>
            ))}
          </div>

          {pending.length > 0 && (
            <div className="mt-3 grid gap-2" data-testid="ambiguous">
              {pending.map((f, i) => (
                <div
                  key={`${f.name}-${i}`}
                  className="flex flex-wrap items-center gap-3 border border-[var(--bench-line)] px-3 py-2 font-mono text-[12px]"
                >
                  <span className="min-w-0 flex-1 truncate text-[var(--ink-dim)]">{f.name}</span>
                  <label className="flex items-center gap-2">
                    <span className="text-[var(--ink-faint)]">is</span>
                    <select
                      className="border border-[var(--bench-line)] bg-transparent px-2 py-1"
                      defaultValue=""
                      onChange={(e) => {
                        const role = e.target.value as StemSlotName;
                        if (!role) return;
                        void decodeInto(role, f);
                        setPending((p) => p.filter((x) => x !== f));
                      }}
                    >
                      <option value="">choose…</option>
                      {STEM_ORDER.map((r) => (
                        <option key={r} value={r}>
                          {STEM_LABEL[r]}
                        </option>
                      ))}
                    </select>
                  </label>
                </div>
              ))}
            </div>
          )}

          <p className="mt-5 font-mono text-[10px] uppercase tracking-[0.2em] text-[var(--ink-faint)]">
            song title
          </p>
          <div className="up-field mt-2">
            <input
              data-testid="title"
              value={title}
              maxLength={32}
              placeholder="untitled"
              onChange={(e) => {
                setTitle(e.target.value);
                setTitleTouched(true);
              }}
            />
            <span className="shrink-0 font-mono text-[11px] tabular-nums text-[var(--ink-faint)]">
              {title.length} / 32
            </span>
          </div>

          {timing && (
            <div className="mt-3 flex flex-wrap items-center gap-3 font-mono text-[12px]">
              <span className="text-[var(--ink)]" data-testid="tempo">
                {timingLabel(timing)}
              </span>
              <button
                className="st-btn st-btn--quiet"
                data-testid="edit-timing"
                onClick={() => setEditTiming((v) => !v)}
              >
                {editTiming ? "Done" : "Edit"}
              </button>
            </div>
          )}
          {timing && editTiming && (
            <div
              className="mt-2 flex flex-wrap items-center gap-3 font-mono text-[12px]"
              data-testid="timing-editor"
            >
              <label className="flex items-center gap-2">
                <span className="text-[var(--ink-dim)]">BPM</span>
                <input
                  className="w-20 border border-[var(--bench-line)] bg-transparent px-2 py-1"
                  data-testid="bpm"
                  value={String(timing.bpm)}
                  onChange={(e) => {
                    const v = Number(e.target.value);
                    if (Number.isFinite(v) && v > 0) setTiming({ ...timing, bpm: v, edited: true });
                  }}
                />
              </label>
              <label className="flex items-center gap-2">
                <span className="text-[var(--ink-dim)]">first beat</span>
                <input
                  className="w-24 border border-[var(--bench-line)] bg-transparent px-2 py-1"
                  data-testid="first-beat"
                  value={String(timing.downbeatSeconds)}
                  onChange={(e) => {
                    const v = Number(e.target.value);
                    if (Number.isFinite(v) && v >= 0)
                      setTiming({ ...timing, downbeatSeconds: v, edited: true });
                  }}
                />
                <span className="text-[var(--ink-faint)]">s</span>
              </label>
              <button
                className="st-btn st-btn--quiet"
                data-testid="reset-timing"
                onClick={() => {
                  const t = analyzeTiming(
                    STEM_ORDER.filter((n) => decoded[n]).map((name) => {
                      const buf = decoded[name]!;
                      return { name, channel: buf.getChannelData(0), sampleRate: buf.sampleRate };
                    }),
                  );
                  setTiming(t);
                }}
              >
                Reset to automatic
              </button>
            </div>
          )}

          {(busyPhase || prepState !== "idle") && (
            <div className="mt-4" data-testid="prep-status">
              <div className="flex items-center gap-3 font-mono text-[12px] text-[var(--ink-dim)]">
                {(busyPhase || prepState === "working") && (
                  <Loader2
                    size={14}
                    strokeWidth={1.6}
                    className="shrink-0 animate-spin text-[var(--ink)]"
                    data-testid="prep-spinner"
                  />
                )}
                <span className="min-w-0 flex-1 truncate">
                  {busyPhase ?? prepDetail}
                </span>
                {prepState === "working" && prepFraction !== null && (
                  <span className="shrink-0 tabular-nums text-[var(--ink)]">
                    {Math.round(prepFraction * 100)}%
                  </span>
                )}
              </div>
              {(busyPhase || prepState === "working") && (
                <div className="mt-2 h-[3px] w-full overflow-hidden bg-[var(--bench-line)]">
                  <div
                    className={`h-full bg-[var(--ink)] transition-[width] duration-200 ${
                      busyPhase || prepFraction === null ? "animate-pulse" : ""
                    }`}
                    style={{
                      width: busyPhase ? "100%" : `${Math.max(4, (prepFraction ?? 0) * 100)}%`,
                    }}
                  />
                </div>
              )}
            </div>
          )}
        </section>

        {/* 3 — upload */}
        <section className="up-card" data-testid="step-upload">
          <div className="up-card__head">
            <span className="up-card__num">3</span>
            <span className="up-card__title">upload</span>
          </div>

          {!result && !uploading && (
            <p className="font-mono text-[12px] text-[var(--ink-dim)]" data-testid="no-song">
              No song has been written.
            </p>
          )}

          {result?.ok ? (
            <div data-testid="success">
              <p className="font-mono text-[16px] text-[var(--ink)]">{title || "Untitled song"}</p>
              <p className="mt-1 font-mono text-[14px] text-[var(--ink)]" data-testid="ready-line">
                Uploaded and verified. Press Play on your SP-1.
              </p>
              <p className="mt-1 font-mono text-[12px] text-[var(--ink-dim)]">
                Generation {result.generation} · song slot {result.targetSongSlot === 0 ? "A" : "B"}{" "}
                · index slot {result.targetIndexSlot === 0 ? "A" : "B"} · {result.sectorCount}{" "}
                sectors verified from SP-1 storage.
              </p>
              <button className="st-btn mt-3" data-testid="replace-song" onClick={replaceSong}>
                Replace song
              </button>
            </div>
          ) : (
            <>
              <div className="flex flex-wrap items-center justify-between gap-4">
                <div className="min-w-0">
                  {!canUpload && !uploading && (
                    <p
                      className="font-mono text-[12px] text-[var(--ink-dim)]"
                      data-testid="upload-hint"
                    >
                      {!connected
                        ? "Connect your SP-1 to upload."
                        : incompatible || !bulkCapable
                          ? "Update the Stem Tape firmware to use fast song upload."
                          : !allFour
                            ? "Load all four stems."
                            : prepState !== "ready"
                              ? "Preparing audio…"
                              : capacity.status === "insufficient"
                                ? "This song is too long for the space on your SP-1."
                                : "Checking your SP-1…"}
                    </p>
                  )}
                  {uploading && (
                    <button
                      className="st-btn st-btn--quiet"
                      data-testid="cancel"
                      onClick={() => {
                        abortRef.current.aborted = true;
                      }}
                    >
                      Cancel
                    </button>
                  )}
                </div>
                <button
                  className="st-btn st-btn--primary flex items-center justify-center gap-4 px-8 py-5"
                  data-testid="upload"
                  disabled={!canUpload}
                  onClick={() => void startUpload()}
                >
                  <Upload size={16} strokeWidth={1.4} />
                  Upload to SP-1
                </button>
              </div>

              {progress && (
                <div className="mt-4" data-testid="progress">
                  <div className="h-[6px] w-full bg-[var(--bench-line)]">
                    <div
                      className="h-full bg-[var(--ink)]"
                      style={{ width: `${Math.round(progress.fraction * 100)}%` }}
                    />
                  </div>
                  <p className="mt-2 font-mono text-[13px] text-[var(--ink)]" data-testid="progress-title">
                    {title || "Untitled song"}
                  </p>
                  <p className="mt-1 font-mono text-[12px] leading-relaxed text-[var(--ink-dim)]">
                    {Math.round(progress.fraction * 100)}% · {fmtMiB(progress.verifiedBytes)} /{" "}
                    {fmtMiB(progress.total)} verified · {progress.sectors}/{progress.sectorsTotal}{" "}
                    sectors verified
                    <br />
                    {(progress.rate / 1048576).toFixed(2)} MiB/s · {fmtClock(progress.eta)} left ·{" "}
                    {progress.retries} {progress.retries === 1 ? "retry" : "retries"}
                    <br />
                    {progress.checkpoint}
                  </p>
                  <p className="mt-2 font-mono text-[12px] text-[var(--ink)]" data-testid="uploading-copy">
                    Uploading and verifying on SP-1. Keep it connected.
                  </p>
                </div>
              )}

              {result && !result.ok && (
                <div className="mt-3" data-testid="upload-error">
                  <p className="font-mono text-[14px] text-[var(--ink)]">
                    {result.outcome === "unknown"
                      ? "Reconnect the SP-1 to confirm whether the new song committed."
                      : "Upload stopped. Your previous song is still active."}
                  </p>
                  <p className="mt-1 font-mono text-[12px] leading-relaxed text-[var(--ink-dim)]">
                    {result.detail}
                  </p>
                  <button className="st-btn mt-3" onClick={downloadReport}>
                    Download diagnostic report
                  </button>
                </div>
              )}
            </>
          )}
        </section>

        {/* activity */}
        <section className="up-card" data-testid="activity">
          <div className="mb-4 flex items-center justify-between gap-4">
            <span className="font-mono text-[13px] uppercase tracking-[0.18em] text-[var(--ink)]">
              activity
            </span>
            <span className="font-mono text-[11px] text-[var(--ink-faint)]">
              {log.length === 0 ? "No recent activity" : `${log.length} events`}
            </span>
          </div>
          <div className="grid gap-2">
            <button className="up-act" data-testid="copy-activity" onClick={copyActivity}>
              <ClipboardCopy size={15} strokeWidth={1.4} />
              copy activity
            </button>
            <button className="up-act" data-testid="download-report" onClick={downloadReport}>
              <Download size={15} strokeWidth={1.4} />
              download diagnostic report
            </button>
            <button className="up-act" data-testid="clear-activity" onClick={() => setLog([])}>
              <Trash2 size={15} strokeWidth={1.4} />
              clear activity
            </button>
          </div>
          {log.length > 0 && (
            <ul
              className="mt-3 max-h-[220px] overflow-auto font-mono text-[11px] text-[var(--ink-dim)]"
              data-testid="log"
            >
              {log.map((e, i) => (
                <li key={i} data-level={e.level}>
                  {e.at} · {e.level} · {e.text}
                </li>
              ))}
            </ul>
          )}
        </section>
      </main>
    </div>
  );
}

