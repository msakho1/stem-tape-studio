import { memo } from "react";
import type { HeadLaneSnapshot } from "@/audio/headLanes";

/**
 * Compact HEADS instrumentation strip.
 *
 * Heads mode is FOUR readers over ONE stem, so this strip reads
 * `HEADS · VOCALS` followed by the four positions inside that one buffer.
 *
 * Instrumentation only: every value comes from HeadLanes' own snapshot, taken
 * by the ~10 Hz engine status poll. Nothing here is inside the audio path and
 * nothing here writes engine state.
 */
function clock(s: number): string {
  const t = Math.max(0, s);
  const m = Math.floor(t / 60);
  const sec = Math.floor(t % 60);
  return `${m}:${sec.toString().padStart(2, "0")}`;
}

export const HeadsStatus = memo(function HeadsStatus({
  active,
  heads,
  source,
  notice,
}: {
  active: boolean;
  heads: HeadLaneSnapshot[];
  source: { index: number; name: string } | null;
  notice: string | null;
}) {
  if (!active) {
    if (!notice) return null;
    return (
      <p
        className="mb-3 font-mono text-[10px] uppercase tracking-[0.18em] text-[var(--signal)]"
        data-testid="heads-notice"
        role="status"
      >
        {notice}
      </p>
    );
  }

  return (
    <div
      className="mb-3 flex flex-wrap items-center gap-x-3 gap-y-1 border border-[var(--bench-line)] px-2 py-1.5"
      data-testid="heads-status"
      data-heads-active="true"
      data-heads-source={source?.name ?? ""}
      role="status"
    >
      <span className="font-mono text-[10px] uppercase tracking-[0.28em] text-[var(--signal)]">
        heads{source ? ` · ${source.name}` : ""}
      </span>
      {heads.map((h) => {
        const state = h.muted ? "muted" : h.playing ? "moving" : "parked";
        return (
          <span
            key={h.lane}
            className="font-mono text-[10px] tabular-nums tracking-[0.08em] text-[var(--ink-dim)]"
            data-testid={`head-${h.lane + 1}`}
            data-state={state}
            data-latched={h.latched}
            data-reverse={h.reverse}
            data-muted={h.muted}
            data-position={h.position.toFixed(3)}
          >
            <span className={h.playing && !h.muted ? "text-[var(--signal)]" : undefined}>
              {h.lane + 1}
              {h.muted ? "✕" : h.playing ? "▸" : "■"}
            </span>{" "}
            {clock(h.position)}
            {h.latched ? " L" : ""}
            {h.reverse ? " ↺" : ""}
          </span>
        );
      })}
    </div>
  );
});
