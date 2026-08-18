import { describe, expect, it } from "vitest";
import { assessCapacity, uploadEnabled, type CapabilityQueryState } from "../capacity";

const REQ = 43;
const gate = (over: Partial<Parameters<typeof uploadEnabled>[0]>) =>
  uploadEnabled({
    deviceConnected: true,
    capabilitiesNegotiated: true,
    capacity: "fits",
    songPrepared: true,
    transferActive: false,
    ...over,
  });

describe("tri-state staging capacity", () => {
  it("disconnected with a prepared song is unknown, never 'does not fit'", () => {
    const a = assessCapacity({ requiredSectors: REQ, availableSectors: null, queryState: "none" });
    expect(a.status).toBe("unknown");
    expect(a.availableSectors).toBeNull();
    expect(a.line).toBe(
      "43 sectors required. Connect a compatible Stem Tape SP-1 to check available storage.",
    );
    expect(a.note).toBe(
      "Upload remains disabled until device capacity is confirmed. No data has been written.",
    );
    expect(a.line + a.note).not.toMatch(/does not fit|does NOT fit|fits/i);
  });

  it("capability query pending is unknown", () => {
    const a = assessCapacity({
      requiredSectors: REQ,
      availableSectors: 256,
      queryState: "pending",
    });
    expect(a.status).toBe("unknown");
    expect(a.availableSectors).toBeNull();
    expect(a.line).toContain("Connect a compatible Stem Tape SP-1");
  });

  it("failed / incompatible capability query is unknown with its own wording", () => {
    const a = assessCapacity({
      requiredSectors: REQ,
      availableSectors: 256,
      queryState: "unverified",
    });
    expect(a.status).toBe("unknown");
    expect(a.availableSectors).toBeNull();
    expect(a.line).toBe("Device storage capacity could not be verified.");
    expect(a.note).toContain("No data has been written.");
  });

  it("compatible device with sufficient capacity fits", () => {
    const a = assessCapacity({
      requiredSectors: REQ,
      availableSectors: 256,
      queryState: "compatible",
    });
    expect(a.status).toBe("fits");
    expect(a.line).toBe("43 sectors required · 256 available · fits");
    expect(a.note).toBe("");
  });

  it("compatible device with insufficient capacity does not fit", () => {
    const a = assessCapacity({
      requiredSectors: REQ,
      availableSectors: 8,
      queryState: "compatible",
    });
    expect(a.status).toBe("insufficient");
    expect(a.line).toBe("43 sectors required · 8 available · does not fit");
    expect(a.note).toBe("No data will be written.");
  });

  it("disconnecting after a 'fits' result returns to unknown with no stale capacity", () => {
    const before = assessCapacity({
      requiredSectors: REQ,
      availableSectors: 256,
      queryState: "compatible",
    });
    expect(before.status).toBe("fits");
    const after = assessCapacity({
      requiredSectors: REQ,
      availableSectors: 256,
      queryState: "none",
    });
    expect(after.status).toBe("unknown");
    expect(after.availableSectors).toBeNull();
    expect(after.line).not.toContain("256");
  });

  it("reconnecting to a different-capacity device recalculates", () => {
    expect(
      assessCapacity({ requiredSectors: REQ, availableSectors: 256, queryState: "compatible" })
        .status,
    ).toBe("fits");
    expect(
      assessCapacity({ requiredSectors: REQ, availableSectors: 8, queryState: "compatible" })
        .status,
    ).toBe("insufficient");
  });

  it("unknown capacity never enables upload", () => {
    expect(gate({ capacity: "unknown" })).toBe(false);
    expect(gate({ capacity: "insufficient" })).toBe(false);
    expect(gate({ capacity: "fits" })).toBe(true);
    expect(gate({ capacity: "fits", deviceConnected: false })).toBe(false);
    expect(gate({ capacity: "fits", capabilitiesNegotiated: false })).toBe(false);
    expect(gate({ capacity: "fits", songPrepared: false })).toBe(false);
    expect(gate({ capacity: "fits", transferActive: true })).toBe(false);
  });

  it("unknown capacity never emits insufficient-capacity wording and never permits a write", () => {
    const unknowns: CapabilityQueryState[] = ["none", "pending", "unverified"];
    for (const queryState of unknowns) {
      for (const availableSectors of [null, 0, 8, 256]) {
        const a = assessCapacity({ requiredSectors: REQ, availableSectors, queryState });
        expect(a.status).toBe("unknown");
        expect(a.line).not.toMatch(/does not fit/i);
        expect(a.note).not.toMatch(/does not fit/i);
        // No write command may be reachable in any unknown case.
        expect(
          gate({ capacity: a.status, capabilitiesNegotiated: queryState === "compatible" }),
        ).toBe(false);
      }
    }
  });

  it("no prepared song is unknown, not insufficient", () => {
    const a = assessCapacity({
      requiredSectors: 0,
      availableSectors: 256,
      queryState: "compatible",
    });
    expect(a.status).toBe("unknown");
  });
});
