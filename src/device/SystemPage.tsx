import { useEffect, useMemo, useState } from "react";
import { DiagnosticPanel } from "@/device/DiagnosticPanel";
import { WorkletPanel } from "@/audio/WorkletPanel";
import { InputPanel } from "@/audio/InputPanel";
import { V26_MAP } from "@/machine/v26map";
import { formatMiB } from "@/audio/memory";
import type { AudioEngine, EngineStatus } from "@/audio/engine";
import type { RecorderSnapshot } from "@/audio/input/recorder";

type DiagProps = React.ComponentProps<typeof DiagnosticPanel>;

interface Props extends DiagProps {
  engine: AudioEngine;
  status: EngineStatus;
  acks: { id: number; type: string; status: string; detail: string }[];
  unlockNote: string;
}

type SysTab = "status" | "input" | "diagnostics";
const SYS_TABS: { id: SysTab; label: string }[] = [
  { id: "status", label: "status" },
  { id: "input", label: "input" },
  { id: "diagnostics", label: "diagnostics" },
];

const Pulse = () => (
  <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1" aria-hidden>
    <path d="M2 13h4l2-7 4 14 2.5-8 2 3H22" />
  </svg>
);
const Reel = () => (
  <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1" aria-hidden>
    <circle cx="11" cy="12" r="8" />
    <circle cx="11" cy="12" r="2.2" />
    <circle cx="11" cy="6.5" r="1.3" />
    <circle cx="15.8" cy="14.8" r="1.3" />
    <circle cx="6.2" cy="14.8" r="1.3" />
  </svg>
);
const Disc = () => (
  <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1" aria-hidden>
    <ellipse cx="12" cy="6" rx="7" ry="3" />
    <path d="M5 6v6c0 1.7 3.1 3 7 3s7-1.3 7-3V6" />
    <path d="M5 12v6c0 1.7 3.1 3 7 3s7-1.3 7-3v-6" />
  </svg>
);
const Chip = () => (
  <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1" aria-hidden>
    <rect x="6" y="6" width="12" height="12" />
    <rect x="9.5" y="9.5" width="5" height="5" />
    <path d="M9 6V3M15 6V3M9 21v-3M15 21v-3M6 9H3M6 15H3M21 9h-3M21 15h-3" />
  </svg>
);
const Target = () => (
  <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1" aria-hidden>
    <circle cx="12" cy="12" r="5" />
    <path d="M12 2v4M12 18v4M2 12h4M18 12h4" />
  </svg>
);
const Sliders = () => (
  <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1" aria-hidden>
    <path d="M3 7h18M3 12h18M3 17h18" />
    <circle cx="8" cy="7" r="2" />
    <circle cx="15" cy="12" r="2" />
    <circle cx="10" cy="17" r="2" />
  </svg>
);
const Doc = () => (
  <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1" aria-hidden>
    <path d="M6 3h8l4 4v14H6z" />
    <path d="M14 3v4h4M9 12h6M9 16h6" />
  </svg>
);

export function SystemPage({ engine, status, acks, unlockNote, ...diag }: Props) {
  const [sysTab, setSysTab] = useState<SysTab>("status");
  const [open, setOpen] = useState<string | null>(null);
  const [snap, setSnap] = useState<RecorderSnapshot | null>(null);

  useEffect(() => {
    const tick = () => setSnap(engine.recording()?.snapshot() ?? null);
    tick();
    const t = setInterval(tick, 700);
    return () => clearInterval(t);
  }, [engine]);

  const coverage = useMemo(() => {
    const satisfied = (r: (typeof V26_MAP)[number]) =>
      (diag.state.coverage[r.id] ?? 0) > 0 || diag.observed[r.id] != null;
    return { covered: V26_MAP.filter(satisfied).length, total: V26_MAP.length };
  }, [diag.state.coverage, diag.observed]);

  const decodedTracks = status.tracks.filter((t) => t.decoded).length;
  const engineMode = status.tracks.some((t) => t.engineMode === "worklet") ? "worklet" : "node";
  const migration = status.tracks[0]?.migrationStatus ?? "stable";
  const ready = status.contextState === "running";

  const exportReport = () => {
    const blob = new Blob(
      [
        JSON.stringify(
          { at: new Date().toISOString(), status, coverage, machine: diag.state, leds: diag.leds, acks },
          null,
          2,
        ),
      ],
      { type: "application/json" },
    );
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = "stem-tape-system-report.json";
    a.click();
    URL.revokeObjectURL(url);
  };

  const rows: { id: string; icon: React.ReactNode; label: string; value: string; body: React.ReactNode }[] = [
    {
      id: "engine",
      icon: <Pulse />,
      label: "engine & migration",
      value: migration,
      body: <WorkletPanel engine={engine} status={status} />,
    },
    {
      id: "machine",
      icon: <Sliders />,
      label: "machine state",
      value: diag.state.power ? "live" : "idle",
      body: <DiagnosticPanel {...diag} />,
    },
    {
      id: "coverage",
      icon: <Target />,
      label: "mapping coverage",
      value: `${coverage.covered} of ${coverage.total}`,
      body: (
        <p className="font-mono text-[10px] leading-relaxed text-[var(--ink-dim)]">
          {coverage.covered} of {coverage.total} v2.6 control-map rows have been exercised in this session. Open MACHINE
          STATE for the full row-by-row evidence table.
        </p>
      ),
    },
    {
      id: "log",
      icon: <Doc />,
      label: "interaction log",
      value: acks.length ? `${acks.length} acks` : "empty",
      body: (
        <ul className="grid gap-1" data-testid="ack-log">
          {acks.length === 0 && <li className="font-mono text-[10px] text-[var(--ink-faint)]">no commands yet.</li>}
          {acks.slice(0, 12).map((a) => (
            <li key={a.id} className="font-mono text-[10px] text-[var(--ink-dim)]">
              <span className="text-[var(--ink)]">
                #{a.id} {a.type}
              </span>{" "}
              · {a.status} · {a.detail}
            </li>
          ))}
        </ul>
      ),
    },
  ];

  return (
    <section className="st-sys" data-testid="system-page">
      <header className="st-pj-banner">
        <div className="min-w-0">
          <p className="st-pj-banner__name">system</p>
          <p className="st-pj-banner__eyebrow">sp-1 · v2.6</p>
        </div>
        <span className="st-pj-banner__state" data-saved={ready}>
          <i aria-hidden />
          {ready ? "ready" : status.contextState}
        </span>
      </header>

      <div className="st-sys-tabs" role="tablist">
        {SYS_TABS.map((t) => (
          <button
            key={t.id}
            type="button"
            role="tab"
            aria-selected={sysTab === t.id}
            className="st-sys-tab"
            data-on={sysTab === t.id}
            onClick={() => setSysTab(t.id)}
          >
            {t.label}
          </button>
        ))}
      </div>

      {sysTab === "status" && (
        <>
          <div className="st-pj-card" data-testid="audio-diagnostics">
            <p className="st-pj-card__title">core status</p>
            <div className="st-sys-grid">
              <div className="st-sys-cell">
                <span className="st-sys-cell__icon">
                  <Pulse />
                </span>
                <div>
                  <p className="st-sys-cell__k">audio engine</p>
                  <p className="st-sys-cell__v" data-testid="ctx-state">
                    {ready ? "ready" : status.contextState}
                  </p>
                </div>
              </div>
              <div className="st-sys-cell">
                <span className="st-sys-cell__icon">
                  <Reel />
                </span>
                <div>
                  <p className="st-sys-cell__k">tracks</p>
                  <p className="st-sys-cell__v">
                    {decodedTracks} of {status.tracks.length}
                  </p>
                </div>
              </div>
              <div className="st-sys-cell">
                <span className="st-sys-cell__icon">
                  <Disc />
                </span>
                <div>
                  <p className="st-sys-cell__k">storage</p>
                  <p className="st-sys-cell__v">local</p>
                </div>
              </div>
              <div className="st-sys-cell">
                <span className="st-sys-cell__icon">
                  <Chip />
                </span>
                <div>
                  <p className="st-sys-cell__k">memory</p>
                  <p className="st-sys-cell__v">{formatMiB(status.decodedBytes)}</p>
                </div>
              </div>
            </div>
          </div>

          <div className="st-pj-card">
            <p className="st-pj-card__title">audio engine</p>
            <div className="st-sys-quad">
              <div className="st-sys-quad__c">
                <p className="st-sys-cell__k">engine</p>
                <p className="st-sys-cell__v">{engineMode}</p>
              </div>
              <div className="st-sys-quad__c">
                <p className="st-sys-cell__k">mode</p>
                <p className="st-sys-cell__v">{migration}</p>
              </div>
              <div className="st-sys-quad__c">
                <p className="st-sys-cell__k">sample rate</p>
                <p className="st-sys-cell__v">
                  {status.sampleRate ? `${Math.round(status.sampleRate / 1000)} kHz` : "—"}
                </p>
              </div>
              <div className="st-sys-quad__c">
                <p className="st-sys-cell__k">rate</p>
                <p className="st-sys-cell__v" data-testid="start-spread" data-spread={status.startSpreadMs}>
                  {status.rate.toFixed(3)}×
                </p>
              </div>
            </div>
            <p className="mt-2 font-mono text-[10px] text-[var(--ink-faint)]">
              {unlockNote} · position {status.position.toFixed(3)}s of {status.duration.toFixed(3)}s · start spread{" "}
              {status.startSpreadMs.toFixed(4)} ms · last error {status.lastError ?? "none"}
            </p>
          </div>

          <div className="st-pj-card">
            <p className="st-pj-card__title">input &amp; recording</p>
            <div className="st-sys-input">
              <button type="button" className="st-sys-switch" onClick={() => setSysTab("input")}>
                <span className="st-sys-cell__k">input</span>
                <span className="st-sys-switch__track" data-on={snap?.inputEnabled ?? false}>
                  <i />
                </span>
                <span className="st-sys-cell__v">{snap?.inputEnabled ? "on" : "off"}</span>
              </button>
              <div className="st-sys-monitor">
                <p className="st-sys-cell__k">monitor</p>
                <div className="st-sys-seg">
                  {(["off", "dry", "fx"] as const).map((m) => (
                    <span key={m} className="st-sys-seg__i" data-on={(snap?.monitor ?? "off") === m}>
                      {m}
                    </span>
                  ))}
                </div>
              </div>
              <div className="st-sys-quad__c">
                <p className="st-sys-cell__k">bit depth</p>
                <p className="st-sys-cell__v">{snap?.settings ? "24 bit" : "24 bit"}</p>
              </div>
              <div className="st-sys-quad__c">
                <p className="st-sys-cell__k">latency</p>
                <p className="st-sys-cell__v">
                  {snap?.latency?.totalMs != null ? `${snap.latency.totalMs.toFixed(1)} ms` : "—"}
                </p>
              </div>
            </div>
          </div>

          <div className="st-pj-card">
            <p className="st-pj-card__title">advanced diagnostics</p>
            <div className="st-sys-rows">
              {rows.map((r) => (
                <div key={r.id}>
                  <button
                    type="button"
                    className="st-sys-row"
                    aria-expanded={open === r.id}
                    onClick={() => setOpen(open === r.id ? null : r.id)}
                  >
                    <span className="st-sys-row__icon">{r.icon}</span>
                    <span className="st-sys-row__label">{r.label}</span>
                    <span className="st-sys-row__value">{r.value}</span>
                    <span className="st-sys-row__chev" data-open={open === r.id} aria-hidden>
                      ›
                    </span>
                  </button>
                  {open === r.id && <div className="st-sys-row__body">{r.body}</div>}
                </div>
              ))}
            </div>
          </div>

          <button type="button" className="st-pj-save" onClick={exportReport}>
            ⤓ export report
          </button>
        </>
      )}

      {sysTab === "input" && <InputPanel engine={engine} />}

      {sysTab === "diagnostics" && (
        <div className="grid gap-4">
          <WorkletPanel engine={engine} status={status} />
          <DiagnosticPanel {...diag} />
        </div>
      )}
    </section>
  );
}
