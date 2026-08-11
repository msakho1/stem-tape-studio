/**
 * Bootstrap for the supplied photographic SP-1 SVG.
 *
 * The uploaded asset ships a <script> block that assumes it is the root of a
 * standalone SVG document (`document.documentElement`, `document.getElementById`).
 * Inline in the React DOM that script never executes, so this module re-runs the
 * SAME interaction logic — identical geometry constants, identical transforms,
 * identical events and identical `svg.stemTape` host API — scoped to the inlined
 * <svg> element. Nothing visual in the asset is changed.
 */

export type StemChannel = 1 | 2 | 3 | 4;

export interface Sp1PhotoHandlers {
  onFaderStart?: (channel: StemChannel, value: number, pointerId: number) => void;
  onFaderMove?: (channel: StemChannel, value: number, pointerId: number) => void;
  onFaderEnd?: (channel: StemChannel, value: number, pointerId: number, cancelled: boolean) => void;
  onButtonDown?: (channel: StemChannel, pointerId: number) => void;
  onButtonUp?: (channel: StemChannel, pointerId: number, cancelled: boolean) => void;
  onRocker?: (value: -1 | 0 | 1, pointerId: number) => void;
}

export interface Sp1PhotoApi {
  setFader(channel: StemChannel, value: number): void;
  setButton(channel: StemChannel, active: boolean): void;
  setRocker(value: -1 | 0 | 1): void;
}

type WithApi = SVGSVGElement & { stemTape?: Sp1PhotoApi };

const clamp = (v: number, a: number, b: number) => Math.max(a, Math.min(b, v));

/** Photographed fader-slot geometry, taken verbatim from the asset script. */
const ORIGINAL_CY = 428;
const TOP = 424;
const BOTTOM = 518;

/** Strips the standalone-document script; everything else is left untouched. */
export function stripSvgScripts(markup: string): string {
  return markup.replace(/<script[\s\S]*?<\/script>/gi, "");
}

export function bootstrapSp1Photo(svg: SVGSVGElement, handlers: Sp1PhotoHandlers): () => void {
  const cleanups: Array<() => void> = [];
  const on = <K extends keyof HTMLElementEventMap>(
    el: Element,
    type: K | string,
    fn: (e: never) => void,
    opts?: AddEventListenerOptions,
  ) => {
    el.addEventListener(type, fn as EventListener, opts);
    cleanups.push(() => el.removeEventListener(type, fn as EventListener));
  };

  const emit = (name: string, detail: unknown) => {
    svg.dispatchEvent(new CustomEvent(name, { detail, bubbles: true }));
    const alias =
      name === "stem-fader" ? "faderchange" : name === "stem-button" ? "buttonchange" : name === "stem-rocker" ? "rockerchange" : null;
    if (alias) svg.dispatchEvent(new CustomEvent(alias, { detail, bubbles: true }));
  };

  const svgPoint = (clientX: number, clientY: number) => {
    const ctm = svg.getScreenCTM();
    if (!ctm) return null;
    return new DOMPoint(clientX, clientY).matrixTransform(ctm.inverse());
  };

  // ---------------- FADERS ----------------
  const faderSetters = new Map<StemChannel, (v: number, source?: string) => void>();

  for (const raw of Array.from(svg.querySelectorAll<SVGRectElement>(".fader-hit"))) {
    const ch = Number(raw.dataset["channel"]) as StemChannel;
    const layer = svg.querySelector<SVGGElement>(`#fader-layer-${ch}`);
    if (!layer) continue;
    let pointerId: number | null = null;

    const setValue = (value: number, source = "pointer") => {
      const v = clamp(value, 0, 1);
      const dy = BOTTOM - v * (BOTTOM - TOP) - ORIGINAL_CY;
      layer.setAttribute("transform", `translate(0 ${dy.toFixed(3)})`);
      raw.dataset["value"] = String(v);
      raw.setAttribute("aria-valuenow", String(Math.round(v * 100)));
      emit("stem-fader", { channel: ch, value: v, source });
      return v;
    };
    faderSetters.set(ch, setValue);

    const valueFrom = (e: PointerEvent) => {
      const p = svgPoint(e.clientX, e.clientY);
      if (!p) return null;
      return clamp(1 - (p.y - TOP) / (BOTTOM - TOP), 0, 1);
    };

    on(raw, "pointerdown", (e: PointerEvent) => {
      e.preventDefault();
      if (pointerId != null) return;
      pointerId = e.pointerId;
      try {
        raw.setPointerCapture(e.pointerId);
      } catch {
        /* capture optional */
      }
      const v = valueFrom(e);
      if (v == null) return;
      setValue(v);
      handlers.onFaderStart?.(ch, v, e.pointerId);
    });
    on(raw, "pointermove", (e: PointerEvent) => {
      if (pointerId !== e.pointerId) return;
      const v = valueFrom(e);
      if (v == null) return;
      setValue(v);
      handlers.onFaderMove?.(ch, v, e.pointerId);
    });
    const finish = (e: PointerEvent, cancelled: boolean) => {
      if (pointerId !== e.pointerId) return;
      pointerId = null;
      try {
        raw.releasePointerCapture(e.pointerId);
      } catch {
        /* already released */
      }
      handlers.onFaderEnd?.(ch, Number(raw.dataset["value"] ?? 1), e.pointerId, cancelled);
    };
    on(raw, "pointerup", (e: PointerEvent) => finish(e, false));
    on(raw, "pointercancel", (e: PointerEvent) => finish(e, true));

    on(raw, "keydown", (e: KeyboardEvent) => {
      let v = Number(raw.dataset["value"] ?? 1);
      if (e.key === "ArrowUp" || e.key === "ArrowRight") v += 0.025;
      else if (e.key === "ArrowDown" || e.key === "ArrowLeft") v -= 0.025;
      else if (e.key === "Home") v = 0;
      else if (e.key === "End") v = 1;
      else return;
      e.preventDefault();
      const next = setValue(v, "keyboard");
      handlers.onFaderStart?.(ch, next, -100 - ch);
      handlers.onFaderEnd?.(ch, next, -100 - ch, false);
    });
  }

  // ---------------- BUTTONS ----------------
  const buttonSetters = new Map<StemChannel, (active: boolean) => void>();

  for (const raw of Array.from(svg.querySelectorAll<SVGRectElement>('[id^="button-hit-"]'))) {
    const ch = Number(raw.dataset["channel"]) as StemChannel;
    const layer = svg.querySelector<SVGGElement>(`#button-layer-${ch}`);
    if (!layer) continue;
    let pointerId: number | null = null;

    const pressVisual = () => {
      layer.setAttribute("transform", "translate(0 2.4) scale(0.985 0.97)");
      layer.style.transformOrigin = "center";
      layer.classList.add("pressed");
    };
    const releaseVisual = () => {
      layer.setAttribute("transform", "translate(0 0)");
      layer.classList.remove("pressed");
    };

    buttonSetters.set(ch, (active: boolean) => {
      raw.setAttribute("aria-pressed", String(active));
    });

    on(raw, "pointerdown", (e: PointerEvent) => {
      e.preventDefault();
      if (pointerId != null) return;
      pointerId = e.pointerId;
      try {
        raw.setPointerCapture(e.pointerId);
      } catch {
        /* capture optional */
      }
      pressVisual();
      handlers.onButtonDown?.(ch, e.pointerId);
    });
    const up = (e: PointerEvent, cancelled: boolean) => {
      if (pointerId !== e.pointerId) return;
      pointerId = null;
      releaseVisual();
      try {
        raw.releasePointerCapture(e.pointerId);
      } catch {
        /* already released */
      }
      handlers.onButtonUp?.(ch, e.pointerId, cancelled);
      if (!cancelled) emit("stem-button", { channel: ch, active: raw.getAttribute("aria-pressed") === "true" });
    };
    on(raw, "pointerup", (e: PointerEvent) => up(e, false));
    on(raw, "pointercancel", (e: PointerEvent) => up(e, true));

    on(raw, "keydown", (e: KeyboardEvent) => {
      if (e.key !== " " && e.key !== "Enter") return;
      e.preventDefault();
      pressVisual();
      const id = -200 - ch;
      handlers.onButtonDown?.(ch, id);
      window.setTimeout(() => {
        releaseVisual();
        handlers.onButtonUp?.(ch, id, false);
      }, 90);
    });
  }

  // ---------------- SIDE ROCKER ----------------
  const rockerHit = svg.querySelector<SVGRectElement>("#rocker-hit");
  const rockerLayer = svg.querySelector<SVGGElement>("#rocker-layer");
  let rockerPointer: number | null = null;

  const setRocker = (input: number, source = "pointer", pointerId = -300) => {
    const v: -1 | 0 | 1 = input < 0 ? -1 : input > 0 ? 1 : 0;
    rockerHit?.setAttribute("aria-valuenow", String(v));
    rockerLayer?.setAttribute("transform", `translate(0 ${v * -3})`);
    emit("stem-rocker", { value: v, source });
    handlers.onRocker?.(v, pointerId);
  };

  if (rockerHit) {
    on(rockerHit, "pointerdown", (e: PointerEvent) => {
      e.preventDefault();
      if (rockerPointer != null) return;
      rockerPointer = e.pointerId;
      try {
        rockerHit.setPointerCapture(e.pointerId);
      } catch {
        /* capture optional */
      }
      const p = svgPoint(e.clientX, e.clientY);
      const box = rockerHit.getBBox();
      const up = p ? p.y < box.y + box.height / 2 : true;
      setRocker(up ? 1 : -1, "pointer", e.pointerId);
    });
    const release = (e: PointerEvent) => {
      if (rockerPointer !== e.pointerId) return;
      rockerPointer = null;
      setRocker(0, "pointer", e.pointerId);
      try {
        rockerHit.releasePointerCapture(e.pointerId);
      } catch {
        /* already released */
      }
    };
    on(rockerHit, "pointerup", release);
    on(rockerHit, "pointercancel", release);
    on(rockerHit, "keydown", (e: KeyboardEvent) => {
      if (e.key === "ArrowUp") {
        e.preventDefault();
        setRocker(1, "keyboard");
      } else if (e.key === "ArrowDown") {
        e.preventDefault();
        setRocker(-1, "keyboard");
      } else if (e.key === " " || e.key === "Enter" || e.key === "Escape") {
        e.preventDefault();
        setRocker(0, "keyboard");
      }
    });
  }

  // ---------------- PUBLIC HOST API ----------------
  const api: Sp1PhotoApi = {
    setFader(channel, value) {
      faderSetters.get(channel)?.(value, "api");
    },
    setButton(channel, active) {
      buttonSetters.get(channel)?.(active);
    },
    setRocker(value) {
      setRocker(value, "api");
    },
  };
  (svg as WithApi).stemTape = api;

  return () => {
    for (const c of cleanups) c();
    delete (svg as WithApi).stemTape;
  };
}
