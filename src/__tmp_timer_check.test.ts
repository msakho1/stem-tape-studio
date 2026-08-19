import { describe, it, vi } from "vitest";
describe("timer check", () => {
  it("has advanceTimersByTime", () => {
    console.log("keys:", Object.keys(vi).filter(k => k.includes("Timer")));
    vi.useFakeTimers();
    console.log("after fake keys:", Object.keys(vi).filter(k => k.includes("Timer")));
    console.log("advanceTimersByTime:", typeof (vi as any).advanceTimersByTime);
  });
});
