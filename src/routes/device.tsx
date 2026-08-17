/**
 * SP-1 companion uploader — local song-library transfer over Web Serial.
 *
 * Scope guard: this route is not an instrument, MIDI surface, LED simulator or
 * diagnostic dashboard. It connects, lists slots, prepares four stems locally
 * and (when a compatible device is negotiated) performs one transactional
 * upload.
 *
 * This route constructs NO command bytes, block addresses, index bytes or CRC
 * records. Everything device-specific lives behind StemTapeDeviceTransport.
 * Physical mutation is locked; only in-process mock ports can write.
 */
import { createFileRoute, Link } from "@tanstack/react-router";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { SupportButton } from "@/components/SupportButton";
import { sniffHeader } from "@/audio/format";
import { Sp1Transport, Sp1Session, BAUD_RATE, type SerialLikePort } from "@/sp1/protocol";
import { bpmFromTaps, STEM_ORDER, STEM_LABEL, type StemSlotName } from "@/sp1/prepare";
import { prepareCanonicalSong, type CanonicalSong } from "@/sp1/song";
import { parseCapabilities, readOnlyVerdict, type CompatibilityVerdict } from "@/sp1/compatibility";
import { StemTapeTransport, type DeviceSongSlot, type UploadProgress, type UploadResult } from "@/sp1/transport";
import { buildReceipt } from "@/sp1/receipt";
import {
  outcomeWording,
  resolveWording,
  simulatedRowWording,
  successLogWording,
  writeStateWording,
} from "@/sp1/wording";
import { sectorsForFrames, BLOCKS_PER_SECTOR, PHYSICAL_BLOCK_BYTES, SECTOR_BYTES, SAMPLE_RATE } from "@/sp1/stemTapeFormat";
import { sha256Hex } from "@/sp1/digest";
import { encodeSong } from "@/sp1/sector";

const STAGE_NAMES = ["connect sp-1", "add stems", "prepare song", "transfer", "verify"] as const;

export const Route = createFileRoute("/device")({
  component: DevicePage,
  head: () => ({
    meta: [
      { title: "SP-1 Uploader — Stem Tape" },
      {
        name: "description",
        content:
          "Prepare four stems for a Stem Tape SP-1 in your browser: no account, no cloud, no upload of your audio. Physical transfer is locked until the firmware contract is final.",
      },
      { property: "og:title", content: "SP-1 Uploader — Stem Tape" },
      {
        property: "og:description",
        content: "Connect the SP-1 over USB, prepare four stereo 24-bit stems locally, and inspect the song library.",
      },
      { property: "og:type", content: "website" },
      { name: "twitter:card", content: "summary" },
    ],
  }),
});

type Files = Partial<Record<StemSlotName, File>>;
type Decoded = Partial<Record<StemSlotName, AudioBuffer>>;

const fmtBytes = (n: number) => (n < 1024 * 1024 ? `${(n / 1024).toFixed(0)} KiB` : `${(n / 1048576).toFixed(1)} MiB`);
const fmtSecs = (s: number) => (s >= 1 ? `${s.toFixed(2)} s` : `${Math.round(s * 1000)} ms`);

function DevicePage() {
  const [supported, setSupported] = useState<boolean | null>(null);
  const [status, setStatus] = useState("Not connected");
  const [log, setLog] = useState<string[]>([]);
  const [songs, setSongs] = useState<DeviceSongSlot[] | null>(null);
  const [verdict, setVerdict] = useState<CompatibilityVerdict>(() => readOnlyVerdict());
  const [mockMode, setMockMode] = useState(false);
  const [description, setDescription] = useState<ReturnType<StemTapeTransport["describe"]> | null>(null);
  const [slot, setSlot] = useState(0);
  const [files, setFiles] = useState<Files>({});
  const [decoded, setDecoded] = useState<Decoded>({});
  const [song, setSong] = useState<CanonicalSong | null>(null);
  const [progress, setProgress] = useState<{ stage: string; fraction: number; detail: string } | null>(null);
  const [busy, setBusy] = useState(false);
  const [uninitialised, setUninitialised] = useState(false);
  const [result, setResult] = useState<UploadResult | null>(null);
  const [playbackConfirmed, setPlaybackConfirmed] = useState(false);
  const [songSha, setSongSha] = useState<string | null>(null);
  const [showTech, setShowTech] = useState(false);
  const [sourceRates, setSourceRates] = useState<Partial<Record<StemSlotName, number | null>>>({});
  const [title, setTitle] = useState("");
  const [artist, setArtist] = useState("");
  const [bpm, setBpm] = useState("");
  const [downbeat, setDownbeat] = useState("0");
  const transportRef = useRef<StemTapeTransport | null>(null);
  const abortRef = useRef({ aborted: false });
  const tapsRef = useRef<number[]>([]);
  const previewRef = useRef<HTMLAudioElement | null>(null);

  const say = useCallback((line: string) => {
    setLog((l) => [...l.slice(-60), `${new Date().toLocaleTimeString()}  ${line}`]);
  }, []);

  useEffect(() => {
    setSupported(typeof navigator !== "undefined" && !!(navigator as Navigator & { serial?: unknown }).serial);
  }, []);

  const refresh = useCallback(async () => {
    const t = transportRef.current;
    if (!t) return;
    setSongs(await t.listSongs());
    setDescription(t.describe());
    const bad = !t.indexInitialised;
    setUninitialised(bad);
    if (bad) say("This SP-1's song index is uninitialised or written by different firmware. Nothing was changed.");
  }, [say]);

  const connect = useCallback(async () => {
    // The in-process mock port exists for development and automated smoke
    // checks only. Production builds never look at it, so a published page
    // cannot be pushed into "simulated device" mode from the console.
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
    setBusy(true);
    say("Opening the serial port…");
    try {
      await port.open({ baudRate: BAUD_RATE });
      const io = new Sp1Transport(port);
      const session = new Sp1Session(io);
      try {
        await port.setSignals?.({ dataTerminalReady: true, requestToSend: true });
      } catch { /* optional */ }
      say("Port open at 115200 baud. Entering transfer mode…");
      const l = await session.handshake(40, (n) => setStatus(`Connecting (attempt ${n})…`));

      // Answering SP1XFER! proves only that an SP-1-class device is listening.
      // Writes require the Stem Tape capability reply, which stock firmware
      // never sends.
      const rawCaps = await session.queryCapabilities();
      const caps = rawCaps ? parseCapabilities(rawCaps) : null;
      const t = new StemTapeTransport(session, caps, { kind: injected ? "mock" : "physical" });
      transportRef.current = t;
      setVerdict(t.verdict);
      setMockMode(!!injected);
      await t.readIndex();

      say(`Connected: ${l.numSlots} song slots reported, ${l.sampleRate / 1000} kHz.`);
      say(t.verdict.summary);
      setStatus(transportRef.current?.writable ? "Connected" : "Connected (read-only)");
      await refresh();
      session.startKeepalive();
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      say(`Could not reach the SP-1: ${msg}`);
      setStatus("Not connected");
      await transportRef.current?.disconnect().catch(() => {});
      transportRef.current = null;
      setSongs(null);
      setDescription(null);
    } finally {
      setBusy(false);
    }
  }, [refresh, say]);

  const disconnect = useCallback(async () => {
    const t = transportRef.current;
    if (!t) return;
    await t.disconnect();
    transportRef.current = null;
    setSongs(null);
    setDescription(null);
    setStatus("Not connected");
    say("Exited transfer mode and released the port. The SP-1 has resumed.");
  }, [say]);

  useEffect(() => {
    return () => {
      void transportRef.current?.disconnect().catch(() => {});
    };
  }, []);

  /* ---------- stems ---------- */

  const setStem = useCallback(
    async (name: StemSlotName, file: File) => {
      setFiles((f) => ({ ...f, [name]: file }));
      setSong(null);
      setSongSha(null);
      setResult(null);
      const sniff = sniffHeader(await file.slice(0, 65536).arrayBuffer());
      const ac = new AudioContext();
      try {
        const buf = await ac.decodeAudioData(await file.arrayBuffer());
        setDecoded((d) => ({ ...d, [name]: buf }));
        setSourceRates((r) => ({ ...r, [name]: sniff.sampleRate ?? null }));
        const rate = sniff.sampleRate ? `${sniff.sampleRate} Hz` : `${buf.sampleRate} Hz (decoded)`;
        say(`${STEM_LABEL[name]}: ${file.name} · ${rate} · ${buf.numberOfChannels} ch · ${fmtSecs(buf.duration)}`);
      } catch (e) {
        say(`${STEM_LABEL[name]}: could not decode ${file.name} — ${e instanceof Error ? e.message : String(e)}`);
      } finally {
        void ac.close();
      }
    },
    [say],
  );

  const preview = useCallback((name: StemSlotName) => {
    const file = files[name];
    if (!file) return;
    previewRef.current?.pause();
    const el = new Audio(URL.createObjectURL(file));
    previewRef.current = el;
    void el.play();
  }, [files]);

  const allFour = STEM_ORDER.every((n) => decoded[n]);
  const bpmValue = Number(bpm);
  const downbeatValue = Number(downbeat);
  const metadataComplete = Number.isFinite(bpmValue) && bpmValue > 0 && Number.isFinite(downbeatValue) && downbeatValue >= 0;

  const prepare = useCallback(async () => {
    if (!allFour) return;
    if (!metadataComplete) {
      say("BPM and beat zero are required: Gate, Delay and loops are tempo-locked, so they are not optional notes.");
      return;
    }
    setBusy(true);
    setProgress({ stage: "decoding", fraction: 1, detail: "sources decoded locally" });
    try {
      const result = await prepareCanonicalSong(
        STEM_ORDER.map((n) => ({ name: n, filename: files[n]!.name, buffer: decoded[n]! })),
        {
          metadata: { title, artist, bpm: bpmValue, downbeatSeconds: downbeatValue },
          onStage: (stage, fraction) => setProgress({ stage, fraction, detail: stage }),
        },
      );
      setSong(result);
      setSongSha(await sha256Hex(encodeSong(result)));
      if (result.lengthSpreadSeconds > 0.001) {
        say(
          `Stem lengths differ by ${fmtSecs(result.lengthSpreadSeconds)} — shorter stems were padded with digital silence to the longest.`,
        );
      }
      const detail = `${result.frames} frames · stereo 24-bit @ 48 kHz · ${result.audioBytes} audio bytes (${fmtBytes(result.audioBytes)})`;
      say(`Prepared: ${detail}`);
      setProgress({ stage: "checksums", fraction: 1, detail });
    } catch (e) {
      setSong(null);
      say(`Preparation failed: ${e instanceof Error ? e.message : String(e)}`);
    } finally {
      setBusy(false);
    }
  }, [allFour, artist, bpmValue, decoded, downbeatValue, files, metadataComplete, say, title]);

  const mutationLocked = !verdict.writable;

  const startUpload = useCallback(async () => {
    const t = transportRef.current;
    if (!t || !song) return;
    abortRef.current = { aborted: false };
    setBusy(true);
    try {
      const out = await t.uploadSong({ slot, song, signal: abortRef.current, onProgress: setProgress });
      setResult(out);
      setPlaybackConfirmed(false);
      if (out.ok) {
        say(successLogWording(t.mode.kind));
      } else if (out.outcome === "unknown") {
        say(`Outcome unknown — ${out.detail}`);
      } else {
        say(`Upload stopped: ${out.detail}`);
      }
      await refresh();
    } catch (e) {
      say(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  }, [refresh, say, slot, song]);

  const resolveUnknown = useCallback(async () => {
    const t = transportRef.current;
    if (!t || !song || !result) return;
    setBusy(true);
    try {
      const outcome = await t.resolveOutcome({
        slot,
        frames: song.frames,
        songChecksum: result.songChecksum || 0,
      });
      setResult({ ...result, outcome, ok: outcome === "committed" });
      say(resolveWording(t.mode.kind, outcome));
      await refresh();
    } finally {
      setBusy(false);
    }
  }, [refresh, result, say, slot, song]);

  const downloadReceipt = useCallback(() => {
    const t = transportRef.current;
    if (!t || !song || !result) return;
    const receipt = buildReceipt({
      song,
      result,
      caps: t.caps,
      slot,
      mode: t.mode.kind,
      physicalPlaybackConfirmed: playbackConfirmed,
    });
    const url = URL.createObjectURL(new Blob([JSON.stringify(receipt, null, 2)], { type: "application/json" }));
    const a = document.createElement("a");
    a.href = url;
    a.download = `stem-tape-receipt-${Date.now()}.json`;
    a.click();
    URL.revokeObjectURL(url);
  }, [playbackConfirmed, result, slot, song]);

  const initialiseIndex = useCallback(async () => {
    const t = transportRef.current;
    if (!t) return;
    if (!confirm("Write a fresh, empty song index to this SP-1? Existing songs become unreachable.")) return;
    setBusy(true);
    try {
      await t.initialiseLibrary();
      say("Song index initialised — every slot is empty.");
      await refresh();
    } catch (e) {
      say(`Initialisation failed: ${e instanceof Error ? e.message : String(e)}`);
    } finally {
      setBusy(false);
    }
  }, [refresh, say]);

  const removeSlot = useCallback(
    async (index: number) => {
      const t = transportRef.current;
      if (!t) return;
      if (!confirm(`Delete song ${index + 1} from the SP-1? Its four tracks become unreachable.`)) return;
      setBusy(true);
      try {
        await t.deleteSong(index);
        say(`Deleted song ${index + 1}.`);
        await refresh();
      } catch (e) {
        say(`Delete failed: ${e instanceof Error ? e.message : String(e)}`);
      } finally {
        setBusy(false);
      }
    },
    [refresh, say],
  );

  const verifySlot = useCallback(
    async (index: number) => {
      setBusy(true);
      try {
        await refresh();
        say(`Slot ${index + 1} re-read from the device index.`);
      } finally {
        setBusy(false);
      }
    },
    [refresh, say],
  );

  const tap = useCallback(() => {
    const now = performance.now();
    const taps = tapsRef.current.filter((t) => now - t < 4000);
    taps.push(now);
    tapsRef.current = taps;
    const v = bpmFromTaps(taps);
    if (v) setBpm(String(v));
  }, []);

  const connected = !!songs && !!description;
  const requiredSectors = song ? sectorsForFrames(song.frames) : 0;
  const capacityOk = !!description && requiredSectors > 0 && requiredSectors <= description.sectorsPerSong;
  const anyWriteOccurred = !!result && result.writtenBlocks > 0;
  const stageIndex = result ? 4 : busy && progress ? 3 : song ? 2 : allFour ? 1 : connected ? 1 : 0;
  const nextStep = !connected
    ? "Next: connect a Stem Tape SP-1 over USB."
    : !allFour
      ? "Next: add four synchronised stem files."
      : !song
        ? "Next: confirm BPM and beat zero, then prepare the song."
        : !result
          ? "Next: review the prepared song and start the transfer."
          : result.outcome === "committed"
            ? "Next: confirm physical playback on the SP-1."
            : result.outcome === "unknown"
              ? "Next: reconnect and resolve the unknown outcome."
              : "Next: fix the reported problem and retry safely.";
  const stemRows = useMemo(
    () =>
      STEM_ORDER.map((name) => ({
        name,
        file: files[name],
        buf: decoded[name],
        out: song?.stems.find((s) => s.name === name),
      })),
    [decoded, files, song],
  );

  return (
    <div className="min-h-screen">
      <header className="border-b border-[var(--bench-line)]">
        <div className="flex items-start justify-between gap-4 px-4 pt-3 md:px-8">
          <p className="font-mono text-[9px] uppercase tracking-[0.24em] text-[var(--ink-faint)] md:text-[10px]">
            not affiliated with teenage engineering
          </p>
          <div className="flex shrink-0 items-center gap-4">
            <nav className="hidden items-center gap-6 lg:flex" aria-label="Site">
              <Link to="/" className="st-tab">instrument</Link>
              <Link to="/shop" className="st-tab">shop</Link>
              <Link to="/about" className="st-tab">about</Link>
              <span className="st-tab" data-on>uploader</span>
            </nav>
            <SupportButton />
          </div>
        </div>
        <div className="flex flex-wrap items-center gap-x-8 gap-y-3 px-4 pb-4 pt-2 md:px-8">
          <Link to="/" className="flex items-center gap-3">
            <svg width="34" height="34" viewBox="0 0 34 34" aria-hidden className="text-[var(--ink)]">
              <path d="M17 5 L30 28 H4 Z" fill="none" stroke="currentColor" strokeWidth="1.2" />
            </svg>
            <div>
              <p className="font-mono text-xl tracking-tight text-[var(--ink)]">SP-1 uploader</p>
              <p className="font-mono text-[11px] text-[var(--ink-dim)]">connect · four stems · prepare · inspect</p>
            </div>
          </Link>
        </div>
      </header>

      <main className="mx-auto w-full max-w-[860px] px-4 pb-24 pt-6 md:px-8">
        <nav className="st-section" aria-label="Uploader stages" data-testid="stages">
          <ol className="flex flex-wrap gap-x-5 gap-y-2 font-mono text-[11px] uppercase tracking-[0.16em]">
            {STAGE_NAMES.map((name, i) => (
              <li
                key={name}
                data-testid={`stage-${i + 1}`}
                data-active={stageIndex === i ? "" : undefined}
                className={stageIndex === i ? "text-[var(--ink)]" : stageIndex > i ? "text-[var(--ink-dim)]" : "text-[var(--ink-faint)]"}
              >
                {i + 1} · {name}
              </li>
            ))}
          </ol>
          <p className="mt-2 font-mono text-[12px] text-[var(--ink-dim)]" data-testid="next-step">{nextStep}</p>
          <div className="mt-2 flex flex-wrap gap-3 font-mono text-[11px]">
            {mockMode && (
              <span className="border border-[var(--bench-line)] px-2 py-[2px] text-[var(--ink)]" data-testid="simulated-badge">
                SIMULATED DEVICE — nothing is written to hardware
              </span>
            )}
            <span data-testid="write-state" className="text-[var(--ink-dim)]">
              {anyWriteOccurred ? "a write has occurred on this device" : "no data has been written"}
            </span>
            <span data-testid="safe-to-disconnect" className="text-[var(--ink-dim)]">
              {busy ? "do NOT disconnect: an operation is in progress" : "safe to disconnect"}
            </span>
          </div>
        </nav>

        <section className="st-section" data-testid="write-lock">
          <p className="st-section__title">{verdict.writable ? "compatibility negotiated" : "physical uploads locked"}</p>
          <p className="font-mono text-[13px] leading-relaxed text-[var(--ink-dim)]">{verdict.summary}</p>
          <ul className="mt-3 grid gap-1 font-mono text-[11px] text-[var(--ink-faint)]">
            {verdict.requirements.map((r) => (
              <li key={r.id} data-testid={`req-${r.id}`}>
                {r.satisfied ? "ok  " : "no  "}
                {r.label} · {r.detail}
              </li>
            ))}
          </ul>
        </section>

        {supported === false && (
          <section className="st-section" data-testid="unsupported">
            <p className="st-section__title">this browser cannot talk to the SP-1</p>
            <p className="font-mono text-[13px] leading-relaxed text-[var(--ink-dim)]">
              Direct USB transfer needs Web Serial: Chrome or Edge on desktop, or a supported Chromium browser on
              Android. iPhone and iPad Safari do not provide Web Serial — there is no way to transfer songs from
              iOS, and any page claiming otherwise is wrong.
            </p>
          </section>
        )}

        {/* connection */}
        <section className="st-section">
          <p className="st-section__title">1 · connect sp-1</p>
          <div className="flex flex-wrap items-center gap-3">
            <button
              className="st-btn"
              data-testid="connect"
              disabled={busy || connected}
              onClick={() => void connect()}
            >
              Connect SP-1
            </button>
            <button className="st-btn" data-testid="disconnect" disabled={!connected} onClick={() => void disconnect()}>
              Disconnect
            </button>
            <span className="font-mono text-[12px] text-[var(--ink-dim)]" data-testid="status">{status}</span>
            {connected && (
              <span className="font-mono text-[11px] text-[var(--ink-faint)]" data-testid="mode">
                {mockMode ? "mock transport" : "physical device · read-only"}
              </span>
            )}
          </div>
          {connected && description && (
            <dl className="mt-3 grid grid-cols-2 gap-x-6 gap-y-1 font-mono text-[12px] text-[var(--ink-dim)] md:grid-cols-3">
              <div>device · <span className="text-[var(--ink)]">{description.deviceName}</span></div>
              <div>transport · <span className="text-[var(--ink)]">{description.transport}</span></div>
              <div>audio · <span className="text-[var(--ink)]">{description.audioFormat}</span></div>
              <div>slots reported · <span className="text-[var(--ink)]" data-testid="slots">{description.slots}</span></div>
              <div>library base · <span className="text-[var(--ink)]">block {description.libraryBase}</span></div>
              <div>
                per song · <span className="text-[var(--ink)]">{description.sectorsPerSong}</span> sectors (
                {fmtBytes(description.capacityBytesPerSong)})
              </div>
              <div>index blocks · <span className="text-[var(--ink)]">{description.indexBlocks}</span></div>
              <div>generation · <span className="text-[var(--ink)]">{description.generation}</span></div>
              <div>staging · <span className="text-[var(--ink)]">{description.staging ? "yes" : "no"}</span></div>
            </dl>
          )}
        </section>

        {connected && uninitialised && (
          <section className="st-section" data-testid="uninitialised">
            <p className="st-section__title">song index not initialised</p>
            <p className="font-mono text-[13px] leading-relaxed text-[var(--ink-dim)]">
              This SP-1's index block does not carry a recognised magic, so no song list can be read. Initialisation
              is disabled for physical devices.
            </p>
            <button
              className="st-btn mt-3"
              data-testid="initialise"
              disabled={busy || mutationLocked}
              onClick={() => void initialiseIndex()}
            >
              Initialise song index
            </button>
          </section>
        )}

        {/* library */}
        {connected && songs && (
          <section className="st-section">
            <p className="st-section__title">song slots</p>
            <div className="grid gap-2">
              {songs.map((s) => (
                <div
                  key={s.index}
                  className={`flex flex-wrap items-center gap-3 border border-[var(--bench-line)] px-3 py-2 font-mono text-[12px] ${
                    slot === s.index ? "bg-[var(--bench-raise,transparent)]" : ""
                  }`}
                  data-testid={`slot-${s.index}`}
                >
                  <label className="flex items-center gap-2">
                    <input type="radio" name="slot" checked={slot === s.index} onChange={() => setSlot(s.index)} />
                    <span className="text-[var(--ink)]">song {s.index + 1}</span>
                  </label>
                  <span className="text-[var(--ink-dim)]" data-testid={`slot-${s.index}-state`}>
                    {s.occupied ? `occupied · ${fmtSecs(s.durationSeconds)} · ${fmtBytes(s.bytes)}` : "empty"}
                  </span>
                  <span className="ml-auto flex gap-2">
                    <button className="st-btn" disabled={busy} onClick={() => void verifySlot(s.index)}>Verify</button>
                    <button
                      className="st-btn"
                      disabled={busy || !s.occupied || mutationLocked}
                      onClick={() => void removeSlot(s.index)}
                    >
                      Delete
                    </button>
                  </span>
                </div>
              ))}
            </div>
          </section>
        )}

        {/* stems */}
        <section className="st-section">
          <p className="st-section__title">2 · add stems</p>
          <div className="grid gap-2">
            {stemRows.map(({ name, file, buf, out }) => (
              <div
                key={name}
                className="grid gap-1 border border-[var(--bench-line)] px-3 py-2 font-mono text-[12px]"
                data-testid={`stem-${name}`}
                onDragOver={(e) => e.preventDefault()}
                onDrop={(e) => {
                  e.preventDefault();
                  const f = e.dataTransfer.files[0];
                  if (f) void setStem(name, f);
                }}
              >
                <div className="flex flex-wrap items-center gap-3">
                  <span className="w-[92px] text-[var(--ink)]">{STEM_LABEL[name]}</span>
                  <input
                    type="file"
                    accept=".wav,.mp3,audio/wav,audio/mpeg"
                    data-testid={`file-${name}`}
                    onChange={(e) => {
                      const f = e.target.files?.[0];
                      if (f) void setStem(name, f);
                    }}
                  />
                  <button className="st-btn" disabled={!file} onClick={() => preview(name)}>Preview</button>
                </div>
                {buf && (
                  <p className="text-[var(--ink-dim)]" data-testid={`info-${name}`}>
                    {file?.name} · {sourceRates[name] ?? buf.sampleRate} Hz · {buf.numberOfChannels} ch · {fmtSecs(buf.duration)}
                    {out ? ` · out ${out.pcm24.length} B stereo 24-bit · peak ${(out.peak * 100).toFixed(0)}%` : ""}
                    {out?.clipped ? " · CLIPPING" : ""}
                    {out && out.padFrames > 0 ? " · padded with silence" : ""}
                  </p>
                )}
              </div>
            ))}
          </div>

          <div className="mt-3 grid gap-2 font-mono text-[12px] md:grid-cols-2">
            <label className="flex items-center gap-2">
              <span className="w-[72px] text-[var(--ink-dim)]">title</span>
              <input className="st-input flex-1 border border-[var(--bench-line)] bg-transparent px-2 py-1" value={title} onChange={(e) => setTitle(e.target.value)} />
            </label>
            <label className="flex items-center gap-2">
              <span className="w-[72px] text-[var(--ink-dim)]">artist</span>
              <input className="st-input flex-1 border border-[var(--bench-line)] bg-transparent px-2 py-1" value={artist} onChange={(e) => setArtist(e.target.value)} />
            </label>
            <label className="flex items-center gap-2">
              <span className="w-[72px] text-[var(--ink-dim)]">bpm</span>
              <input className="st-input w-24 border border-[var(--bench-line)] bg-transparent px-2 py-1" data-testid="bpm" value={bpm} onChange={(e) => setBpm(e.target.value)} />
              <button className="st-btn" onClick={tap} data-testid="tap">Tap tempo</button>
            </label>
            <label className="flex items-center gap-2">
              <span className="w-[72px] text-[var(--ink-dim)]">beat zero</span>
              <input className="st-input w-24 border border-[var(--bench-line)] bg-transparent px-2 py-1" data-testid="downbeat" value={downbeat} onChange={(e) => setDownbeat(e.target.value)} />
              <span className="text-[var(--ink-faint)]">seconds</span>
            </label>
          </div>
          <p className="mt-2 font-mono text-[11px] text-[var(--ink-faint)]">
            BPM and beat zero are required, not optional notes: Gate, Delay, loops and every other synchronised
            behaviour are tempo-locked. A song is never reported ready if the negotiated firmware cannot store them.
          </p>

          <div className="mt-3 flex flex-wrap items-center gap-3">
            <button
              className="st-btn"
              data-testid="prepare"
              disabled={!allFour || !metadataComplete || busy}
              onClick={() => void prepare()}
            >
              Prepare stems
            </button>
          </div>
        </section>

        {song && (
          <section className="st-section" data-testid="review">
            <p className="st-section__title">3 · prepare song</p>
            <p className="font-mono text-[13px] leading-relaxed text-[var(--ink-dim)]" data-testid="prepared">
              {song.metadata.title || "untitled"} — {fmtSecs(song.durationSeconds)}, {song.metadata.bpm} BPM, beat zero
              at {song.metadata.downbeatSeconds}s. Four stems, 48 kHz stereo 24-bit.
            </p>
            <p className="mt-1 font-mono text-[12px] text-[var(--ink-dim)]" data-testid="capacity">
              {requiredSectors} sectors of {description ? description.sectorsPerSong : "?"} available in song slot{" "}
              {slot + 1} · {capacityOk ? "fits" : "does NOT fit — shorten the song"}
            </p>
            <button className="st-btn mt-3" onClick={() => setShowTech((v) => !v)} data-testid="tech-toggle">
              {showTech ? "Hide technical detail" : "Show technical detail"}
            </button>
            {showTech && (
              <dl className="mt-2 grid gap-1 font-mono text-[11px] text-[var(--ink-faint)]" data-testid="tech">
                <div>frames · {song.frames} @ {SAMPLE_RATE} Hz</div>
                <div>audio bytes · {song.audioBytes}</div>
                <div>sector bytes · {SECTOR_BYTES} ({BLOCKS_PER_SECTOR} × {PHYSICAL_BLOCK_BYTES} B blocks)</div>
                <div>blocks to write · {requiredSectors * BLOCKS_PER_SECTOR}</div>
                <div>song sha-256 · {songSha ?? "…"}</div>
                <div>song checksum · {song.checksum}</div>
                {song.stems.map((st) => (
                  <div key={st.name}>
                    {st.name} · {st.filename} · pad {st.padFrames} frames · checksum {st.checksum}
                  </div>
                ))}
              </dl>
            )}
          </section>
        )}

        {/* upload */}
        <section className="st-section">
          <p className="st-section__title">4 · transfer</p>
          <div className="flex flex-wrap items-center gap-3">
            <button
              className="st-btn"
              data-testid="upload"
              disabled={!connected || !song || busy || mutationLocked}
              onClick={() => void startUpload()}
            >
              Upload to song {slot + 1}
            </button>
            <button className="st-btn" data-testid="cancel" disabled={!busy} onClick={() => { abortRef.current.aborted = true; }}>
              Cancel
            </button>
          </div>
          {mutationLocked && (
            <p className="mt-2 font-mono text-[12px] text-[var(--ink-dim)]" data-testid="upload-locked">
              {verdict.summary}
            </p>
          )}
          {progress && (
            <p className="mt-2 font-mono text-[12px] text-[var(--ink-dim)]" data-testid="progress">
              {progress.stage} · {Math.round(progress.fraction * 100)}% · {progress.detail}
            </p>
          )}
        </section>

        {result && (
          <section className="st-section" data-testid="verify">
            <p className="st-section__title">5 · verify</p>
            <p className="font-mono text-[13px] leading-relaxed text-[var(--ink-dim)]" data-testid="outcome">
              {result.outcome === "committed"
                ? "Committed. The device index now points at this song."
                : result.outcome === "failed"
                  ? `Not committed — ${result.detail.replace(/\.$/, "")}. The slot still holds whatever it held before, so retrying is safe.`
                  : `Outcome unknown — ${result.detail.replace(/\.$/, "")}. Reconnect the SP-1 and resolve it below before assuming anything.`}
            </p>
            <ul className="mt-3 grid gap-1 font-mono text-[12px]" data-testid="verification">
              <li data-testid="v-simulated">
                {result.verification.simulatedVerification ? "ok  " : "no  "}simulated verification (mock protocol run)
              </li>
              <li data-testid="v-readback">
                {result.verification.deviceReadbackVerification ? "ok  " : "no  "}device readback verification
                (committed bytes re-read from the SP-1)
              </li>
              <li data-testid="v-playback">
                {playbackConfirmed && result.verification.deviceReadbackVerification ? "ok  " : "no  "}physical playback
                verification (you heard it play on the device)
              </li>
            </ul>
            <div className="mt-3 flex flex-wrap items-center gap-3">
              {result.outcome === "unknown" && (
                <button className="st-btn" data-testid="resolve" disabled={busy} onClick={() => void resolveUnknown()}>
                  Reconnect &amp; resolve outcome
                </button>
              )}
              <label className="flex items-center gap-2 font-mono text-[12px] text-[var(--ink-dim)]">
                <input
                  type="checkbox"
                  data-testid="playback-confirm"
                  checked={playbackConfirmed}
                  disabled={!result.verification.deviceReadbackVerification}
                  onChange={(e) => setPlaybackConfirmed(e.target.checked)}
                />
                I played song {slot + 1} on the SP-1 and heard all four stems
              </label>
              <button className="st-btn" data-testid="receipt" onClick={downloadReceipt}>
                Download receipt
              </button>
            </div>
          </section>
        )}

        <section className="st-section">
          <p className="st-section__title">activity</p>
          <pre className="max-h-[220px] overflow-auto whitespace-pre-wrap font-mono text-[11px] text-[var(--ink-dim)]" data-testid="log">
            {log.join("\n")}
          </pre>
        </section>
      </main>
    </div>
  );
}
