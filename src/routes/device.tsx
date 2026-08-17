/**
 * SP-1 companion uploader — local song-library transfer over Web Serial.
 *
 * Scope guard: this route is not an instrument, MIDI surface, LED simulator or
 * diagnostic dashboard. It connects, lists slots, prepares four stems locally
 * and performs one transactional upload with read-back verification.
 */
import { createFileRoute, Link } from "@tanstack/react-router";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { SupportButton } from "@/components/SupportButton";
import { Sp1Transport, Sp1Session, BAUD_RATE, type SerialLikePort } from "@/sp1/protocol";
import {
  parseMeta,
  buildMeta,
  metaBlockCount,
  capacity,
  blocksToSeconds,
  trackAudioBlocks,
  type Sp1Meta,
} from "@/sp1/meta";
import {
  prepareFourStems,
  validatePackage,
  bpmFromTaps,
  STEM_ORDER,
  STEM_LABEL,
  type PrepareResult,
  type StemSlotName,
} from "@/sp1/prepare";
import { uploadSong, deleteSlot, type UploadProgress } from "@/sp1/upload";

export const Route = createFileRoute("/device")({
  component: DevicePage,
  head: () => ({
    meta: [
      { title: "SP-1 Uploader — Stem Tape" },
      {
        name: "description",
        content:
          "Load four stems onto a Stem Tape SP-1 over USB. Everything is prepared in your browser: no account, no cloud, no upload of your audio.",
      },
      { property: "og:title", content: "SP-1 Uploader — Stem Tape" },
      {
        property: "og:description",
        content: "Connect the SP-1 over USB, choose a song slot, load four stems, verify and disconnect.",
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
  const [meta, setMeta] = useState<Sp1Meta | null>(null);
  const [slot, setSlot] = useState(0);
  const [files, setFiles] = useState<Files>({});
  const [decoded, setDecoded] = useState<Decoded>({});
  const [prepared, setPrepared] = useState<PrepareResult | null>(null);
  const [progress, setProgress] = useState<UploadProgress | null>(null);
  const [busy, setBusy] = useState(false);
  const [uninitialised, setUninitialised] = useState(false);
  const [title, setTitle] = useState("");
  const [artist, setArtist] = useState("");
  const [bpm, setBpm] = useState("");
  const [downbeat, setDownbeat] = useState("0");
  const sessionRef = useRef<Sp1Session | null>(null);
  const abortRef = useRef({ aborted: false });
  const tapsRef = useRef<number[]>([]);
  const previewRef = useRef<HTMLAudioElement | null>(null);

  const say = useCallback((line: string) => {
    setLog((l) => [...l.slice(-60), `${new Date().toLocaleTimeString()}  ${line}`]);
  }, []);

  useEffect(() => {
    setSupported(typeof navigator !== "undefined" && "serial" in navigator);
  }, []);

  const layout = sessionRef.current?.layout ?? null;
  const cap = layout && meta ? capacity(layout, meta) : null;

  const refresh = useCallback(async () => {
    const session = sessionRef.current;
    if (!session?.layout) return;
    const n = metaBlockCount(session.layout);
    const parsedRaw = await session.lock.run(async () => {
      const b0 = await session.readBlock(0);
      if (n === 1) return b0;
      const b1 = await session.readBlock(1);
      const joined = new Uint8Array(1024);
      joined.set(b0, 0);
      joined.set(b1, 512);
      return joined;
    });
    const m = parseMeta(parsedRaw, session.layout);
    setMeta(m);
    const bad = m.magic !== session.layout.magic;
    setUninitialised(bad);
    if (bad) {
      say("This SP-1's song index is uninitialised or written by different firmware. Nothing was changed.");
    }
  }, [say]);

  const connect = useCallback(async () => {
    const injected = (globalThis as { __SP1_MOCK_PORT__?: SerialLikePort }).__SP1_MOCK_PORT__;
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
      sessionRef.current = session;
      try {
        await port.setSignals?.({ dataTerminalReady: true, requestToSend: true });
      } catch { /* optional */ }
      say("Port open at 115200 baud. Entering transfer mode…");
      const l = await session.handshake(40, (n) => setStatus(`Connecting (attempt ${n})…`));
      say(`Connected: ${l.numSlots} song slots × ${l.ntrk} tracks, ${l.sampleRate / 1000} kHz, ${l.blockSize}-byte blocks.`);
      setStatus("Connected to SP-1");
      await refresh();
      session.startKeepalive();
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      say(`Could not reach the SP-1: ${msg}`);
      setStatus("Not connected");
      await sessionRef.current?.io.close();
      sessionRef.current = null;
      setMeta(null);
    } finally {
      setBusy(false);
    }
  }, [refresh, say]);

  const disconnect = useCallback(async () => {
    const session = sessionRef.current;
    if (!session) return;
    session.stopKeepalive();
    await session.exit();
    await session.io.close();
    sessionRef.current = null;
    setMeta(null);
    setStatus("Not connected");
    say("Exited transfer mode and released the port. The SP-1 has resumed.");
  }, [say]);

  useEffect(() => {
    return () => {
      const session = sessionRef.current;
      if (session) {
        session.stopKeepalive();
        void session.io.close();
      }
    };
  }, []);

  /* ---------- stems ---------- */

  const setStem = useCallback(
    async (name: StemSlotName, file: File) => {
      setFiles((f) => ({ ...f, [name]: file }));
      setPrepared(null);
      const ac = new AudioContext();
      try {
        const buf = await ac.decodeAudioData(await file.arrayBuffer());
        setDecoded((d) => ({ ...d, [name]: buf }));
        say(
          `${STEM_LABEL[name]}: ${file.name} · ${buf.sampleRate} Hz · ${buf.numberOfChannels} ch · ${fmtSecs(buf.duration)}`,
        );
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

  const prepare = useCallback(async () => {
    const l = sessionRef.current?.layout;
    if (!l || !allFour) return;
    setBusy(true);
    setProgress({ stage: "decoding", fraction: 1, detail: "sources decoded locally" });
    try {
      const res = await prepareFourStems(
        STEM_ORDER.map((n) => ({ name: n, filename: files[n]!.name, buffer: decoded[n]! })),
        {
          deviceRate: l.sampleRate,
          maxBlocks: l.trackBlocks,
          onStage: (stage, fraction) =>
            setProgress({ stage: stage as UploadProgress["stage"], fraction, detail: stage }),
        },
      );
      const check = validatePackage(res);
      if (!check.ok) {
        say(`Package validation failed: ${check.detail}`);
        setPrepared(null);
        return;
      }
      setPrepared(res);
      if (res.lengthSpreadSeconds > 0.001) {
        say(
          `Stem lengths differ by ${fmtSecs(res.lengthSpreadSeconds)} — shorter stems were padded with digital silence to the longest.`,
        );
      }
      if (res.truncated) say(`One stem exceeded this SP-1's per-track region and was clamped to ${res.blocks} blocks.`);
      say(`Prepared: ${check.detail}`);
      setProgress({ stage: "checksumming", fraction: 1, detail: check.detail });
    } finally {
      setBusy(false);
    }
  }, [allFour, decoded, files, say]);

  const startUpload = useCallback(async () => {
    const session = sessionRef.current;
    if (!session || !meta || !prepared) return;
    abortRef.current = { aborted: false };
    setBusy(true);
    session.stopKeepalive();
    const out = await uploadSong({
      session,
      meta,
      slot,
      stems: prepared.stems,
      signal: abortRef.current,
      onProgress: setProgress,
    });
    say(out.ok ? out.detail : `Upload stopped: ${out.detail}`);
    if (!out.ok) say("Nothing was committed — the slot still holds whatever it held before. You can retry safely.");
    await refresh();
    session.startKeepalive();
    setBusy(false);
  }, [meta, prepared, refresh, say, slot]);

  const initialiseIndex = useCallback(async () => {
    const session = sessionRef.current;
    if (!session?.layout || !meta) return;
    if (!confirm("Write a fresh, empty song index to this SP-1? Existing songs become unreachable.")) return;
    setBusy(true);
    try {
      await session.lock.run(async () => {
        const fresh = parseMeta(new Uint8Array(metaBlockCount(session.layout!) * 512), session.layout!);
        const mb = buildMeta(fresh, session.layout!);
        if (metaBlockCount(session.layout!) === 2) await session.writeBlock(1, mb.slice(512, 1024));
        await session.writeBlock(0, mb.slice(0, 512));
        await session.flush();
      });
      say("Song index initialised — every slot is empty.");
      await refresh();
    } catch (e) {
      say(`Initialisation failed: ${e instanceof Error ? e.message : String(e)}`);
    } finally {
      setBusy(false);
    }
  }, [meta, refresh, say]);

  const removeSlot = useCallback(
    async (index: number) => {
      const session = sessionRef.current;
      if (!session || !meta) return;
      if (!confirm(`Delete song ${index + 1} from the SP-1? Its four tracks become unreachable.`)) return;
      setBusy(true);
      try {
        await deleteSlot(session, meta, index);
        say(`Deleted song ${index + 1}.`);
        await refresh();
      } catch (e) {
        say(`Delete failed: ${e instanceof Error ? e.message : String(e)}`);
      } finally {
        setBusy(false);
      }
    },
    [meta, refresh, say],
  );

  const verifySlot = useCallback(
    async (index: number) => {
      const session = sessionRef.current;
      if (!session || !session.layout) return;
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

  const connected = !!meta && !!layout;
  const stemRows = useMemo(
    () =>
      STEM_ORDER.map((name) => ({
        name,
        file: files[name],
        buf: decoded[name],
        out: prepared?.stems.find((s) => s.name === name),
      })),
    [decoded, files, prepared],
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
              <p className="font-mono text-[11px] text-[var(--ink-dim)]">connect · four stems · verify · disconnect</p>
            </div>
          </Link>
        </div>
      </header>

      <main className="mx-auto w-full max-w-[860px] px-4 pb-24 pt-6 md:px-8">
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
          <p className="st-section__title">1 · connection</p>
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
          </div>
          {connected && layout && cap && (
            <dl className="mt-3 grid grid-cols-2 gap-x-6 gap-y-1 font-mono text-[12px] text-[var(--ink-dim)] md:grid-cols-3">
              <div>device · <span className="text-[var(--ink)]">Stem Tape SP-1</span></div>
              <div>protocol · <span className="text-[var(--ink)]">SP1XFER block v1</span></div>
              <div>audio · <span className="text-[var(--ink)]">{layout.sampleRate / 1000} kHz mono int16</span></div>
              <div>slots · <span className="text-[var(--ink)]" data-testid="slots">{layout.numSlots}</span></div>
              <div>
                used · <span className="text-[var(--ink)]">{fmtBytes(cap.usedBlocks * 512)}</span> of{" "}
                {fmtBytes(cap.totalBlocks * 512)}
              </div>
              <div>per track · <span className="text-[var(--ink)]">{fmtSecs(cap.perTrackSeconds)}</span></div>
            </dl>
          )}
        </section>

        {connected && uninitialised && (
          <section className="st-section" data-testid="uninitialised">
            <p className="st-section__title">song index not initialised</p>
            <p className="font-mono text-[13px] leading-relaxed text-[var(--ink-dim)]">
              This SP-1's index block does not carry this firmware's magic, so no song list can be read. Initialising
              writes one empty index (all slots marked empty) and flushes it. Any songs currently on the device become
              unreachable and their audio blocks are overwritten by later uploads. Nothing is initialised automatically.
            </p>
            <button
              className="st-btn mt-3"
              data-testid="initialise"
              disabled={busy}
              onClick={() => void initialiseIndex()}
            >
              Initialise song index
            </button>
          </section>
        )}

        {/* library */}
        {connected && meta && layout && (
          <section className="st-section">
            <p className="st-section__title">2 · song slots</p>
            <div className="grid gap-2">
              {meta.slots.map((s, i) => {
                const occupied = s.present.some((p) => !!p);
                const secs = blocksToSeconds(trackAudioBlocks(s, 0), layout.sampleRate);
                return (
                  <div
                    key={i}
                    className={`flex flex-wrap items-center gap-3 border border-[var(--bench-line)] px-3 py-2 font-mono text-[12px] ${
                      slot === i ? "bg-[var(--bench-raise,transparent)]" : ""
                    }`}
                    data-testid={`slot-${i}`}
                  >
                    <label className="flex items-center gap-2">
                      <input type="radio" name="slot" checked={slot === i} onChange={() => setSlot(i)} />
                      <span className="text-[var(--ink)]">song {i + 1}</span>
                    </label>
                    <span className="text-[var(--ink-dim)]" data-testid={`slot-${i}-state`}>
                      {occupied ? `occupied · ${fmtSecs(secs)} · ${fmtBytes((s.trkLen[0] || 0) * 4 * 512)}` : "empty"}
                    </span>
                    <span className="ml-auto flex gap-2">
                      <button className="st-btn" disabled={busy} onClick={() => void verifySlot(i)}>Verify</button>
                      <button className="st-btn" disabled={busy || !occupied} onClick={() => void removeSlot(i)}>
                        Delete
                      </button>
                    </span>
                  </div>
                );
              })}
            </div>
            <p className="mt-2 font-mono text-[11px] text-[var(--ink-faint)]">
              The SP-1 index carries no title, artist, BPM or downbeat field — song details you enter below stay on
              this device only.
            </p>
          </section>
        )}

        {/* stems */}
        <section className="st-section">
          <p className="st-section__title">3 · four stems</p>
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
                    {file?.name} · {buf.sampleRate} Hz · {buf.numberOfChannels} ch · {fmtSecs(buf.duration)}
                    {out ? ` · out ${fmtBytes(out.outputBytes)} · peak ${(out.peak * 100).toFixed(0)}%` : ""}
                    {out?.clipped ? " · CLIPPING" : ""}
                    {out && out.padSamples > 0 ? " · padded with silence" : ""}
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
              <input className="st-input w-24 border border-[var(--bench-line)] bg-transparent px-2 py-1" value={bpm} onChange={(e) => setBpm(e.target.value)} />
              <button className="st-btn" onClick={tap} data-testid="tap">Tap tempo</button>
            </label>
            <label className="flex items-center gap-2">
              <span className="w-[72px] text-[var(--ink-dim)]">beat zero</span>
              <input className="st-input w-24 border border-[var(--bench-line)] bg-transparent px-2 py-1" value={downbeat} onChange={(e) => setDownbeat(e.target.value)} />
              <span className="text-[var(--ink-faint)]">seconds</span>
            </label>
          </div>
          <p className="mt-2 font-mono text-[11px] text-[var(--ink-faint)]">
            The transfer contract has no metadata block for these values, so nothing is invented on the wire: the
            SP-1 derives its beat grid from the length of the first track in a song.
          </p>

          <div className="mt-3 flex flex-wrap items-center gap-3">
            <button className="st-btn" data-testid="prepare" disabled={!connected || !allFour || busy} onClick={() => void prepare()}>
              Prepare stems
            </button>
            {prepared && (
              <span className="font-mono text-[12px] text-[var(--ink-dim)]" data-testid="prepared">
                {prepared.blocks} blocks per track · {fmtSecs(blocksToSeconds(prepared.blocks, layout?.sampleRate ?? 48000))}
              </span>
            )}
          </div>
        </section>

        {/* upload */}
        <section className="st-section">
          <p className="st-section__title">4 · upload</p>
          <div className="flex flex-wrap items-center gap-3">
            <button className="st-btn" data-testid="upload" disabled={!connected || !prepared || busy} onClick={() => void startUpload()}>
              Upload to song {slot + 1}
            </button>
            <button className="st-btn" data-testid="cancel" disabled={!busy} onClick={() => { abortRef.current.aborted = true; }}>
              Cancel
            </button>
          </div>
          {progress && (
            <p className="mt-2 font-mono text-[12px] text-[var(--ink-dim)]" data-testid="progress">
              {progress.stage} · {Math.round(progress.fraction * 100)}% · {progress.detail}
            </p>
          )}
        </section>

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
