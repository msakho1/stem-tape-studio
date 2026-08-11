import { useState } from "react";

/** Cupped hand receiving a floating coin. */
function SupportMark() {
  return (
    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.4" aria-hidden>
      <circle cx="12" cy="5.5" r="2.6" />
      <path d="M12 4.4v2.2M11 5.5h2" strokeWidth="1" />
      <path d="M3.5 13.5c0 4 3.8 6.5 8.5 6.5s8.5-2.5 8.5-6.5" strokeLinecap="round" />
      <path d="M3.5 13.5h17" strokeLinecap="round" />
    </svg>
  );
}

const TIERS = [
  { label: "$3", note: "a coffee for the reverse engineering" },
  { label: "$10", note: "keeps the tape rolling" },
  { label: "$25", note: "funds the next firmware experiment" },
];

/**
 * Support affordance. Payment is intentionally not wired yet — the panel
 * states that plainly rather than pretending to charge anyone.
 */
export function SupportButton() {
  const [open, setOpen] = useState(false);

  return (
    <div className="relative">
      <button
        type="button"
        className="st-toggle inline-flex items-center gap-2"
        data-testid="support-project"
        aria-expanded={open}
        onClick={() => setOpen((v) => !v)}
      >
        <SupportMark />
        support this project
      </button>

      {open && (
        <div
          className="absolute right-0 z-30 mt-2 w-64 border border-[var(--bench-line)] bg-[var(--bench-raised)] p-3 shadow-lg"
          data-testid="support-panel"
        >
          <p className="font-mono text-[11px] text-[var(--ink)]">Support Stem Tape</p>
          <p className="mt-1 font-mono text-[10px] leading-relaxed text-[var(--ink-dim)]">
            Independent R&amp;D, no label, no sponsor. Contributions go straight into build time.
          </p>
          <div className="mt-3 grid gap-1">
            {TIERS.map((t) => (
              <button
                key={t.label}
                type="button"
                className="flex items-baseline justify-between gap-2 border border-[var(--bench-line)] px-2 py-1 text-left"
                onClick={() => setOpen(false)}
              >
                <span className="font-mono text-[12px] text-[var(--ink)]">{t.label}</span>
                <span className="font-mono text-[9px] text-[var(--ink-faint)]">{t.note}</span>
              </button>
            ))}
          </div>
          <p className="mt-2 font-mono text-[9px] text-[var(--ink-faint)]">
            checkout is not connected yet — this panel is the placeholder.
          </p>
        </div>
      )}
    </div>
  );
}
