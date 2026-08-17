/**
 * A simulated run may never borrow physical-device language.
 */
import { describe, expect, it } from "vitest";
import {
  DEVICE_ONLY_PHRASES,
  outcomeWording,
  resolveWording,
  simulatedRowWording,
  successLogWording,
  writeStateWording,
} from "../wording";

const mockSentences = [
  writeStateWording("mock", true),
  writeStateWording("mock", false),
  outcomeWording("mock", { outcome: "committed", detail: "ok." }),
  outcomeWording("mock", { outcome: "failed", detail: "nak." }),
  outcomeWording("mock", { outcome: "unknown", detail: "lost." }),
  successLogWording("mock"),
  simulatedRowWording("mock"),
  resolveWording("mock", "committed"),
  resolveWording("mock", "failed"),
  resolveWording("mock", "unknown"),
];

describe("outcome wording", () => {
  it("no simulated sentence contains device-only language", () => {
    for (const s of mockSentences) {
      for (const phrase of DEVICE_ONLY_PHRASES) {
        expect(s.toLowerCase()).not.toContain(phrase.toLowerCase());
      }
    }
  });

  it("every simulated sentence says it is simulated or mock", () => {
    for (const s of mockSentences) {
      expect(/simulat|mock/i.test(s)).toBe(true);
    }
  });

  it("physical wording is unchanged and does claim the device", () => {
    expect(outcomeWording("physical", { outcome: "committed", detail: "" })).toBe(
      "Committed. The device index now points at this song.",
    );
    expect(writeStateWording("physical", true)).toBe("a write has occurred on this device");
    expect(successLogWording("physical")).toContain("re-read from the SP-1");
    expect(resolveWording("physical", "committed")).toContain("committed and verified on the connected device");
  });
});
