import { useEffect, useMemo, useRef } from "react";

interface Props {
  /** Decoded audio for the selected track, or null when nothing is loaded. */
  buffer: AudioBuffer | null;
  /** 0..1 playhead. */
  progress: number;
  /** Number of equal slices to mark (chop division). */
  slices?: number;
  /** Normalised loop window, 0..1, when looping is engaged. */
  loop?: { start: number; end: number } | null;
  height?: number;
  className?: string;
}

/** Downsampled peak envelope, computed once per buffer identity. */
function peaks(buffer: AudioBuffer, bins: number): number[] {
  const data = buffer.getChannelData(0);
  const step = Math.max(1, Math.floor(data.length / bins));
  const out: number[] = [];
  for (let i = 0; i < bins; i++) {
    let max = 0;
    const start = i * step;
    for (let j = start; j < start + step && j < data.length; j += 8) {
      const v = Math.abs(data[j]!);
      if (v > max) max = v;
    }
    out.push(max);
  }
  return out;
}

export function Waveform({
  buffer,
  progress,
  slices = 0,
  loop = null,
  height = 84,
  className,
}: Props) {
  const ref = useRef<HTMLCanvasElement | null>(null);
  const env = useMemo(() => (buffer ? peaks(buffer, 320) : null), [buffer]);

  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = canvas.clientWidth;
    const h = height;
    canvas.width = Math.max(1, Math.round(w * dpr));
    canvas.height = Math.round(h * dpr);
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    const styles = getComputedStyle(canvas);
    const ink = styles.getPropertyValue("--ink-dim").trim() || "#999";
    const faint = styles.getPropertyValue("--ink-faint").trim() || "#666";
    const signal = styles.getPropertyValue("--signal").trim() || "#e5484d";

    // loop window shading
    if (loop && loop.end > loop.start) {
      ctx.fillStyle = faint;
      ctx.globalAlpha = 0.12;
      ctx.fillRect(loop.start * w, 0, (loop.end - loop.start) * w, h);
      ctx.globalAlpha = 1;
    }

    if (env) {
      const mid = h / 2;
      const bw = w / env.length;
      ctx.fillStyle = ink;
      for (let i = 0; i < env.length; i++) {
        const a = Math.max(1, env[i]! * (h * 0.9));
        ctx.fillRect(i * bw, mid - a / 2, Math.max(1, bw - 0.6), a);
      }
    } else {
      ctx.strokeStyle = faint;
      ctx.globalAlpha = 0.6;
      ctx.beginPath();
      ctx.moveTo(0, h / 2);
      ctx.lineTo(w, h / 2);
      ctx.stroke();
      ctx.globalAlpha = 1;
    }

    // slice markers
    if (slices > 1) {
      ctx.strokeStyle = faint;
      ctx.setLineDash([3, 4]);
      ctx.globalAlpha = 0.8;
      for (let i = 1; i < slices; i++) {
        const x = Math.round((i / slices) * w) + 0.5;
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.lineTo(x, h);
        ctx.stroke();
      }
      ctx.setLineDash([]);
      ctx.globalAlpha = 1;
    }

    // playhead
    const px = Math.round(Math.min(1, Math.max(0, progress)) * w) + 0.5;
    ctx.strokeStyle = signal;
    ctx.beginPath();
    ctx.moveTo(px, 0);
    ctx.lineTo(px, h);
    ctx.stroke();
    ctx.fillStyle = signal;
    ctx.beginPath();
    ctx.arc(px, 4, 3, 0, Math.PI * 2);
    ctx.fill();
  }, [env, progress, slices, loop, height]);

  return (
    <canvas
      ref={ref}
      data-testid="waveform"
      className={className}
      style={{ width: "100%", height, display: "block" }}
    />
  );
}
