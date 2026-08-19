import { describe, expect, it } from "vitest";
import { deriveLeds, initialSurfaceState, SP1_CONNECT_GREETING_MS } from "@/machine/surface";
import { resolveSp1LedFrame, sp1LedStateFrom } from "../sp1LedEngine";
import { sp1Surface } from "@/audio/midi/sp1Surface";

describe("SP-1 connect greeting", () => {
  it("emits one connection event per device id and null on unplug", () => {
    const seen: (string | null)[] = [];
    const off = sp1Surface.onConnectionChange((c) => seen.push(c ? c.deviceId : null));
    sp1Surface.deviceConnected("p1", "STEM TAPE SP-1");
    sp1Surface.deviceConnected("p1", "STEM TAPE SP-1");
    sp1Surface.deviceDisconnected("p1");
    off();
    expect(seen).toEqual(["p1", null]);
  });

  it("cycles the four track LEDs one after the other, then releases them", () => {
    const t0 = 10_000;
    const state = { ...initialSurfaceState(), sp1ConnectedAt: t0 };
    const lap = 600;
    const lit = (t: number) =>
      resolveSp1LedFrame(sp1LedStateFrom(state, t), t).leds.slice(0, 4).findIndex((l) => l.brightness === 127);
    expect([lit(t0), lit(t0 + lap / 4), lit(t0 + lap / 2), lit(t0 + (3 * lap) / 4)]).toEqual([0, 1, 2, 3]);
    expect(resolveSp1LedFrame(sp1LedStateFrom(state, t0 + 10), 0).leds[0]!.precedenceKey).toBe("connectGreeting");

    const after = t0 + SP1_CONNECT_GREETING_MS + 1;
    expect(resolveSp1LedFrame(sp1LedStateFrom(state, after), after).leds[0]!.precedenceKey).not.toBe("connectGreeting");
    expect(deriveLeds(state, t0 + 10)["track-led-1"].pattern).toBe("chase");
    expect(deriveLeds(state, after)["track-led-1"].pattern).not.toBe("chase");
  });
});
