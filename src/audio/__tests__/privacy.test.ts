/**
 * Silent privacy guard.
 *
 * The user-facing sentence about network privacy was removed from the UI, but
 * the protection itself must stay. This test asserts the invariant instead of
 * advertising it: decoding stems, analysing the grid and persisting a session
 * must not open ANY network transport (fetch / XHR / WebSocket / sendBeacon /
 * EventSource / Image beacon).
 */

import { describe, expect, it, vi, beforeEach, afterEach } from "vitest";
import { analyzeSongGrid } from "../gridAnalysis";
import { toStoredProject } from "../session";
import type { SessionState } from "../session";

type Call = { transport: string; arg: string };

let calls: Call[] = [];
const saved: Record<string, unknown> = {};

function trap(name: string, make: () => unknown) {
  const g = globalThis as unknown as Record<string, unknown>;
  saved[name] = g[name];
  g[name] = make();
}

beforeEach(() => {
  calls = [];
  const record = (transport: string) =>
    function (...args: unknown[]) {
      calls.push({ transport, arg: String(args[0]) });
      throw new Error(`${transport} is forbidden on the audio path`);
    };
  trap("fetch", () => record("fetch"));
  trap("XMLHttpRequest", () => {
    return class {
      open(...a: unknown[]) {
        record("xhr").apply(null, a);
      }
    };
  });
  trap("WebSocket", () => {
    return class {
      constructor(url: string) {
        record("websocket")(url);
      }
    };
  });
  trap("EventSource", () => {
    return class {
      constructor(url: string) {
        record("eventsource")(url);
      }
    };
  });
  const nav = (globalThis as unknown as { navigator?: { sendBeacon?: unknown } }).navigator;
  if (nav) {
    saved["sendBeacon"] = nav.sendBeacon;
    nav.sendBeacon = record("sendbeacon");
  }
});

afterEach(() => {
  const g = globalThis as unknown as Record<string, unknown>;
  for (const k of ["fetch", "XMLHttpRequest", "WebSocket", "EventSource"]) g[k] = saved[k];
  const nav = (globalThis as unknown as { navigator?: { sendBeacon?: unknown } }).navigator;
  if (nav && "sendBeacon" in saved) nav.sendBeacon = saved["sendBeacon"];
});

/** 3 s of deterministic pulse audio — stands in for a decoded user stem. */
function pcm(sr = 22050, seconds = 3): Float32Array {
  const data = new Float32Array(sr * seconds);
  const period = Math.round(sr * 0.5);
  for (let i = 0; i < data.length; i++) {
    const k = i % period;
    data[i] = k < 400 ? Math.sin((2 * Math.PI * 180 * k) / sr) * (1 - k / 400) : 0;
  }
  return data;
}

describe("privacy: no user audio ever leaves the device", () => {
  it("grid analysis of decoded PCM opens no network transport", () => {
    const grid = analyzeSongGrid([{ channel: pcm(), sampleRate: 22050 }]);
    expect(grid?.bpm ?? 0).toBeGreaterThan(0);
    expect(calls).toEqual([]);
  });

  it("session persistence serialises no audio and opens no network transport", () => {
    const session = {
      projectId: "p1",
      name: "audit session",
      saved: false,
      savedAt: null,
      source: "upload",
      bpm: 120,
      bpmSource: "provisional",
      lastError: null,
      songGrid: null,
      stems: {
        one: {
          role: "one",
          filename: "drums.wav",
          blob: new Blob([new Uint8Array(64)], { type: "audio/wav" }),
          blobKey: "k1",
          contentHash: "h1",
          provenance: "upload",
          trashed: false,
          probe: { duration: 3, channels: 1, decodedSampleRate: 48000, sniff: { container: "wav", sampleRate: 48000 } },
        },
      },
    } as unknown as SessionState;
    const control = { grid: { bpm: 120, source: "provisional" }, songGrid: null } as unknown as Parameters<
      typeof toStoredProject
    >[1];
    const stored = JSON.stringify(toStoredProject(session, control, "indexeddb"));
    expect(stored).not.toMatch(/data:audio|base64|Float32|ArrayBuffer/i);
    expect(calls).toEqual([]);
  });

  it("the trap itself works (guards against a silently passing test)", () => {
    expect(() => (globalThis as unknown as { fetch: (u: string) => void }).fetch("https://example.com")).toThrow();
    expect(calls.map((c) => c.transport)).toEqual(["fetch"]);
  });
});
