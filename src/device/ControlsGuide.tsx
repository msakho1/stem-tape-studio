import { Fragment, useState } from "react";
import { Sp1GuideIllustration, type MiniMotion } from "@/device/Sp1GuideIllustration";
import {
  FX_BANK_CARDS,
  HARDWARE_SECTION,
  LESSONS,
  REFERENCE_SECTIONS,
  keyboardTable,
  type Lesson,
} from "@/device/guideContent";

/**
 * The control reference reuses the authoritative SP-1 asset illustration for
 * every lesson; there is no hand-drawn diagram in this file.
 */
export type { MiniMotion, Lesson };
export { LESSONS };

function Drawer({
  id,
  title,
  meta,
  open,
  onToggle,
  children,
}: {
  id: string;
  title: string;
  meta: string;
  open: boolean;
  onToggle: () => void;
  children: React.ReactNode;
}) {
  return (
    <div className="border border-[var(--bench-line)]">
      <button
        type="button"
        className="flex w-full items-center justify-between gap-3 px-3 py-2 text-left"
        aria-expanded={open}
        data-testid={id}
        onClick={onToggle}
      >
        <span className="font-mono text-[12px] text-[var(--ink)]">{title}</span>
        <span className="shrink-0 font-mono text-[10px] uppercase tracking-[0.12em] text-[var(--ink-faint)]">
          {meta} <span aria-hidden>{open ? "⌃" : "⌄"}</span>
        </span>
      </button>
      {open && <div className="border-t border-[var(--bench-line)] px-3 py-3">{children}</div>}
    </div>
  );
}

/**
 * The control reference on the tape page: twenty animated lessons, four FX
 * bank accordions, one keyboard table and compact reference sections. Details
 * can be collapsed entirely so the surface shows only its hit zones.
 */
export function ControlsGuide({
  showHitZones,
  onToggleHitZones,
}: {
  showHitZones: boolean;
  onToggleHitZones: () => void;
}) {
  const [open, setOpen] = useState<string | null>("play");
  const [details, setDetails] = useState(true);
  const table = keyboardTable();

  const toggle = (id: string) => { console.log("GUIDE_TOGGLE", id); setOpen((cur) => (cur === id ? null : id)); };

  return (
    <div className="mt-4 border-t border-[var(--bench-line)] pt-3" data-testid="controls-guide">
      <div className="flex flex-wrap items-center gap-2">
        <p className="font-mono text-[10px] uppercase tracking-[0.18em] text-[var(--ink-faint)]">controls</p>
        <button type="button" className="st-toggle" data-on={details} onClick={() => setDetails((v) => !v)}>
          {details ? "hide details" : "show details"}
        </button>
        <button type="button" className="st-toggle" data-on={showHitZones} onClick={onToggleHitZones}>
          {showHitZones ? "hide hit zones" : "show hit zones"}
        </button>
      </div>

      {details && (
        <>
          <div className="mt-3 grid gap-1" data-testid="guide-lessons">
            {LESSONS.map((l) => (
              <Drawer
                key={l.id}
                id={`lesson-${l.id}`}
                title={l.title}
                meta={l.gesture}
                open={open === l.id}
                onToggle={() => toggle(l.id)}
              >
                <div className="grid gap-2 md:grid-cols-[1fr_240px]">
                  <p className="font-mono text-[11px] leading-relaxed text-[var(--ink-dim)]">{l.body}</p>
                  <Sp1GuideIllustration highlight={l.highlight} motion={l.motion} held={l.held ?? []} />
                </div>
              </Drawer>
            ))}
          </div>

          <p className="mt-4 font-mono text-[10px] uppercase tracking-[0.18em] text-[var(--ink-faint)]">fx banks</p>
          <div className="mt-2 grid gap-1" data-testid="guide-fx">
            {FX_BANK_CARDS.map((b) => (
              <Drawer
                key={b.bankId}
                id={`fx-bank-${b.bankId}`}
                title={b.label}
                meta={`Track ${b.button} · 3 algorithms`}
                open={open === `fx:${b.bankId}`}
                onToggle={() => toggle(`fx:${b.bankId}`)}
              >
                <ul className="grid gap-2">
                  {b.algorithms.map((a) => (
                    <li key={a.id} data-testid={`fx-algo-${a.id}`} className="font-mono text-[11px]">
                      <span className="text-[var(--ink)]">{a.label}</span>{" "}
                      <span className="text-[var(--ink-dim)]">— {a.blurb}</span>
                    </li>
                  ))}
                </ul>
                <p className="mt-2 font-mono text-[10px] text-[var(--ink-faint)]">
                  Hold Track {b.button} for momentary · FUNCTION + Track {b.button} to latch · VOL −/+ cycles the
                  algorithm.
                </p>
              </Drawer>
            ))}
          </div>

          <p className="mt-4 font-mono text-[10px] uppercase tracking-[0.18em] text-[var(--ink-faint)]">keyboard</p>
          <div className="mt-2 border border-[var(--bench-line)]" data-testid="guide-keyboard">
            <table className="w-full border-collapse font-mono text-[11px]">
              <tbody>
                {table.map(({ group, rows }) => (
                  <Fragment key={group.feature}>
                    <tr data-testid={`kbd-group-${group.feature}`}>
                      <th
                        colSpan={2}
                        className="border-b border-[var(--bench-line)] px-3 py-1 text-left text-[10px] uppercase tracking-[0.14em] text-[var(--ink-faint)]"
                      >
                        {group.label}
                      </th>
                    </tr>
                    {rows.map((r) => (
                      <tr key={`${group.feature}:${r.id}:${r.codes.join("+")}`}>
                        <td className="w-[110px] whitespace-nowrap px-3 py-1 align-top text-[var(--ink)]">{r.label}</td>
                        <td className="px-3 py-1 text-[var(--ink-dim)]">{r.detail}</td>
                      </tr>
                    ))}
                  </Fragment>
                ))}
              </tbody>
            </table>
          </div>

          <div className="mt-4 grid gap-3 md:grid-cols-2" data-testid="guide-reference">
            {REFERENCE_SECTIONS.map((s) => (
              <div key={s.id} className="border border-[var(--bench-line)] px-3 py-2">
                <p className="font-mono text-[10px] uppercase tracking-[0.18em] text-[var(--ink-faint)]">{s.title}</p>
                <ul className="mt-1 grid gap-1">
                  {s.entries.map((e) => (
                    <li key={e.id} data-testid={`ref-${e.id}`} className="font-mono text-[11px] text-[var(--ink-dim)]">
                      {e.note}
                    </li>
                  ))}
                </ul>
              </div>
            ))}
          </div>

          <div className="mt-3 border border-dashed border-[var(--bench-line)] px-3 py-2" data-testid="guide-hardware">
            <p className="font-mono text-[10px] uppercase tracking-[0.18em] text-[var(--ink-faint)]">
              {HARDWARE_SECTION.title}
            </p>
            <ul className="mt-1 grid gap-1">
              {HARDWARE_SECTION.entries.map((e) => (
                <li key={e.id} data-testid={`ref-${e.id}`} className="font-mono text-[11px] text-[var(--ink-faint)]">
                  {e.note}
                </li>
              ))}
            </ul>
          </div>
        </>
      )}
    </div>
  );
}
