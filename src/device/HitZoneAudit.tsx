import { useCallback, useEffect, useState } from "react";
import { HIT_ZONES, MIN_TOUCH_TARGET_PX, type Control } from "@/device/geometry";

interface Measurement {
  control: Control;
  width: number;
  height: number;
  pass: boolean;
}

/**
 * Hit-zone audit.
 *
 * HIT_UNITS is derived arithmetic; it only yields a 44 CSS px target if the SVG
 * itself renders at 375 CSS px wide. Page width != rendered device width, so
 * this measures every zone with getBoundingClientRect() and reports the real
 * numbers, live, at whatever viewport is actually in front of you.
 */
export function HitZoneAudit({ svgRef }: { svgRef: React.RefObject<SVGSVGElement | null> }) {
  const [rows, setRows] = useState<Measurement[]>([]);
  const [svgBox, setSvgBox] = useState<{ w: number; h: number } | null>(null);
  const [viewport, setViewport] = useState<{ w: number; h: number } | null>(null);

  const measure = useCallback(() => {
    const svg = svgRef.current;
    if (!svg) return;
    const box = svg.getBoundingClientRect();
    setSvgBox({ w: box.width, h: box.height });
    setViewport({ w: window.innerWidth, h: window.innerHeight });
    const next: Measurement[] = [];
    for (const z of HIT_ZONES) {
      const el = svg.querySelector<SVGRectElement>(`[data-control="${z.control}"]`);
      if (!el) continue;
      const r = el.getBoundingClientRect();
      next.push({
        control: z.control,
        width: r.width,
        height: r.height,
        pass: r.width >= MIN_TOUCH_TARGET_PX && r.height >= MIN_TOUCH_TARGET_PX,
      });
    }
    setRows(next);
  }, [svgRef]);

  useEffect(() => {
    measure();
    const svg = svgRef.current;
    const ro = new ResizeObserver(() => measure());
    if (svg) ro.observe(svg);
    window.addEventListener("resize", measure);
    return () => {
      ro.disconnect();
      window.removeEventListener("resize", measure);
    };
  }, [measure, svgRef]);

  const failing = rows.filter((r) => !r.pass).length;

  return (
    <section className="st-section">
      <h2 className="st-section__title">hit zone audit (getBoundingClientRect)</h2>
      <div className="st-grid">
        <div className="st-kv">
          <span className="st-kv__k">viewport</span>
          <span className="st-kv__v">{viewport ? `${Math.round(viewport.w)}×${Math.round(viewport.h)}` : "—"}</span>
        </div>
        <div className="st-kv">
          <span className="st-kv__k">svg rendered</span>
          <span className="st-kv__v">{svgBox ? `${svgBox.w.toFixed(1)}×${svgBox.h.toFixed(1)}` : "—"}</span>
        </div>
        <div className="st-kv">
          <span className="st-kv__k">min target</span>
          <span className="st-kv__v">{MIN_TOUCH_TARGET_PX}px</span>
        </div>
        <div className="st-kv">
          <span className="st-kv__k">failing</span>
          <span className="st-kv__v">{failing}</span>
        </div>
      </div>
      <div className="st-rows">
        {rows.map((r) => (
          <div key={r.control} className="st-row">
            <span className="st-row__a">{r.control}</span>
            <span className="st-row__b">
              {r.width.toFixed(1)}×{r.height.toFixed(1)} px
            </span>
            <span className="st-row__d">{r.pass ? "pass ≥44" : "FAIL <44"}</span>
          </div>
        ))}
      </div>
    </section>
  );
}
