import { useMemo } from "react";
import { Waveform } from "@/components/tape/Waveform";
import { LaneStrip } from "@/components/tape/LaneStrip";
import type { SongGrid } from "@/audio/gridAnalysis";

export const LANE_COLORS = [
  "var(--lane-1)",
  "var(--lane-2)",
  "var(--lane-3)",
  "var(--lane-4)",
] as const;
export const LANE_TAGS = ["VOX", "DRM", "BAS", "INS"] as const;

interface Props {
  buffers: (AudioBuffer | null)[];
  activeIndex: number;
  position: number;
  duration: number;
  bpm: number;
  grid: SongGrid | null;
  rate: number;
  playing: boolean;
  slices?: number;
  loop?: { start: number; end: number } | null;
}

function clock(s: number) {
  if (!Number.isFinite(s) || s < 0) s = 0;
  const m = Math.floor(s / 60);
  const sec = Math.floor(s % 60);
  return `${m.toString().padStart(2, "0")}:${sec.toString().padStart(2, "0")}`;
}

/** bar.beat.sixteenth from the detected grid, or a 4/4 grid from BPM at t=0. */
function barPosition(position: number, bpm: number, grid: SongGrid | null) {
  const beatsPerBar = grid?.beatsPerBar ?? 4;
  const beatSeconds = grid?.beatSeconds ?? (bpm > 0 ? 60 / bpm : 0.5);
  const barSeconds = grid?.barSeconds ?? beatSeconds * beatsPerBar;
  const origin = grid?.firstDownbeatS ?? 0;
  const rel = position - origin;
  const bar = Math.floor(rel / barSeconds);
  const inBar = rel - bar * barSeconds;
  const beat = Math.floor(inBar / beatSeconds);
  const sixteenth = Math.floor(((inBar - beat * beatSeconds) / beatSeconds) * 4);
  return {
    barSeconds,
    label: `${bar + 1}.${(((beat % beatsPerBar) + beatsPerBar) % beatsPerBar) + 1}.${Math.max(0, sixteenth) + 1}`,
    bar: bar + 1,
  };
}

/** Static reel artwork; rotation is applied by the parent group. */
function Reel({ cx, cy }: { cx: number; cy: number }) {
  return (
    <g transform={`translate(${cx} ${cy})`}>
      <circle r="72" fill="none" stroke="currentColor" strokeWidth="2.5" />
      <circle r="66" fill="none" stroke="currentColor" strokeWidth="1" opacity="0.5" />
      <circle r="40" fill="var(--ink)" opacity="0.55" />
      <circle r="14" fill="none" stroke="currentColor" strokeWidth="2" />
      <circle r="4" fill="currentColor" />
      {[0, 72, 144, 216, 288].map((a) => (
        <g key={a} transform={`rotate(${a})`}>
          <path
            d="M -13 -18 L -30 -62 A 66 66 0 0 1 30 -62 L 13 -18 A 40 40 0 0 0 -13 -18 Z"
            fill="var(--bench)"
          />
          <path
            d="M -13 -18 L -30 -62 A 66 66 0 0 1 30 -62 L 13 -18 A 40 40 0 0 0 -13 -18 Z"
            fill="none"
            stroke="currentColor"
            strokeWidth="2"
          />
        </g>
      ))}
    </g>
  );
}

/**
 * The transport deck: two spinning reels, a level meter, the tape path and the
 * bar-ruled song timeline with one coloured lane per stem.
 *
 * Purely presentational — every value arrives as a prop from the engine status.
 */
export function TapeDeck({
  buffers,
  activeIndex,
  position,
  duration,
  bpm,
  grid,
  rate,
  playing,
  slices = 1,
  loop = null,
}: Props) {
  const progress = duration > 0 ? position / duration : 0;
  const { label: barLabel, barSeconds, bar } = barPosition(position, bpm, grid);

  // Reel spin: one revolution per second at 1.00x, direction follows the tape.
  const speed = Math.abs(rate) < 0.02 ? 0 : Math.abs(rate);
  const spin = speed > 0 ? { animationDuration: `${(2.4 / speed).toFixed(3)}s` } : {};
  const reelStyle: React.CSSProperties = {
    ...spin,
    animationPlayState: playing && speed > 0 ? "running" : "paused",
    animationDirection: rate < 0 ? "reverse" : "normal",
  };

  const ticks = useMemo(() => {
    if (!(duration > 0) || !(barSeconds > 0)) return [] as { bar: number; x: number }[];
    const totalBars = Math.floor(duration / barSeconds);
    const stepBars = Math.max(1, Math.ceil(totalBars / 12 / 4) * 4);
    const out: { bar: number; x: number }[] = [];
    for (let b = 0; b <= totalBars; b += stepBars) {
      out.push({ bar: b + 1, x: (b * barSeconds) / duration });
    }
    return out;
  }, [duration, barSeconds]);

  // Meter needle: peak of the active buffer around the playhead.
  const level = useMemo(() => {
    const buf = buffers[activeIndex] ?? null;
    if (!buf || !playing) return 0;
    const data = buf.getChannelData(0);
    const centre = Math.floor(Math.min(0.999, Math.max(0, progress)) * data.length);
    let max = 0;
    for (let i = centre; i < Math.min(data.length, centre + 2048); i += 16) {
      const v = Math.abs(data[i]!);
      if (v > max) max = v;
    }
    return Math.min(1, max);
  }, [buffers, activeIndex, progress, playing]);

  const needle = -45 + level * 90;

  return (
    <div className="st-deck" data-testid="tape-deck">
      <div className="st-deck__head">
        <div className="st-deck__id">
          <p>stem tape</p>
          <p>
            <span className="text-[var(--ink)]">{bpm.toFixed(bpm % 1 === 0 ? 0 : 2)}</span> bpm
          </p>
        </div>
        <p className="st-deck__clock" data-testid="deck-clock">
          {clock(position)} / {clock(duration)}
        </p>
        <p className="st-deck__bar" data-testid="deck-bar">
          bar {barLabel}
        </p>
      </div>

      <div className="st-deck__panel">
        <svg
          viewBox="0 0 720 260"
          className="st-deck__svg"
          role="img"
          aria-label={`Tape transport, bar ${bar}`}
        >
          <g className="text-[var(--ink)]">
            <g transform="translate(170 118)">
              <g className="st-reel" style={reelStyle}>
                <Reel cx={0} cy={0} />
              </g>
            </g>
            <g transform="translate(550 118)">
              <g className="st-reel" style={reelStyle}>
                <Reel cx={0} cy={0} />
              </g>
            </g>

            {/* tape path */}
            <path
              d="M170 190 L246 226 H300 M420 226 H474 L550 190"
              fill="none"
              stroke="currentColor"
              strokeWidth="4"
            />
            <circle cx="246" cy="226" r="11" fill="none" stroke="currentColor" strokeWidth="3" />
            <circle cx="300" cy="226" r="11" fill="none" stroke="currentColor" strokeWidth="3" />
            <circle cx="420" cy="226" r="11" fill="none" stroke="currentColor" strokeWidth="3" />
            <circle cx="474" cy="226" r="11" fill="none" stroke="currentColor" strokeWidth="3" />
            <rect
              x="330"
              y="206"
              width="60"
              height="40"
              rx="4"
              fill="none"
              stroke="currentColor"
              strokeWidth="2.5"
            />
            <path d="M346 220h28M350 226h20M354 232h12" stroke="currentColor" strokeWidth="2" />

            {/* level meter */}
            <rect
              x="300"
              y="18"
              width="120"
              height="62"
              rx="3"
              fill="none"
              stroke="currentColor"
              strokeWidth="2"
            />
            {[0, 1, 2, 3, 4, 5, 6, 7, 8].map((i) => (
              <path
                key={i}
                d={`M${314 + i * 11} ${i % 4 === 0 ? 34 : 40} v${i % 4 === 0 ? 18 : 12}`}
                stroke="currentColor"
                strokeWidth={i % 4 === 0 ? 2 : 1.2}
                opacity={i > 6 ? 0.9 : 0.55}
              />
            ))}
            <g transform="translate(360 62)">
              <path
                className="st-deck__needle"
                d="M0 0 V-34"
                stroke="var(--signal)"
                strokeWidth="2"
                style={{ transform: `rotate(${needle}deg)` }}
              />
            </g>
            <text
              x="360"
              y="76"
              textAnchor="middle"
              fontSize="9"
              fill="currentColor"
              opacity="0.65"
            >
              -12 · 0 · +12
            </text>
          </g>

          {/* stem lamps */}
          {LANE_COLORS.map((c, i) => (
            <circle
              key={i}
              cx={318 + i * 28}
              cy={104}
              r={7}
              fill={c}
              opacity={buffers[i] ? (playing ? 1 : 0.6) : 0.2}
            />
          ))}
        </svg>
      </div>

      <div className="st-deck__timeline">
        <Waveform
          buffer={buffers[activeIndex] ?? null}
          progress={progress}
          slices={slices}
          loop={loop}
          height={70}
        />
        <div className="st-deck__ruler">
          {ticks.map((t) => (
            <span key={t.bar} style={{ left: `${t.x * 100}%` }}>
              {t.bar}
            </span>
          ))}
        </div>
        <div className="mt-2 grid gap-1.5">
          {LANE_COLORS.map((c, i) => (
            <LaneStrip key={i} buffer={buffers[i] ?? null} color={c} progress={progress} />
          ))}
        </div>
      </div>

      <div className="st-deck__legend">
        {LANE_TAGS.map((tag, i) => (
          <span key={tag} data-on={i === activeIndex}>
            <i style={{ background: LANE_COLORS[i] }} />
            {tag}
          </span>
        ))}
        <span className="ml-auto st-deck__state" data-on={playing}>
          {playing ? "▶ play" : "■ stop"}
        </span>
      </div>
    </div>
  );
}
