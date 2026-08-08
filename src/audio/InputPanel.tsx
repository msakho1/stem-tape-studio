/**
 * Phase 6 Input Drawer — permission, device, monitoring, latency, takes,
 * tempo grid and local WAV export. Everything here stays on the device.
 */

import { useCallback, useEffect, useMemo, useState } from "react";
import type { AudioEngine } from "./engine";
import { MONITOR_CEILING, type MonitorMode, type RecorderSnapshot } from "./input/recorder";
import { totalCompensationMs } from "./input/latency";
import { exportTakeToWav, planFor } from "./export/exportTake";
import type { BitDepth } from "./export/wavStream";

export function InputPanel({ engine }: { engine: AudioEngine }) {
  const [snap, setSnap] = useState<RecorderSnapshot | null>(null);
  const [note, setNote] = useState("input is off — nothing is being captured");
  const [busy, setBusy] = useState(false);
  const [depth, setDepth] = useState<BitDepth>(24);
  const [perfNote, setPerfNote] = useState("master performance recorder idle");

  const refresh = useCallback(() => {
    setSnap(engine.recording()?.snapshot() ?? null);
  }, [engine]);

  useEffect(() => {
    refresh();
    const id = setInterval(refresh, 250);
    return () => clearInterval(id);
  }, [refresh]);

  const enable = useCallback(
    async (deviceId?: string) => {
      setBusy(true);
      const unlocked = await engine.unlock();
      if (!unlocked.ok) {
        setNote(unlocked.detail);
        setBusy(false);
        return;
      }
      const rec = engine.recording();
      const r = rec ? await rec.enableInput(deviceId) : { ok: false, detail: "engine not ready" };
      setNote(r.detail);
      setBusy(false);
      refresh();
    },
    [engine, refresh],
  );

  const setMonitor = (mode: MonitorMode) => {
    engine.recording()?.setMonitor(mode);
    refresh();
  };

  const takes = snap?.takes ?? [];
  const meterPct = Math.min(100, Math.round((snap?.meter.peak ?? 0) * 100));
  const grid = engine.grid;
  const plan = useMemo(() => (takes[0] ? planFor(takes[0], depth) : null), [takes, depth]);

  return (
    <section className="st-card" data-testid="input-panel">
      <header className="flex items-center justify-between gap-3">
        <h2 className="font-mono text-[11px] uppercase tracking-[0.2em] text-[var(--ink)]">input &amp; recording</h2>
        <span className="font-mono text-[10px] text-[var(--ink-faint)]" data-testid="rec-transfer">
          {snap?.transferPath ?? "none"}
        </span>
      </header>

      <p className="mt-2 font-mono text-[10px] leading-relaxed text-[var(--ink-faint)]" data-testid="rec-privacy">
        {snap?.privacy ?? "captured audio is written to this device only — nothing is uploaded"}
      </p>

      {/* ---- permission ---- */}
      <div className="mt-3 flex flex-wrap items-center gap-2">
        <button
          type="button"
          className="st-btn"
          data-testid="enable-input"
          disabled={busy}
          onClick={() => void enable()}
        >
          {snap?.inputEnabled ? "input enabled" : "enable input"}
        </button>
        {snap?.inputEnabled && (
          <button type="button" className="st-btn" data-testid="disable-input" onClick={() => { engine.recording()?.disableInput(); refresh(); }}>
            release microphone
          </button>
        )}
        {snap?.devices.length ? (
          <select
            className="st-select font-mono text-[10px]"
            data-testid="input-device"
            onChange={(e) => void enable(e.target.value)}
            defaultValue=""
          >
            <option value="">default input</option>
            {snap.devices.map((d) => (
              <option key={d.deviceId} value={d.deviceId}>{d.label}</option>
            ))}
          </select>
        ) : null}
      </div>
      <p className="mt-2 font-mono text-[10px] text-[var(--ink-dim)]" data-testid="rec-note">{note}</p>

      {/* ---- meter ---- */}
      <div className="mt-3">
        <div className="h-2 w-full overflow-hidden rounded-full bg-[var(--surface-2)]">
          <div className="h-full bg-[var(--accent)] transition-[width] duration-75" style={{ width: `${meterPct}%` }} data-testid="rec-meter" />
        </div>
        <p className="mt-1 font-mono text-[10px] text-[var(--ink-faint)]">
          peak {(snap?.meter.peak ?? 0).toFixed(3)} · rms {(snap?.meter.rms ?? 0).toFixed(3)}
        </p>
      </div>

      {/* ---- monitoring ---- */}
      <div className="mt-3 flex flex-wrap items-center gap-2">
        {(["off", "dry", "fx"] as MonitorMode[]).map((m) => (
          <button
            key={m}
            type="button"
            className="st-btn"
            data-on={snap?.monitor === m}
            data-testid={`monitor-${m}`}
            onClick={() => setMonitor(m)}
          >
            monitor {m}
          </button>
        ))}
        <span className="font-mono text-[10px] text-[var(--ink-faint)]">
          ceiling {MONITOR_CEILING.toFixed(2)} · use headphones to avoid feedback
        </span>
      </div>

      {/* ---- latency ---- */}
      <p className="mt-3 font-mono text-[10px] text-[var(--ink-dim)]" data-testid="rec-latency">
        {snap?.latencyStatement ?? "latency unknown until input is enabled"}
      </p>
      <label className="mt-1 flex items-center gap-2 font-mono text-[10px] text-[var(--ink-faint)]">
        manual offset
        <input
          type="range"
          min={-50}
          max={50}
          step={1}
          defaultValue={0}
          data-testid="latency-offset"
          onChange={(e) => {
            const rec = engine.recording();
            if (rec) rec.latency = { ...rec.latency, manualOffsetMs: Number(e.target.value) };
            refresh();
          }}
        />
        <span className="text-[var(--ink)]">{snap ? `${totalCompensationMs(snap.latency).toFixed(1)} ms total` : "—"}</span>
      </label>

      {/* ---- tempo grid ---- */}
      <div className="mt-4 flex flex-wrap items-center gap-2">
        <button
          type="button"
          className="st-btn"
          data-testid="grid-tap"
          onClick={() => { engine.execute({ id: Date.now(), t: performance.now(), type: "grid.tap", payload: {} }); refresh(); }}
        >
          tap tempo
        </button>
        <button
          type="button"
          className="st-btn"
          data-on={engine.quantisePunch}
          data-testid="grid-quantise"
          onClick={() => { engine.execute({ id: Date.now() + 1, t: performance.now(), type: "grid.quantise", payload: {} }); refresh(); }}
        >
          quantise punch
        </button>
        <span className="font-mono text-[10px] text-[var(--ink-dim)]" data-testid="grid-readout">
          {grid.rejected ? "grid rejected" : grid.bpm ? `${grid.bpm.toFixed(2)} BPM (${grid.source})` : "no grid learned"}
        </span>
      </div>

      {/* ---- takes ---- */}
      <ul className="mt-4 grid gap-2" data-testid="take-list">
        {takes.length === 0 && <li className="font-mono text-[10px] text-[var(--ink-faint)]">no takes yet — hold a track button to arm it</li>}
        {takes.map((t) => (
          <li key={t.id} className="flex flex-wrap items-center gap-2 rounded-md bg-[var(--surface-2)] px-2 py-1 font-mono text-[10px]">
            <span className="text-[var(--ink)]">{t.label || t.id}</span>
            <span className="text-[var(--ink-dim)]">track {t.trackId + 1 || "master"}</span>
            <span className="text-[var(--ink-dim)]">{(t.frames / t.sampleRate).toFixed(2)}s</span>
            <span data-testid={`take-state-${t.id}`} className={t.state === "ready" ? "text-[var(--accent)]" : "text-[var(--ink-faint)]"}>
              {t.state}{t.failureReason ? ` · ${t.failureReason}` : ""}
            </span>
            <button
              type="button"
              className="st-btn"
              onClick={() => { setNote(engine.recording()?.setTakeEnabled(t.id, !t.enabled).detail ?? ""); refresh(); }}
            >
              {t.enabled ? "mute pass" : "unmute pass"}
            </button>
            <button
              type="button"
              className="st-btn"
              data-testid={`export-${t.id}`}
              disabled={t.state !== "ready"}
              onClick={async () => {
                const r = await exportTakeToWav(t, depth);
                setNote(r.detail);
              }}
            >
              export wav
            </button>
          </li>
        ))}
      </ul>

      <div className="mt-2 flex flex-wrap items-center gap-2">
        {([16, 24] as BitDepth[]).map((d) => (
          <button key={d} type="button" className="st-btn" data-on={depth === d} onClick={() => setDepth(d)}>
            {d}-bit
          </button>
        ))}
        {plan && <span className="font-mono text-[10px] text-[var(--ink-faint)]">{plan.plan.detail}</span>}
      </div>

      {/* ---- master performance recording ---- */}
      <div className="mt-4 flex flex-wrap items-center gap-2">
        <button
          type="button"
          className="st-btn"
          data-testid="perf-record"
          onClick={async () => {
            await engine.unlock();
            const p = engine.performance();
            if (!p) return;
            const r = p.recording ? p.stop() : await p.start();
            setPerfNote(r.detail);
          }}
        >
          {engine.performance()?.recording ? "stop performance" : "record performance"}
        </button>
        <span className="font-mono text-[10px] text-[var(--ink-dim)]" data-testid="perf-note">{perfNote}</span>
      </div>

      {/* ---- budget + telemetry ---- */}
      {snap && (
        <dl className="mt-4 grid grid-cols-2 gap-x-3 gap-y-1 font-mono text-[10px] text-[var(--ink-dim)]" data-testid="rec-telemetry">
          <dt>state</dt>
          <dd className="text-[var(--ink)]">{snap.model.lastAck}</dd>
          <dt>blocks emitted / recycled</dt>
          <dd>{snap.blocksEmitted} / {snap.blocksRecycled}</dd>
          <dt>pool free / exhaustions</dt>
          <dd>{snap.poolFree} / {snap.poolExhaustions}</dd>
          <dt>pages resident</dt>
          <dd>{snap.budget.resident} · {(snap.budget.bytes / 1048576).toFixed(1)} MiB of {(snap.budget.limitBytes / 1048576).toFixed(0)} MiB</dd>
          <dt>page misses / underruns</dt>
          <dd>{snap.budget.misses} / {snap.budget.underruns}</dd>
        </dl>
      )}
    </section>
  );
}
