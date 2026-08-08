# Phase 6 — Live Recording, Overdub, Tempo Grid, Punch-In, Heads/PRINT, WAV Export

## A. Repository audit (verified this turn, not assumed)

| Area | Actual state | Phase 6 integration point |
| --- | --- | --- |
| Engine | `src/audio/engine.ts` (1575 lines), `class AudioEngine`, `buildGraph()` at line 255 | add `RecorderBus` + per-track writer, master tap |
| Per-track graph | `input → dry/filter→wet → preFx → FxRack.input → gain(fader) → soloGain → analyser → master → masterAnalyser → destination` (engine.ts 265–303) | live input joins at `input`; take layers join at `input`; master tap after `master` |
| Tape worklet | `public/tape-processor.js` (328 lines), `class TapeProcessor`, frame-scheduled FIFO, ordered `applyAtContextFrame` commands | reused unchanged for Node/Worklet parity; recording does NOT extend it |
| Protocol | `src/audio/workletProtocol.ts`, `sharedApplyFrame()`, `DRIFT_TOLERANCE_FRAMES = 2` | recorder reuses the same shared-frame scheduling |
| Timeline | `src/audio/tape.ts` `TapeTimeline.positionAt/timeAtPosition` (exact glide integral) | authoritative tape-coordinate mapping for takes |
| FX | `src/audio/fx/rack.ts` (502), permanent `FxRack.input` | untouched; dry recording taps before it |
| Gestures | `src/input/gestures.ts` `GestureEngine`, `holdMs 450`, `powerHoldMs 1200`, `HoldLevel` | recording arm uses existing `holdStart` on `track*`; no second engine |
| Arbitration | `src/machine/chordArbiter.ts` `PerfIntent`, claim-before-dispatch | new `rec.*` intents added to the same precedence table |
| Reducer | `src/machine/surface.ts` (862), `grid: {bpm, rejected, source}` already tapped at 338–368, LED table ~743/825 | grid becomes frame-anchored; `recording` LED tier inserted |
| Commands | `src/audio/commands.ts` ordered stream + `Ack` | new `rec.*` / `export.*` command types |
| Storage | `src/audio/store.ts` `SCHEMA_VERSION = 2`, OPFS-with-IndexedDB-fallback behind one `projectStore` API; `contentHash()` exists | takes get chunked blobs through the same façade |
| Perf schema | `src/machine/stemPerformance.ts` `STEM_TAPE_SCHEMA_VERSION = 3` | → 4, with take manifests |
| WAV | `src/audio/wav.ts` — 16-bit only, whole-buffer, main-thread | replaced by a streaming 16/24-bit encoder in a worker; existing fn kept for the demo |
| Heads/PRINT | Already *simulated* in the reducer (`heads.scrub`, `heads.print`, 451/479) — labels only, no audio | Phase 6C attaches experimental audio behind a flag |

No `getUserMedia`, no MediaStream, no recorder code exists today. Grid is currently a label, not a clock.

## B. Feasibility verdict by feature

- **Straightforward:** permissions/device enumeration, metering, monitoring with gain guard, onset trigger, pre-roll, chunked OPFS writes, dry WAV export, master performance WAV, take manifests, undo/redo, LED tiers.
- **Technically challenging:** tape-coordinate mapping under glide/reverse/loop, loop-overdub wrap exactness, backpressure without dropped frames, interrupted-take recovery.
- **Browser-dependent:** raw-input constraints (`echoCancellation:false` etc. may be ignored), deviceId selection (absent on iOS Safari), `SyncAccessHandle` (Safari worker-only), true input latency, Web MIDI (absent on Safari), iOS download behavior.
- **Physically unverified:** loaded-track hold = overdub, hold-latch-after-release, punch-out timing, Heads source/offsets, PRINT semantics.
- **Hardware-only:** sync jack, PO sync, real MIDI clock out.

## C. Chosen recording architecture — **Candidate C + per-track writers**

One `input-capture-processor` worklet (shared): metering, onset detection, bounded look-back ring (default 4 s stereo ≈ 1.5 MiB), grid-punch history, chunk emission. Its PCM is routed to the *armed* track's writer only.

Rejected: **A** (extending `TapeProcessor`) — couples capture to tape lifetime, breaks Node-engine mode, and every Node↔Worklet migration would interrupt capture. **B** (per-track recorder processors) — duplicates onset/look-back four times and quadruples ring RAM for a device that can only arm one input source at a time.

Ownership: audio thread owns write position and frame counting; the worker owns bytes; the main thread owns only manifests and UI. Playback of a finished take uses an ordinary `AudioBufferSourceNode`/worklet layer connected at the track's existing `input` node, so it inherits filter → FX → fader → solo untouched. Migration is **rejected while `recording`/`finalizing`** with an honest ack; queued migration runs at finalize. Processor failure keeps all flushed chunks; the manifest is marked `interrupted`, never `ready`. No second retained copy of stems: takes are separate layers, never a re-render of the base stem.

`MediaRecorder` is not used anywhere.

## D. Frame and tape-coordinate model

**Chosen: store real-time PCM + a rate/direction mapping curve, render on playback.** Rationale: resampling-on-write bakes in an irreversible decision, loses editability, and compounds error across glides; storing raw + curve keeps takes portable, exportable dry, and re-renderable. Cost is playback-side interpolation, which the existing cubic interpolator already implements.

Mapping: each captured block records `contextFrame`; tape position comes from `TapeTimeline.positionAt(contextFrame / sampleRate)` — the same integral used for seams, never React state or rAF. The take stores a sampled rate/direction curve (one node per 128 frames, run-length compressed) plus loop/window and grid snapshots. Loop wrap is written as segment boundaries at the exact seam frame. Unlinked stems use their own timeline instance; relink only changes future scheduling, never stored take data.

**Reverse recording in 6B is rejected with an explicit ack** ("reverse recording is not supported in this build") rather than guessed; backward-write is a 6C/Phase 8 candidate pending physical verification.

## E. Recording state machine

`idle → arming → waiting-for-sound → (waiting-for-grid) → recording | overdubbing → stopping → finalizing → ready`, plus `interrupted` and `failed`. It lives on the audio thread for trigger/punch decisions and is mirrored into `surface.ts` for LEDs. Arm = existing `holdStart` at `holdMs`; tap while waiting cancels; tap while recording stops; lost pointer capture forces `stopping` (never stuck). Onset detection: **RMS + peak envelope with hysteresis and minimum-duration gate** (rejects spectral flux as unnecessary DSP weight for percussion/voice/melody).

## F. Grid and punch

Tap timestamps convert to AudioContext frames; BPM from a median-filtered inter-tap interval with outlier rejection (existing 25 % reject preserved) feeding a light PLL for phase. Hold-to-round and clear-grid rows already exist and are kept. Metronome LEDs schedule from audio frames, not timers. Late-press punch: if the command lands within a tunable late window (default 120 ms or 1/8 beat, whichever is smaller) after a boundary, the take starts from the look-back buffer at that boundary; if within an early window before the next boundary, it schedules forward. Punch-out default: **next loop seam when looping, else immediate** — flagged for physical verification.

## G. Storage, memory, export

`capture worklet → bounded MessagePort queue (high-water mark, dropped-frame counter) → recording worker → OPFS SyncAccessHandle chunk stream (2 s chunks) → IndexedDB chunk fallback → take manifest`, all behind the existing `projectStore` façade. Manifests checkpoint every chunk so a crash yields a recoverable partial take. Quota checked before arming and during recording via `navigator.storage.estimate()`; storage figures reported separately from decoded RAM (MiB, 1024², per existing convention). Newly recorded material becomes playable from the chunks already resident in the write-through window — no second full PCM copy. Export: streaming WAV encoder worker, 16/24-bit with TPDF dither on reduction; master performance tap after `master`. Record Performance lives in the project drawer, never on the SP-1 artwork.

## H. Privacy, monitoring, safety

Monitoring defaults **off**, headphone warning, hard gain ceiling, never auto-enabled. All audio stays local; the network-log assertion from Phase 5C is extended to recording and export. Every `MediaStreamTrack` is stopped on disable/project-close. Explicit handling planned for disconnect, backgrounding, lock, suspend, song/bank switch, stem delete, memory/quota ceilings, worklet and worker failure, permission revocation, sample-rate change, and refresh during finalize — all resolve to a recoverable state, and a take is never shown as `ready` if frames were dropped.

## I. Ambiguity gates

Heads/PRINT and loaded-track overdub ship **off by default, labelled experimental, excluded from the 37/37 v2.6 parity count**, behind semantic commands and outside the permanent schema. A physical SP-1 checklist (hold latch after release, onset timing, pre-roll, loaded-track record, stop timing, punch in/out, late tolerance, grid learning, heads source/offsets/which three, fourth track, PRINT, fixed vs variable loop while recording) ships with 6C. Web MIDI clock is **deferred to Phase 8** (absent on Safari, hardware sync is firmware-only).

## J. New files

`src/audio/input/inputDevices.ts`, `inputSession.ts`, `recorder.ts`, `takes.ts`, `latency.ts`; `src/audio/export/wavStream.ts`, `performanceRecorder.ts`; `src/workers/recordingWorker.ts`, `wavWorker.ts`; `public/input-capture-processor.js`; `src/machine/recordingState.ts`; `src/device/InputDrawer.tsx`. Extended: `commands.ts`, `engine.ts`, `store.ts` (SCHEMA 3), `stemPerformance.ts` (v4), `surface.ts`, `chordArbiter.ts`, `stemTapeV1Map.ts`, `DiagnosticPanel.tsx`.

## K. Sequence and acceptance

**6A Capture Core** — permissions, devices, metering, monitoring, shared capture worklet, onset + pre-roll, take manifests, chunked persistence, finalize/recovery, dry WAV export. *Accept:* arm an empty track, sound triggers, full attack captured, playback in sync, save/reload, export — no second full copy in RAM.

**6B Overdub & tape coordinates** — layers, loop overdub, rate/glide-aware mapping, unlinked tracks, undo/redo, latency compensation (reported estimate → manual offset → optional loopback wizard), master performance recording. *Accept:* repeated overdubs across speed changes, undo the last pass only, export the audible performance.

**6C Grid, Heads, PRINT** — frame-anchored grid, tap learning, late look-back punch, punch-out, grid LEDs, beatmatch, physical gate, experimental Heads/PRINT. *Accept:* tap a grid, punch a late transient onto the intended beat with its attack intact.

Numeric acceptance per subphase: synthetic capture alignment ≤ 2 frames, punch on the intended frame, zero missing/duplicated frames across chunks, loop wrap exact to 1 frame, duration exact to 1 frame, transition residual ≤ −60 dBFS, fade deviation ≤ 0.25 dB, zero unexplained underruns, exact WAV header/data length, Node↔Worklet parity, no user audio in any network request. Real-device results reported in milliseconds, never as "sample-accurate".

## L. Blockers with recommended defaults

1. Loaded-track hold = overdub — default yes, labelled extension. 2. Punch-out — default next loop seam. 3. Reverse recording — default reject with ack. 4. Heads/PRINT — default off. 5. iOS device selection — default system route, stated honestly. 6. Web MIDI — default deferred.
