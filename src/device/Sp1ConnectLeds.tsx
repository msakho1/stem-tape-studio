import { useEffect, useState } from "react";
import { SP1_CONNECT_GREETING_LAP_MS, SP1_CONNECT_GREETING_MS } from "@/machine/surface";

/**
 * Track-LED positions taken verbatim from src/assets/stem-tape-sp1-outline.svg
 * (viewBox 0 0 900 1120, circles cx=208/338/468/598, cy=673, r=9).
 */
const LED_POSITIONS = [208, 338, 468, 598].map((cx) => ({
  left: `${(cx / 900) * 100}%`,
  top: `${(673 / 1120) * 100}%`,
}));

const STEP_MS = SP1_CONNECT_GREETING_LAP_MS / 4;

/** Overlays the four track LEDs of the SP-1 outline and chases them on connect. */
export function Sp1ConnectLeds({ connected }: { connected: boolean }) {
  const [chaseIndex, setChaseIndex] = useState<number | null>(null);

  useEffect(() => {
    if (!connected) {
      setChaseIndex(null);
      return;
    }
    const start = performance.now();
    setChaseIndex(0);
    const id = window.setInterval(() => {
      const elapsed = performance.now() - start;
      if (elapsed >= SP1_CONNECT_GREETING_MS) {
        setChaseIndex(null);
        window.clearInterval(id);
        return;
      }
      setChaseIndex(Math.floor(elapsed / STEP_MS) % 4);
    }, STEP_MS);
    return () => window.clearInterval(id);
  }, [connected]);

  return (
    <div className="pointer-events-none absolute inset-0" aria-hidden data-testid="sp1-connect-leds">
      {LED_POSITIONS.map((pos, i) => {
        const on = connected && (chaseIndex === null || chaseIndex === i);
        return (
          <span
            key={i}
            data-led={`track-${i + 1}`}
            data-on={on}
            className="absolute block h-[2%] w-[2%] -translate-x-1/2 -translate-y-1/2 rounded-full transition-[background-color,box-shadow] duration-100"
            style={{
              left: pos.left,
              top: pos.top,
              backgroundColor: on ? "var(--accent, #ef7479)" : "transparent",
              boxShadow: on ? "0 0 0 3px rgba(239, 116, 121, 0.25)" : "none",
            }}
          />
        );
      })}
    </div>
  );
}
