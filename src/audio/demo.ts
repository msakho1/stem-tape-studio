/**
 * Bundled demo project — generated procedurally in the browser, so it is
 * license-clear by construction (no commercial music, nothing copyrighted) and
 * short enough that it cannot hide a memory problem.
 *
 * Every stem carries the SAME transient at t=0 and on every bar line, so a
 * misalignment of even a few milliseconds is audible as flam, and mute/fader
 * changes are unmistakable.
 */

import type { StemRole } from "./format";
import { encodeWav } from "./wav";

export const DEMO_SAMPLE_RATE = 48000;
export const DEMO_BPM = 100;
export const DEMO_BARS = 4;
const BEAT = 60 / DEMO_BPM;
const DEMO_SECONDS = DEMO_BARS * 4 * BEAT; // 9.6 s

function alloc(seconds: number) {
  return new Float32Array(Math.round(seconds * DEMO_SAMPLE_RATE));
}

/** Shared alignment marker: a sharp click on every bar line, in every stem. */
function stampBarClicks(out: Float32Array, amp = 0.5) {
  for (let bar = 0; bar < DEMO_BARS; bar++) {
    const start = Math.round(bar * 4 * BEAT * DEMO_SAMPLE_RATE);
    for (let i = 0; i < 240; i++) {
      const env = Math.exp(-i / 40);
      out[start + i] = (out[start + i] ?? 0) + amp * env * Math.sin((2 * Math.PI * 2000 * i) / DEMO_SAMPLE_RATE);
    }
  }
}

function envAt(out: Float32Array, at: number, len: number, gen: (i: number, env: number) => number) {
  const start = Math.round(at * DEMO_SAMPLE_RATE);
  const n = Math.round(len * DEMO_SAMPLE_RATE);
  for (let i = 0; i < n; i++) {
    const idx = start + i;
    if (idx >= out.length) break;
    const env = Math.exp(-i / (n * 0.35));
    out[idx] = (out[idx] ?? 0) + gen(i, env);
  }
}

function makeVocals(): Float32Array {
  const out = alloc(DEMO_SECONDS);
  const notes = [523.25, 587.33, 659.25, 587.33];
  for (let bar = 0; bar < DEMO_BARS; bar++) {
    const f = notes[bar % notes.length]!;
    envAt(out, bar * 4 * BEAT + BEAT, BEAT * 2, (i, env) => {
      const t = i / DEMO_SAMPLE_RATE;
      const vib = 1 + 0.012 * Math.sin(2 * Math.PI * 5.5 * t);
      return 0.28 * env * (Math.sin(2 * Math.PI * f * vib * t) + 0.3 * Math.sin(4 * Math.PI * f * vib * t));
    });
  }
  stampBarClicks(out, 0.4);
  return out;
}

function makeDrums(): Float32Array {
  const out = alloc(DEMO_SECONDS);
  let noise = 12345;
  const rand = () => ((noise = (noise * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff) * 2 - 1;
  for (let beat = 0; beat < DEMO_BARS * 4; beat++) {
    const at = beat * BEAT;
    if (beat % 4 === 0 || beat % 4 === 2) {
      envAt(out, at, 0.25, (i, env) => {
        const t = i / DEMO_SAMPLE_RATE;
        return 0.6 * env * Math.sin(2 * Math.PI * (110 - 70 * Math.min(1, t * 12)) * t);
      });
    }
    if (beat % 4 === 1 || beat % 4 === 3) envAt(out, at, 0.12, (_i, env) => 0.32 * env * rand());
    envAt(out, at + BEAT / 2, 0.05, (_i, env) => 0.12 * env * rand());
  }
  stampBarClicks(out, 0.4);
  return out;
}

function makeBass(): Float32Array {
  const out = alloc(DEMO_SECONDS);
  const roots = [65.41, 73.42, 82.41, 61.74];
  for (let bar = 0; bar < DEMO_BARS; bar++) {
    const f = roots[bar % roots.length]!;
    for (const off of [0, 2 * BEAT, 3 * BEAT]) {
      envAt(out, bar * 4 * BEAT + off, BEAT * 0.9, (i, env) => {
        const t = i / DEMO_SAMPLE_RATE;
        const phase = (f * t) % 1;
        return 0.36 * env * (phase * 2 - 1); // saw
      });
    }
  }
  stampBarClicks(out, 0.35);
  return out;
}

function makeInstruments(): Float32Array {
  const out = alloc(DEMO_SECONDS);
  const chords = [
    [261.63, 329.63, 392.0],
    [293.66, 369.99, 440.0],
    [329.63, 415.3, 493.88],
    [246.94, 311.13, 369.99],
  ];
  for (let bar = 0; bar < DEMO_BARS; bar++) {
    const chord = chords[bar % chords.length]!;
    for (const f of chord) {
      envAt(out, bar * 4 * BEAT, BEAT * 3.6, (i, env) => {
        const t = i / DEMO_SAMPLE_RATE;
        const attack = Math.min(1, t * 8);
        return 0.09 * Math.max(env, 0.35) * attack * Math.sin(2 * Math.PI * f * t);
      });
    }
  }
  stampBarClicks(out, 0.3);
  return out;
}

export interface DemoStem {
  role: StemRole;
  filename: string;
  blob: Blob;
}

/** Stereo (duplicated mono) 16-bit 48 kHz WAVs — the P4 contract format. */
export function buildDemoProject(): DemoStem[] {
  const parts: [StemRole, Float32Array][] = [
    ["vocals", makeVocals()],
    ["drums", makeDrums()],
    ["bass", makeBass()],
    ["instruments", makeInstruments()],
  ];
  return parts.map(([role, mono]) => ({
    role,
    filename: `demo-${role}.wav`,
    blob: encodeWav([mono, mono], DEMO_SAMPLE_RATE),
  }));
}

export const DEMO_NOTICE =
  `generated locally in your browser · ${DEMO_BPM} BPM · ${DEMO_SECONDS.toFixed(1)} s · shared bar-line transient in all four stems`;
