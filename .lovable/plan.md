# Stem Tape — Browser Prototype Implementation Plan

Goal: prove the creative concept (four stems that are simultaneously one song and four independent tape tracks) in the browser before any SP-1 firmware work. No implementation until approved.

## 1. Feasibility verdict

**Straightforward** — four-file decode to `AudioBuffer`; one `AudioContext` as the authoritative clock; sample-accurate start of four `AudioBufferSourceNode`s; four faders/mute/solo/master via `GainNode`; per-track meters via `AnalyserNode`; per-track `loopStart`/`loopEnd`/`loop`; tape varispeed via `playbackRate` (speed and pitch move together — the native node behaviour *is* tape behaviour); canvas waveforms from worker-computed peaks; localStorage/IndexedDB settings.

**Technically challenging** — click-free loop boundaries (native looping offers no crossfade; needs dual-source ping-pong scheduling or a worklet); reverse-while-playing without disturbing other tracks; keeping four independent playheads exact once each has its own rate and loop; WAV export.

**Browser-dependent** — codec support (WAV/MP3 everywhere; M4A/AAC generally on Chrome/Edge/Safari; **AIFF reliable only on Safari** — we runtime-probe each file and report honestly rather than promising a format); `MediaRecorder` output type (Chrome/Edge give WebM/Opus, not WAV; Safari gives MP4); autoplay unlock requires a gesture; mobile memory caps.

**Better postponed** — multiple playback heads; BPM/grid snapping; PWA install; touch layout.

**Not realistic in-browser** — guaranteed glitch-free playback under arbitrary system load, and true sub-buffer latency; both are host-scheduler dependent and we will measure rather than claim.

## 2. Recommended architecture — staged, not worklet-first

Phase A uses standard Web Audio nodes. They already give per-track loop windows, per-track rate and a shared clock, which covers the entire MVP. Phase B introduces a custom `StemTapeProcessor` AudioWorklet only where nodes genuinely fall short: bidirectional reading, sample-accurate loop crossfades, and multiple heads. Building the worklet immediately would delay the creative question by weeks for no MVP capability.

```text
File input ──► decode (decodeAudioData, off main thread where possible)
                  │
                  ├─► peaks worker ──► canvas waveform renderers
                  │
                  ▼
            AudioBuffer x4 (+ reversed copies, lazy)
                  │
   TransportClock (single AudioContext.currentTime)
                  │
   ┌──────────────┴──────────────┐
   │  TrackEngine x4             │   each: source node (recreated on
   │  pos / loop / dir / rate    │   seek, loop-change, reverse) →
   └──────────────┬──────────────┘   trackGain → panner? (no) → meter
                  ▼
        Mixer: 4 trackGain → soloBus → masterGain
                  ├─► destination (speakers)
                  └─► recorder tap (worklet) ─► worker ─► WAV Blob
                  │
            IndexedDB (settings, optional audio blobs)
```

## 3. Synchronization strategy

- **One clock.** A single `AudioContext`. No `<audio>` elements anywhere. All scheduling uses absolute `ctx.currentTime + lookahead` values.
- **Same frame start.** Compute `startAt = ctx.currentTime + 0.08` once, then call `source.start(startAt, offset)` on all four. Identical `startAt` = identical first rendered frame.
- **No accumulating drift.** Playhead is never incremented by a timer. It is *derived*: `pos = anchorPos + (ctx.currentTime - anchorCtxTime) * rate * dir`. Every rate/direction/loop change writes a new `(anchorPos, anchorCtxTime)` pair, so error cannot compound. `requestAnimationFrame` only reads this for drawing.
- **Pause/resume.** Freeze each track's derived position, stop the sources, and on resume restart all four at one shared future `startAt` with their frozen offsets. Linked tracks share one offset; unlinked keep their own.
- **Seek.** Global seek rewrites every linked track's anchor and restarts them at a common `startAt`. Unlinked tracks are untouched.
- **Going independent.** Unlinking is purely a state flag plus its own anchor pair; the track keeps playing from its current derived position with no restart.
- **Relink.** Snap the track's position to the global transport's current derived position modulo the shared loop, restart it at the next shared `startAt`, and reset its loop to the shared window. This is an audible jump by design; the UI will say so.
- **Verification.** The diagnostics panel compares each track's derived position against the same value recomputed from a reference track and reports drift in ms and samples.

## 4. State model

Two clearly separated layers. React state is UI-authoritative and re-renders; the engine holds real-time values that must never trigger renders.

```ts
// ---- UI / persisted state (React + IndexedDB) ----
type StemRole = 'vocals' | 'drums' | 'bass' | 'instruments';
interface LoopRegion { startSec: number; endSec: number; enabled: boolean }
interface PlaybackHead { id: string; offsetSec: number; gain: number; reversed: boolean } // phase 9
interface TrackState {
  id: string; role: StemRole; label: string;
  file: { name: string; durationSec: number; channels: number; sampleRate: number } | null;
  gain: number; muted: boolean; soloed: boolean;
  linked: boolean; selected: boolean;
  loop: LoopRegion; reversed: boolean; rate: number; startOffsetSec: number;
  heads: PlaybackHead[];
}
interface TransportState { playing: boolean; positionSec: number; durationSec: number; rate: number; sharedLoop: LoopRegion }
interface MixerState { masterGain: number; anySolo: boolean }
interface RecordingState { status: 'idle'|'recording'|'encoding'; startedAtSec: number|null; elapsedSec: number }
interface ProjectState { id: string; name: string; artist: string; tracks: TrackState[]; transport: TransportState; mixer: MixerState; createdAt: string }

// ---- realtime engine state (refs only, never in React state) ----
interface TrackRuntime {
  buffer: AudioBuffer | null; reversedBuffer: AudioBuffer | null;
  source: AudioBufferSourceNode | null; gainNode: GainNode; analyser: AnalyserNode;
  anchorPos: number; anchorCtxTime: number; rate: number; dir: 1 | -1;
}
```

## 5. Component and service structure

```text
src/audio/            AudioEngine.ts (facade)  TransportClock.ts  TrackEngine.ts
                      Mixer.ts  LoopScheduler.ts  Recorder.ts  decode.ts  wav.ts
src/workers/          peaks.worker.ts  wavEncoder.worker.ts
src/worklets/         recorder-tap.js   (phase B: stem-tape-processor.js)
src/state/            useProject.ts (zustand-style store)  persistence.ts
src/components/       FileLoader  TransportBar  StemMixer  TrackStrip  Timeline
                      LoopEditor  RecordingControls  ProjectMenu  DiagnosticsPanel
src/routes/           index.tsx (the instrument)  diagnostics.tsx  about.tsx
```

`AudioEngine` is a plain class instantiated once and held in a ref/context. Components call imperative methods and subscribe to a 60 Hz snapshot for meters and playheads. No audio object ever lives in React state.

## 6. Reverse, varispeed, looping, recording — the specific answers

- **Reverse (MVP):** keep a lazily-built reversed copy of the buffer. Flipping direction stops that one source and starts a new one on the reversed buffer at the mirrored offset `duration - pos`, with mirrored loop points, at the next block boundary. Cost: one extra buffer copy in RAM per reversed track. Other tracks are untouched. **Phase B** replaces this with a bidirectional read pointer in the worklet, removing the duplicate buffer and the restart.
- **Varispeed:** `source.playbackRate`, which resamples and therefore moves pitch with speed — correct tape behaviour, no pitch-preservation node wanted. Range 0.5–1.5×, with an exact snap-to-1.0 control (not a float near 1). Changes use `setTargetAtTime` over ~30 ms to avoid zipper noise, and rewrite the anchor pair.
- **Looping:** native `loop`/`loopStart`/`loopEnd` for the first playable build, then upgrade to **ping-pong scheduling** — two sources per track, the next one scheduled to start a few ms before the current ends, with 5–10 ms equal-power gain ramps — to remove boundary clicks. Free (non-quantised) loops first; BPM snapping later.
- **Recording:** `MediaRecorder` is rejected for the deliverable because Chrome/Edge cannot emit WAV. Instead an AudioWorklet tap on the master bus posts Float32 blocks to a worker that writes 16- or 24-bit PCM WAV incrementally and returns a Blob for download. Start/stop never touches the playback graph.
- **Waveforms:** no wavesurfer.js or peaks.js — each owns its own media/context and would fight our clock. We compute min/max peak pyramids in a worker and draw to canvas ourselves.
- **Persistence:** settings and project JSON in IndexedDB. Audio is **not** uploaded and by default not stored; on reload we restore all controls and prompt to re-pick the files (browsers cannot silently reopen local files; Chrome's File System Access handles can be persisted but Safari has no equivalent, so the prompt is the baseline path). Optional opt-in "embed audio" stores blobs in IndexedDB, and a downloadable `.stemtape` file holds settings plus either references or embedded audio.

## 7. Implementation phases (each ships and is checkable)

1. **Four synchronized files** — load 4 files, play/pause/stop/seek. *Accept:* diagnostics shows <1 sample drift between tracks after 10 min.
2. **Mixer** — faders, mute, solo, hold-to-solo, master, meters. *Accept:* solo isolates instantly, no clicks, meters track audible level.
3. **Shared loop** — draggable region on the main timeline, move-without-resize. *Accept:* all four loop identically and stay in phase for 100+ passes.
4. **Independent loops** — track selection, per-track loop edit, reset-to-shared, relink. *Accept:* drums 1 beat, bass 3 beats, others 4 bars, all continuous.
5. **Reverse** — per selected track. *Accept:* one stem reverses with no interruption to the other three.
6. **Varispeed** — global and per-track, exact 1.0 reset. *Accept:* smooth sweeps, pitch follows speed, no zipper noise.
7. **Recording/export** — WAV download. *Accept:* exported file matches what was heard, correct length ±10 ms.
8. **Persistence** — session restore + `.stemtape` file. *Accept:* reload restores every control; audio re-attaches via prompt.
9. **Multiple heads** (worklet) — 2–4 heads/track with offset, gain, direction. *Accept:* stated CPU ceiling enforced with graceful degradation.
10. **Touch/PWA** — responsive layout, manifest, offline shell.

## 8. Testing plan

Automated where possible (Vitest + an OfflineAudioContext harness): 10-minute drift run; unequal-length files padded from time zero; 100× play/pause; seek storm; shared-loop phase check; four different loop lengths; single-track reverse; global and per-track rate changes; loop-boundary click detection via sample-delta threshold on rendered output; recording length/content accuracy. Manual matrix: Chrome, Edge, Safari desktop, iOS Safari (memory and autoplay), with results recorded in-app.

**Diagnostics panel** (route `/diagnostics`, also a toggleable overlay): `AudioContext.currentTime` and `baseLatency`, each track's derived playhead in sec and samples, pairwise drift in ms/samples, source restart count, dropped-frame/underrun counter from the worklet tap, active head count, and rAF frame timing as a rendering-load proxy. Where a metric is not measurable in a browser, the panel says so instead of showing a fake number.

## 9. Known risks and decisions

Codec gaps (AIFF outside Safari) → probe and report per file. Autoplay policy → explicit "enable audio" gesture on first load. Mobile memory → a 5-min stereo 48 k stem is ~55 MB decoded; four stems plus reversed copies can exceed 400 MB, so mobile gets a size warning and reversed buffers are built lazily and freed. Safari inconsistencies → feature-detected, degraded, and surfaced. AudioWorklet needs same-origin module serving and a secure context — fine here, noted for the eventual companion app. Click-free loops are the highest-risk audio detail and are why ping-pong scheduling is scheduled in Phase 3, not deferred. Export is WAV via our own encoder specifically to dodge `MediaRecorder` format limits. Audio stays local; nothing is uploaded. Keyboard control (space, arrows, 1–4 track select, R record) and visible focus states are built in from Phase 1, not retrofitted.

## 10. First implementation recommendation

**Phases 1–7, no AudioWorklet except the recorder tap.** That single cycle delivers: upload four stems, perfectly synchronized playback, four-fader mixing, one shared loop, per-stem loop lengths, one reversed stem, global varispeed, and a downloadable WAV of the performance — which is exactly the set of things that answers "is this creatively valuable?". The custom four-track processor, multiple heads and grid snapping follow only if the answer is yes.

**Design language:** restrained instrument aesthetic — neutral graphite/bone panel, single signal accent for armed/selected states, monospace numerics, hairline dividers, physical-feeling fader tracks. Original work throughout; no TE logos, trade dress or product graphics.
