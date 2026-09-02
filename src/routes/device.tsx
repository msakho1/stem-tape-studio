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
  Check,
  ClipboardCopy,
  Download,
  Info,
  Loader2,
  Music4,
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
import {
  parseCapabilities,
  readOnlyVerdict,
  type CompatibilityVerdict,
  type StemTapeCapabilities,
} from "@/sp1/compatibility";
import type { LibraryState } from "@/sp1/activeIndex";
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
import {
  APP_FORMAT,
  PHASE_DETAIL,
  PHASE_LABEL,
  UPLOAD_PHASES,
  classifyFailure,
  deviceInfoFrom,
  existingSongIntact,
  failureCopy,
  formatVersion,
  phaseIndex,
  songsFrom,
  storageFrom,
  uploadStateFor,
  type DeviceConnection,
  type UploadFailure,
  type UploadState,
} from "@/device/deviceModel";

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
  const [caps, setCaps] = useState<StemTapeCapabilities | null>(null);
  const [library, setLibrary] = useState<LibraryState | null>(null);
  const [readingDevice, setReadingDevice] = useState(false);
  const [settingUp, setSettingUp] = useState(false);
  const [setupDone, setSetupDone] = useState(false);
  const [setupError, setSetupError] = useState<string | null>(null);
  const [confirmSetup, setConfirmSetup] = useState(false);
  const [upload, setUpload] = useState<UploadState>({ phase: "idle" });



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
      const parsed = rawCaps ? parseCapabilities(rawCaps) : null;
      const t = new StemTapeTransport(session, parsed, { kind: injected ? "mock" : "physical" });
      transportRef.current = t;
      setCaps(parsed);
      setVerdict(t.verdict);
      setQueryState(t.verdict.writable ? "compatible" : "unverified");
      setMockMode(!!injected);
      setReadingDevice(true);
      await t.readIndex();
      setLibrary(t.library);
      setReadingDevice(false);
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
      setCaps(null);
      setLibrary(null);
      setReadingDevice(false);
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
    setCaps(null);
    setLibrary(null);
    setUpload({ phase: "idle" });
    say("Disconnected. The SP-1 has resumed normal operation.");
  }, [say]);


  /* ---- set up (explicit initialization) -------------------------------- */

  const setUpDevice = useCallback(async () => {
    const t = transportRef.current;
    if (!t) return;
    setSettingUp(true);
    setSetupError(null);
    say("Setting up this SP-1: writing a fresh index record to index A…");
    try {
      const { library: lib, reportedGeneration, confirmed } = await t.setUpLibrary();
      setLibrary(lib);
      setSetupDone(true);
      say(
        confirmed
          ? `Set up. The SP-1 now reports generation ${reportedGeneration}. The next upload will commit at generation 2.`
          : `Index written and read back at generation ${lib.generation}. The capability reply reported ${
              reportedGeneration === null ? "no generation" : `generation ${reportedGeneration}`
            }.`,
        confirmed ? "success" : "warning",
      );
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      setSetupError(msg);
      say(`Setting up did not complete: ${msg}`, "error");
    } finally {
      setSettingUp(false);
    }
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

  /* ------------------------------------------------- derived device state */

  const songs = useMemo(() => songsFrom(library), [library]);
  const storage = useMemo(() => storageFrom(caps, songs), [caps, songs]);
  const activeSongId = songs.find((s) => s.isActive)?.id ?? null;

  const connection: DeviceConnection = useMemo(() => {
    if (connecting) return { status: "connecting" };
    if (!connected) return { status: "disconnected" };
    if (!caps || !verdict.writable || !bulkCapable)
      return {
        status: "incompatible",
        deviceFormat: caps ? formatVersion(caps.formatMajor, caps.formatMinor) : "unknown",
        appFormat: APP_FORMAT,
      };
    return { status: "ready", device: deviceInfoFrom(caps) };
  }, [bulkCapable, caps, connected, connecting, verdict.writable]);

  const device = connection.status === "ready" ? connection.device : null;
  /* Every multi-song control is gated on the firmware's own capability
   * record, never on the version number: when a firmware reports
   * deleteSong/reorderSongs, this same build starts showing them. */
  const canDelete = device?.capabilities.deleteSong ?? false;
  const canReorder = device?.capabilities.reorderSongs ?? false;
  const canAddAnother = device?.capabilities.multiSong ?? false;

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
    setUpload({ phase: "preparing" });
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
          setUpload(
            uploadStateFor({
              stage: p.stage,
              sectorsSent: sectors,
              sectorsTotal: p.sectorsTotal ?? sectorsTotal,
            }),
          );
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
        setUpload({ phase: "done" });
        say(
          `Song slot ${out.targetSongSlot === 0 ? "A" : "B"} / index slot ${out.targetIndexSlot === 0 ? "A" : "B"} · generation ${out.previousGeneration} → ${out.generation} · validity magic written and flushed.`,
          "success",
        );
        say(
          `Uploaded and verified in ${secs.toFixed(1)}s · ${(total / 1048576 / Math.max(secs, 0.001)).toFixed(2)} MiB/s verified payload · ${out.retries} retries.`,
          "success",
        );
        // reload — the screen always shows what the device actually holds
        try {
          setReadingDevice(true);
          await t.readIndex();
          setLibrary(t.library);
        } finally {
          setReadingDevice(false);
        }
      } else {
        const sectorsSent = t.bulkTransactions.filter((x) => x.status === 0).length;
        setUpload({
          phase: "failed",
          reason: classifyFailure({
            outcome: out.outcome,
            detail: out.detail,
            sectorsSent,
            sectorsTotal,
          }),
          existingSongIntact: existingSongIntact(out.outcome),
        });
        if (out.outcome === "unknown") say(`Outcome unknown — ${out.detail}`, "warning");
        else say(`Upload stopped — ${out.detail}`, "error");
      }
    } catch (e) {
      const detail = e instanceof Error ? e.message : String(e);
      setUpload({
        phase: "failed",
        reason: classifyFailure({
          outcome: "failed",
          detail,
          sectorsSent: t.bulkTransactions.filter((x) => x.status === 0).length,
          sectorsTotal,
        }),
        existingSongIntact: true,
      });
      say(detail, "error");
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
    setUpload({ phase: "idle" });
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
              <Link to="/firmware" className="st-tab">
                firmware
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
        <div className="flex flex-wrap items-center gap-x-8 gap-y-3 px-4 pb-3 pt-2 md:px-8 md:pb-5 md:pt-3">
          <Link to="/" className="flex items-center gap-3 md:gap-4">
            <img src={stemTapeLogo.url} alt="Stem Tape logo" width={120} height={120} className="h-16 w-16 object-contain md:h-[120px] md:w-[120px]" />
            <div>
              <h1 className="font-mono text-[28px] leading-none tracking-tight text-[var(--ink)]">
                stem tape uploader
              </h1>
              <p className="mt-2 font-mono text-[12px] tracking-[0.08em] text-[var(--ink-dim)]">
                connect · load stems · upload
              </p>
            </div>
          </Link>
          <nav className="flex w-full items-center gap-6 lg:hidden" aria-label="Site">
            <Link to="/" className="st-tab">
              instrument
            </Link>
            <Link to="/firmware" className="st-tab">
              firmware
            </Link>
            <Link to="/about" className="st-tab">
              about
            </Link>
            <span className="st-tab" data-on>
              uploader
            </span>
          </nav>
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

        {/* 1 — connection */}
        <section className="up-card" data-testid="step-connect">
          <div className="up-card__head">
            <span className="up-card__num">1</span>
            <span className="up-card__title">connect sp-1</span>
            <span
              className="up-state"
              data-on={connection.status === "ready"}
              data-testid="connection-state"
            >
              <i />
              {connection.status === "ready"
                ? "connected"
                : connection.status === "connecting"
                  ? "connecting"
                  : connection.status === "incompatible"
                    ? "needs update"
                    : "not connected"}
            </span>
          </div>

          <div className="grid items-end gap-4 sm:grid-cols-[minmax(0,1fr)_minmax(0,300px)]">
            <div>
              {connection.status === "disconnected" && (
                <>
                  <p className="font-mono text-[13px] text-[var(--ink-dim)]" data-testid="status">
                    No SP-1 found. Plug it in over USB, then connect.
                  </p>
                  <button
                    className="st-btn st-btn--primary mt-8 flex w-full items-center justify-center gap-4 py-4 sm:w-auto sm:px-10"
                    data-testid="connect"
                    onClick={() => void connect()}
                  >
                    <Usb size={16} strokeWidth={1.4} />
                    Connect SP-1
                  </button>
                </>
              )}

              {connection.status === "connecting" && (
                <p
                  className="flex items-center gap-3 font-mono text-[13px] text-[var(--ink)]"
                  data-testid="status"
                >
                  <Loader2 size={14} strokeWidth={1.6} className="animate-spin" />
                  Connecting to the SP-1…
                </p>
              )}

              {connection.status === "incompatible" && (
                <div data-testid="incompatible">
                  <p className="font-mono text-[14px] text-[var(--ink)]" data-testid="status">
                    This SP-1 answered, but it speaks a different song format.
                  </p>
                  <p className="mt-2 font-mono text-[12px] leading-relaxed text-[var(--ink-dim)]">
                    Your SP-1 stores songs in format {connection.deviceFormat}; this app writes
                    format {connection.appFormat}. Update the firmware on your SP-1 to upload songs.
                    Nothing on it will be changed in the meantime.
                  </p>
                  <button
                    className="st-btn st-btn--quiet mt-3"
                    data-testid="disconnect"
                    onClick={() => void disconnect()}
                  >
                    Disconnect
                  </button>
                </div>
              )}

              {connection.status === "ready" && (
                <div>
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
                  <p
                    className="mt-2 font-mono text-[11px] text-[var(--ink-faint)]"
                    data-testid="firmware-line"
                  >
                    firmware {connection.device.format.major}.{connection.device.format.minor} ·{" "}
                    {connection.device.sampleRate / 1000} kHz
                  </p>
                  <p className="mt-2 font-mono text-[12px] text-[var(--ink-dim)]">
                    Playback is paused on the SP-1 while it is connected here.
                  </p>
                </div>
              )}
            </div>
            <div className="relative hidden w-full sm:block">
              <img
                src={sp1Outline}
                alt="Line illustration of the SP-1"
                className="w-full select-none"
                draggable={false}
              />
              <Sp1ConnectLeds connected={connection.status === "ready"} />
            </div>
          </div>
        </section>

        {/* what the SP-1 is holding */}
        <section className="up-card" data-testid="library">
          <div className="up-card__head">
            <span className="up-card__title">on your sp-1</span>
          </div>

          {connection.status !== "ready" ? (
            <p className="font-mono text-[12px] leading-relaxed text-[var(--ink-dim)]" data-testid="library-no-device">
              This is where the song stored on your SP-1 appears, with how much room is left.
              Connect the SP-1 to see it.
            </p>
          ) : readingDevice ? (
            <div className="grid gap-3" data-testid="library-skeleton" aria-busy>
              <div className="h-[52px] w-full animate-pulse bg-[var(--bench-line)]" />
              <div className="h-[3px] w-full animate-pulse bg-[var(--bench-line)]" />
              <div className="h-[14px] w-1/2 animate-pulse bg-[var(--bench-line)]" />
            </div>
          ) : library?.requiresInitialization ? (
            <div data-testid="library-needs-setup" data-kind={library.status}>
              <p className="font-mono text-[14px] text-[var(--ink)]">
                {library.status === "legacy"
                  ? "This SP-1 was set up by an earlier version."
                  : "This SP-1 hasn’t been set up yet."}
              </p>
              <p className="mt-1 font-mono text-[12px] leading-relaxed text-[var(--ink-dim)]">
                {library.status === "legacy"
                  ? "Setting it up again will clear the old song, which this firmware can no longer play."
                  : "Its index has never been written for this firmware, so there is nothing stored to lose. Setting it up takes a moment and prepares it for uploads."}
              </p>

              {setupError && (
                <p className="mt-3 font-mono text-[12px] text-[var(--ink)]" data-testid="setup-error">
                  Setting up did not complete: {setupError}. Nothing else on the SP-1 was changed —
                  you can try again.
                </p>
              )}

              {!confirmSetup ? (
                <button
                  className="st-btn mt-4"
                  data-testid="setup-device"
                  disabled={!verdict.writable || settingUp}
                  onClick={() => setConfirmSetup(true)}
                >
                  Set up this SP-1
                </button>
              ) : (
                <div className="mt-4" data-testid="setup-confirm">
                  <p className="font-mono text-[12px] leading-relaxed text-[var(--ink-dim)]">
                    {library.status === "legacy"
                      ? "Setting up clears the song already on this SP-1. That song can no longer be played by this firmware, but it will be gone for good."
                      : "Setting up writes a fresh, empty index. Anything already in the index area is cleared."}
                  </p>
                  <div className="mt-3 flex flex-wrap gap-3">
                    <button
                      className="st-btn st-btn--primary"
                      data-testid="setup-confirm-go"
                      disabled={settingUp}
                      onClick={() => void setUpDevice()}
                    >
                      {settingUp ? "Setting up…" : "Clear and set up"}
                    </button>
                    <button
                      className="st-btn st-btn--quiet"
                      data-testid="setup-cancel"
                      disabled={settingUp}
                      onClick={() => setConfirmSetup(false)}
                    >
                      Keep as is
                    </button>
                  </div>
                </div>
              )}
            </div>
          ) : (
            <>
              {setupDone && (
                <p className="mb-3 font-mono text-[12px] text-[var(--ink-dim)]" data-testid="setup-done">
                  This SP-1 is set up and ready for an upload.
                </p>
              )}
              {songs.length === 0 ? (
                <div data-testid="library-empty">
                  <p className="font-mono text-[14px] text-[var(--ink)]">No song on this SP-1 yet.</p>
                  <p className="mt-1 font-mono text-[12px] leading-relaxed text-[var(--ink-dim)]">
                    The SP-1 is working fine — it just hasn’t been given a song. Load four stems
                    below and upload; there is room for up to {fmtMiB(storage.maxSongBytes)}.
                  </p>
                </div>
              ) : (
                <ul className="grid gap-2" data-testid="song-list">
                  {songs.map((s) => (
                    <li
                      key={s.id}
                      className="flex items-center gap-4 border border-[var(--bench-line)] bg-[var(--bench-raised)] px-4 py-3"
                      data-testid="song-card"
                      data-active={s.id === activeSongId}
                    >
                      <Music4 size={18} strokeWidth={1.3} className="shrink-0 text-[var(--ink-dim)]" />
                      <div className="min-w-0 flex-1">
                        <p className="truncate font-mono text-[15px] text-[var(--ink)]">{s.title}</p>
                        <p className="mt-1 font-mono text-[11px] text-[var(--ink-faint)]">
                          {fmtDur(s.durationSeconds)} · {fmtMiB(s.sizeBytes)}
                          {s.artist ? ` · ${s.artist}` : ""}
                        </p>
                      </div>
                      {s.id === activeSongId && (
                        <span className="up-state" data-on>
                          <i />
                          playing on sp-1
                        </span>
                      )}
                      {/* delete / reorder appear only when the firmware reports them */}
                      {canDelete && <span data-testid="delete-song" />}
                      {canReorder && <span data-testid="reorder-song" />}
                    </li>
                  ))}
                </ul>
              )}

              <div className="mt-4" data-testid="storage">
                <div className="h-[6px] w-full bg-[var(--bench-line)]">
                  <div
                    className="h-full bg-[var(--ink)]"
                    style={{
                      width: `${
                        storage.capacityBytes
                          ? Math.min(100, Math.round((storage.usedBytes / storage.capacityBytes) * 100))
                          : 0
                      }%`,
                    }}
                  />
                </div>
                {/* On a one-song device free space cannot be spent, so the
                    headline leads with what is used. Multi-song firmware leads
                    with what is still available. */}
                <p className="mt-2 font-mono text-[12px] text-[var(--ink-dim)]" data-testid="storage-line">
                  {canAddAnother ? (
                    <>
                      <span className="text-[var(--ink)]">{fmtMiB(storage.freeBytes)} free</span> of{" "}
                      {fmtMiB(storage.capacityBytes)}
                    </>
                  ) : (
                    <>
                      <span className="text-[var(--ink)]">{fmtMiB(storage.usedBytes)}</span> of{" "}
                      {fmtMiB(storage.capacityBytes)} used
                    </>
                  )}
                </p>

              </div>

              {canAddAnother && <span data-testid="add-song" />}
            </>
          )}
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

          {upload.phase === "idle" && !result?.ok && (
            <p className="font-mono text-[12px] leading-relaxed text-[var(--ink-dim)]" data-testid="no-song">
              {canAddAnother ? (
                <>
                  Adding a song keeps the songs already on the SP-1.
                  {connection.status === "ready" && ` Up to ${fmtMiB(storage.freeBytes)}.`}
                </>
              ) : (
                <>
                  Uploading replaces the song on the SP-1. The song already on it keeps playing
                  until the new one is completely stored and checked.
                  {connection.status === "ready" &&
                    ` Replaces the current song. Up to ${fmtMiB(storage.maxSongBytes)}.`}
                </>
              )}
            </p>
          )}


          {result?.ok ? (
            <div data-testid="success">
              <p className="font-mono text-[16px] text-[var(--ink)]">{title || "Untitled song"}</p>
              <p className="mt-1 font-mono text-[14px] text-[var(--ink)]" data-testid="ready-line">
                Uploaded and verified. Press Play on your SP-1.
              </p>
              <p className="mt-1 font-mono text-[12px] text-[var(--ink-dim)]">
                {fmtMiB(result.bytesWritten)} stored and read back off the SP-1 to confirm it
                matches.
              </p>
              <button className="st-btn mt-3" data-testid="replace-song" onClick={replaceSong}>
                Upload a different song
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
                      {connection.status !== "ready"
                        ? connection.status === "incompatible"
                          ? "Update the firmware on your SP-1 to upload songs."
                          : "Connect your SP-1 to upload."
                        : !allFour
                          ? "Load all four stems."
                          : prepState !== "ready"
                            ? "Preparing audio…"
                            : capacity.status === "insufficient"
                              ? `This song needs more room than the ${fmtMiB(storage.maxSongBytes)} your SP-1 can hold.`
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

              {(uploading || upload.phase === "failed") && upload.phase !== "idle" && (
                <div className="mt-5" data-testid="phases">
                  <ol className="grid gap-2">
                    {UPLOAD_PHASES.filter((p) => p !== "done").map((p) => {
                      const current = upload.phase === p;
                      const done =
                        upload.phase !== "failed" && phaseIndex(upload.phase) > phaseIndex(p);
                      return (
                        <li
                          key={p}
                          className="flex items-start gap-3 font-mono text-[12px]"
                          data-testid={`phase-${p}`}
                          data-state={current ? "current" : done ? "done" : "todo"}
                        >
                          <span className="mt-[2px] w-[16px] shrink-0 text-[var(--ink)]">
                            {done ? (
                              <Check size={13} strokeWidth={1.8} />
                            ) : current ? (
                              <Loader2 size={13} strokeWidth={1.8} className="animate-spin" />
                            ) : (
                              <span className="block h-[5px] w-[5px] translate-y-[4px] bg-[var(--bench-line)]" />
                            )}
                          </span>
                          <span className="min-w-0">
                            <span className={current || done ? "text-[var(--ink)]" : "text-[var(--ink-faint)]"}>
                              {PHASE_LABEL[p]}
                            </span>
                            {current && (
                              <>
                                {p === "sending" && upload.phase === "sending" && (
                                  <>
                                    <span className="ml-2 tabular-nums text-[var(--ink-dim)]">
                                      {upload.sectorsSent.toLocaleString()} /{" "}
                                      {upload.sectorsTotal.toLocaleString()} chunks
                                    </span>
                                    <span className="mt-2 block h-[6px] w-full bg-[var(--bench-line)]">
                                      <span
                                        className="block h-full bg-[var(--ink)]"
                                        style={{
                                          width: `${
                                            upload.sectorsTotal
                                              ? Math.round(
                                                  (upload.sectorsSent / upload.sectorsTotal) * 100,
                                                )
                                              : 0
                                          }%`,
                                        }}
                                      />
                                    </span>
                                  </>
                                )}
                                {p !== "sending" && (
                                  <span className="mt-2 block h-[6px] w-full overflow-hidden bg-[var(--bench-line)]">
                                    <span className="block h-full w-1/3 animate-pulse bg-[var(--ink)]" />
                                  </span>
                                )}
                                <span className="mt-1 block leading-relaxed text-[var(--ink-dim)]">
                                  {PHASE_DETAIL[p]}
                                </span>
                              </>
                            )}
                          </span>
                        </li>
                      );
                    })}
                  </ol>
                  {uploading && (
                    <p className="mt-3 font-mono text-[12px] text-[var(--ink)]" data-testid="uploading-copy">
                      Keep the SP-1 connected. Playback is paused while transferring.
                    </p>
                  )}
                </div>
              )}

              {upload.phase === "failed" &&
                (() => {
                  const copy = failureCopy(upload.reason, upload.existingSongIntact);
                  return (
                    <div className="mt-4 border-l-2 border-[var(--ink)] pl-4" data-testid="upload-error">
                      <p className="font-mono text-[14px] leading-relaxed text-[var(--ink)]">
                        {copy.headline}
                      </p>
                      <p className="mt-2 font-mono text-[12px] leading-relaxed text-[var(--ink-dim)]">
                        {copy.body}
                      </p>
                      {copy.retryable && (
                        <button
                          className="st-btn mt-3"
                          data-testid="retry-upload"
                          disabled={!canUpload}
                          onClick={() => void startUpload()}
                        >
                          Try again
                        </button>
                      )}
                    </div>
                  );
                })()}
            </>
          )}
        </section>

        {/* advanced — collapsed, off the normal path */}
        <details className="up-card" data-testid="advanced">
          <summary className="cursor-pointer font-mono text-[13px] uppercase tracking-[0.18em] text-[var(--ink)]">
            advanced &amp; diagnostics
          </summary>

          <div className="mt-4 grid gap-5 font-mono text-[11px] leading-relaxed text-[var(--ink-dim)]">
            {caps ? (
              <>
                <div data-testid="adv-device">
                  <p className="text-[var(--ink)]">device</p>
                  <p>
                    firmware id 0x{(caps.firmwareId >>> 0).toString(16)} · protocol{" "}
                    {caps.protoMajor}.{caps.protoMinor} · format {caps.formatMajor}.
                    {caps.formatMinor} · STIX v{caps.stixVersion}
                  </p>
                  <p>
                    device blocks {caps.deviceBlocks} · song A {caps.song[0].start}+
                    {caps.song[0].blocks} · song B {caps.song[1].start}+{caps.song[1].blocks} ·
                    index A {caps.index[0].start}+{caps.index[0].blocks} · index B{" "}
                    {caps.index[1].start}+{caps.index[1].blocks}
                  </p>
                  <p>
                    active index slot{" "}
                    {library?.activeIndexSlot === null || library?.activeIndexSlot === undefined
                      ? "none"
                      : library.activeIndexSlot === 0
                        ? "A"
                        : "B"}{" "}
                    · active song slot{" "}
                    {library?.activeSongSlot === null || library?.activeSongSlot === undefined
                      ? "none"
                      : library.activeSongSlot === 0
                        ? "A"
                        : "B"}{" "}
                    · generation {library?.generation ?? 0}
                  </p>
                </div>

                <div data-testid="adv-slots">
                  <p className="text-[var(--ink)]">index slots</p>
                  {library?.slots.map((s) => (
                    <p key={s.slot}>
                      slot {s.slot === 0 ? "A" : "B"} · “{s.record.title || "—"}” · generation{" "}
                      {s.record.generation} · {s.validation.valid ? "valid" : "invalid"} (
                      {s.validation.reason})
                      {library.activeIndexSlot === s.slot ? " · active" : ""}
                      {s.validation.valid &&
                      library.activeIndexSlot !== s.slot &&
                      s.record.songPresent
                        ? " · rollback copy, not playable"
                        : ""}
                    </p>
                  ))}
                  {library && <p>{library.explanation}</p>}
                </div>
              </>
            ) : (
              <p data-testid="adv-nodevice">No device connected — nothing to report.</p>
            )}

            {upload.phase === "failed" && (
              <div data-testid="adv-failure">
                <p className="text-[var(--ink)]">last upload failure, verbatim</p>
                <pre className="mt-1 max-h-[160px] overflow-auto whitespace-pre-wrap break-words border border-[var(--bench-line)] p-2 text-[10px]">
                  {JSON.stringify({ reason: upload.reason, result }, null, 2)}
                </pre>
              </div>
            )}

            <div>
              <div className="mb-2 flex items-center justify-between gap-4">
                <span className="text-[var(--ink)]">activity</span>
                <span className="text-[var(--ink-faint)]">
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
                <ul className="mt-3 max-h-[220px] overflow-auto" data-testid="log">
                  {log.map((e, i) => (
                    <li key={i} data-level={e.level}>
                      {e.at} · {e.level} · {e.text}
                    </li>
                  ))}
                </ul>
              )}
            </div>
          </div>
        </details>

      </main>
    </div>
  );
}

