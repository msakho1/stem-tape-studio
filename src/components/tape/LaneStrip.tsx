import { useEffect, useMemo, useRef } from "react";

interface Props {
  buffer: AudioBuffer | null;
  /** CSS colour for this lane's activity ticks. */
  color: string;
  /** 0..1 playhead across the full song. */
  progress: number;
  height?: number;
}

/** Coarse activity ticks — where this stem actually plays, not a full envelope. */
function activity(buffer: AudioBuffer, bins: number): number[] {
  const data = buffer.getChannelData(0);
  const step = Math.max(1, Math.floor(data.length / bins));
  const out: number[] = [];
  for (let i = 0; i < bins; i++) {
    let max = 0;
    const start = i * step;
    for (let j = start; j < start + step && j < data.length; j += 32) {
      const v = Math.abs(data[j]!);
      if (v > max) max = v;
    }
    out.push(max);
  }
  return out;
}

/**
 * One horizontal lane line with activity ticks, matching the four coloured
 * rows under the transport waveform in the reference deck.
 */
export function LaneStrip({ buffer, color, progress, height = 12 }: Props) {
  const ref = useRef<HTMLCanvasElement | null>(null);
  const env = useMemo(() => (buffer ? activity(buffer, 220) : null), [buffer]);

  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = canvas.clientWidth;
    canvas.width = Math.max(1, Math.round(w * dpr));
    canvas.height = Math.round(height * dpr);
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, height);

    const mid = height / 2;
    ctx.strokeStyle = color;
    ctx.globalAlpha = 0.85;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, mid + 0.5);
    ctx.lineTo(w, mid + 0.5);
    ctx.stroke();

    if (env) {
      const bw = w / env.length;
      for (let i = 0; i < env.length; i++) {
        const v = env[i]!;
        if (v < 0.06) continue;
        const a = Math.min(mid - 1, v * mid * 1.4);
        ctx.globalAlpha = 0.9;
        ctx.fillStyle = color;
        ctx.fillRect(i * bw, mid - a, Math.max(1, bw * 0.5), a * 2);
      }
    }

    ctx.globalAlpha = 1;
  }, [env, color, height]);

  return (
    <div className="relative">
      <canvas ref={ref} className="block w-full" style={{ height }} />
      <span
        className="pointer-events-none absolute top-0 w-px bg-[var(--signal)]"
        style={{ left: `${Math.min(1, Math.max(0, progress)) * 100}%`, height }}
      />
    </div>
  );
}
