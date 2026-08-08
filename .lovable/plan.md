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

One shared `input-capture-processor` worklet: metering, onset detection, bounded look-back ring (default 4 s stereo ≈ 1.5 MiB), grid-punch history, block emission. Rejected: **A** (extending `TapeProcessor`) — couples capture to tape lifetime, breaks Node-engine mode, and every Node↔Worklet migration would interrupt capture. **B** (four recorder processors) — duplicates onset/look-back and quadruples ring RAM when only one input target can be armed.

Ownership: audio thread owns write position and frame counting; the recording worker owns bytes; the main thread owns only manifests and UI. `MediaRecorder` is not used anywhere. Migration is **rejected while `recording`/`finalizing`** with an honest ack and runs at finalize. Processor failure keeps flushed chunks and marks the take `interrupted`, never `ready`.

### C1. Transfer path and backpressure (correction 3)

```text
AudioWorklet (capture)
  → preallocated transferable block pool (4096 frames/block, 8 blocks)
  → [transferred MessagePort, worklet ⇄ worker direct]
  → recording worker aggregates blocks
  → ~2 s OPFS storage chunks (fixed high-water mark: 6 pending chunks)
```

The port is created on the main thread with `new MessageChannel()`; one port is transferred into the worklet, the other into the worker, so there is **no main-thread relay** and no structured-clone copy of PCM after allocation. Blocks are returned to the pool by the worker for reuse; the audio thread never allocates in `process()` and never waits on storage. Pool exhaustion or a breached high-water mark → **immediate `interrupted`**, already-written chunks preserved, clean stop. A dropped frame can never produce a `ready` take. Step 6A-1 measures the actual transfer behaviour (block round-trip, zero-copy confirmed via detached `byteLength`, per-block copy count) and reports it in diagnostics.

### C2. Long-take playback — paged take processor (correction 1)

An `AudioBufferSourceNode` cannot stream OPFS chunks or follow a tape-mapping curve, so:

```text
OPFS/IndexedDB chunks
  → take-page worker (read-ahead)
  → bounded transferable page cache (default 8 pages ≈ 16 s stereo ≈ 6 MiB/take)
  → TakeLayerProcessor (AudioWorklet)
  → track input node
```

`TakeLayerProcessor` reads only a bounded window around the current tape position, follows rate/direction/loop/segment data, prefetches before playback and before each loop wrap, counts and reports underruns and page-cache misses, and plays a just-completed take immediately from resident pages.

**Threshold:** takes ≤ **20 s** *and* within the existing memory verdict (`memory.ts judge()`) use a memory-gated `AudioBufferSourceNode` fast path; longer takes, or any take that would push the project into `warn`/`block`, use the streaming processor. Tunable in diagnostics.

## D. Frame and tape-coordinate model

**Chosen: store real-time PCM + exact event-defined timeline segments, rendered on playback.** Resampling-on-write bakes in an irreversible decision and compounds error across glides; raw + segments keeps takes portable, exportable dry and re-renderable, and the existing cubic interpolator already does the playback-side work.

### D1. Segments, not sampled curves (correction 2)

```ts
interface TakeTimelineSegment {
  contextStartFrame: number;
  contextEndFrame: number;
  takeStartFrame: number;
  tapeStartFrame: number;
  direction: 1 | -1;
  rate:
    | { kind: "constant"; value: number }
    | { kind: "linear"; r0: number; r1: number }
    | { kind: "exponential"; r0: number; r1: number; tau: number };
  loopIteration: number;
}
```

A segment is emitted **only** on: rate/glide change, direction change, loop wrap, window/chop boundary, link/relink, recording start/stop. The exponential form matches the existing analytic integral in `glide.ts` exactly — no per-quantum approximation, no unbounded metadata.

Because a looped recording maps many recorded frames onto the same tape coordinates, **each loop pass is a distinct pass sublayer** (`passIndex`, own segment list) inside the take, not one flattened curve. Tape position always comes from `TapeTimeline.positionAt(contextFrame / sampleRate)` — never React state or rAF. Unlinked stems use their own timeline; relink changes only future scheduling, never stored take data.

**Reverse recording is rejected in Phase 6B** with an explicit ack; backward-write is a Phase 8 candidate pending physical verification.

## E. Recording state machine

`idle → arming → waiting-for-sound → (waiting-for-grid) → recording | overdubbing → stopping → finalizing → ready`, plus `interrupted` and `failed`. Trigger/punch decisions live on the audio thread; the state mirrors into `surface.ts` for LEDs. Arm = existing `holdStart` at `holdMs`; tap while waiting cancels; tap while recording stops; lost pointer capture forces `stopping` (never stuck). Onset detection: **RMS + peak envelope with hysteresis and a minimum-duration gate** (spectral flux rejected as unnecessary DSP weight).

**One armed external-input target at a time.** Arming a second track returns an explicit ack — `switched: arm moved to track N` (intentional transfer) or `rejected: track N is recording`. Never two simultaneous capture targets. Holding a Track button while the **FX overlay is open never arms recording**; it stays FX-momentary, enforced in `chordArbiter` before dispatch.

## F. Grid and punch

Tap timestamps convert to AudioContext frames; BPM from a median-filtered inter-tap interval with the existing 25 % outlier rejection, feeding a light PLL for phase. Hold-to-round and clear-grid rows are preserved. Metronome LEDs schedule from audio frames, not timers.

Late-press punch: within the late window (default 120 ms or 1/8 beat, whichever is smaller) after a boundary → start from the look-back buffer at that boundary. Otherwise, when a grid exists, the punch **always schedules to the next grid boundary** — no unquantised gridded starts. Punch-out default: **next loop seam when a loop exists; otherwise immediate with a short equal-power fade** — both flagged for physical verification.

## G. Capture vs monitoring (correction 4)

```text
MediaStreamSource
  ├→ InputCaptureProcessor → recording writer   (always, when input enabled)
  └→ monitorGate → active track input → FX      (closed by default)
```

Capture runs regardless of monitor mode; `monitorGate` alone decides what is heard, and changing it can never alter stored PCM. Modes: off / dry / through-track-FX, with a headphone warning, a hard gain ceiling, and no silent enabling.

**Loop-overdub double-audibility rule:** while a pass is being recorded, the live monitor is the only audible source of that material. A completed pass becomes audible at the **first loop seam after its final chunk is durably written and its first pages are prefetched**; if finalisation is not ready by that seam, it waits one further seam. That pass's live monitor contribution is muted at the same seam it starts playing back, so it is never heard twice.

## H. Storage, memory, export, safety

Manifests checkpoint per chunk, so a crash yields a recoverable partial take. Quota is checked before arming and during recording via `navigator.storage.estimate()`; storage is reported separately from decoded RAM (MiB, 1024²). Export: streaming WAV encoder worker, 16/24-bit with TPDF dither on reduction; master performance tap after `master`; Record Performance lives in the project drawer, never on the SP-1 artwork.

**Large-WAV delivery on iOS:** the encoder worker writes the WAV incrementally to an OPFS file and returns a `File` handle instead of assembling an in-memory `Blob`. Where Safari refuses handle-based download, fall back to a size-gated in-memory blob with an explicit warning above **~300 MiB** plus an offer to export at 16-bit or in loop-length segments. No silent giant allocation.

All audio stays local; the Phase 5C network-log assertion extends to recording and export. Every `MediaStreamTrack` stops on disable/project-close. Explicit handling planned for disconnect, backgrounding, lock, suspend, song/bank switch, stem delete, memory/quota ceilings, worklet and worker failure, permission revocation, sample-rate change, and refresh during finalize.

**Project/song switching:** allowed while `armed`/`waiting` (arm cancelled with an ack); **rejected** while `recording`/`stopping`/`finalizing`.

## I. Defaults and ambiguity gates (correction 5)

- **Loaded-track hold → overdub: enabled by default once Phase 6B passes.** Labelled *Stem Tape extension*, not v2.6 parity, and excluded from the 37/37 count. A future hardware-faithful profile may disable or remap it.
- **Heads and PRINT: off by default**, experimental, behind semantic commands, outside the permanent schema, pending physical verification.
- **Reverse recording: rejected** in Phase 6B with an ack.
- **Web MIDI: deferred to Phase 8** (absent on Safari; hardware sync is firmware-only).

A physical SP-1 checklist ships with 6C: hold latch after release, onset timing, pre-roll, loaded-track record, stop timing, punch in/out, late tolerance, grid learning, heads source/offsets/which three tracks, fourth-track behaviour, PRINT, fixed vs variable loop while recording.

## J. Phase 7 tutorial hooks (correction 6 — metadata only, no UI)

Every Phase 6 semantic command carries a declarative record in `stemTapeV1Map.ts`:

```ts
interface TutorialMeta {
  featureId: string;          // "recording.firstTake"
  lessonId: string;           // "6a.arm-and-trigger"
  requiredState: string[];    // ["input.enabled", "track.empty", "fxOverlay.closed"]
  expectedCommand: AudioCommandType;
  successAck: string;
  resetCommand: AudioCommandType;   // e.g. "rec.undoTake"
  ledExplanation: string;
  requiresMicPermission: boolean;
  modifiesProject: boolean;
}
```

A test asserts **every** new `rec.*` / `grid.*` / `export.*` row has complete, non-placeholder metadata, so Phase 7 never reverse-engineers the recording state machine. No tutorial UI in Phase 6.

## K. New files

`src/audio/input/inputDevices.ts`, `inputSession.ts`, `recorder.ts`, `takes.ts`, `takePages.ts`, `latency.ts`; `src/audio/export/wavStream.ts`, `performanceRecorder.ts`; `src/workers/recordingWorker.ts`, `takePageWorker.ts`, `wavWorker.ts`; `public/input-capture-processor.js`, `public/take-layer-processor.js`; `src/machine/recordingState.ts`; `src/device/InputDrawer.tsx`. Extended: `commands.ts`, `engine.ts`, `store.ts` (SCHEMA 3), `stemPerformance.ts` (v4), `surface.ts`, `chordArbiter.ts`, `stemTapeV1Map.ts`, `DiagnosticPanel.tsx`.

## L. Sequence and acceptance

**6A Capture Core** — permissions, devices, metering, monitor gate, shared capture worklet, block-pool/port transfer, onset + pre-roll, take manifests, chunked persistence, `TakeLayerProcessor` paged playback, finalize/recovery, dry WAV export. *Accept:* arm an empty track, sound triggers, full attack captured, playback in sync from pages, save/reload, export — no second full PCM copy, zero underruns.

**6B Overdub & tape coordinates** — pass sublayers, loop overdub with the seam audibility rule, exact segments across glides, unlinked tracks, undo/redo, latency compensation (reported estimate → manual offset → optional loopback wizard), master performance recording, loaded-track overdub enabled on pass.

**6C Grid, Heads, PRINT** — frame-anchored grid, tap learning, look-back punch, punch-out, grid LEDs, beatmatch, physical verification gate, experimental Heads/PRINT.

Testing order: **each take layer is verified independently before any multilayer stress test.** Numeric acceptance: synthetic capture alignment ≤ 2 frames, punch on the intended frame, zero missing/duplicated frames across chunks, loop wrap exact to 1 frame, duration exact to 1 frame, transition residual ≤ −60 dBFS, fade deviation ≤ 0.25 dB, exact WAV header/data length, Node↔Worklet parity, no user audio in any network request. Diagnostics report **take-page cache misses and worklet load as explicitly labelled proxies**, never as CPU utilisation, unless real processor utilisation is measurable. Real-device results are reported in milliseconds, never as "sample-accurate".

## M. Binding approval corrections — these supersede any conflicting text above

**M1. Take playback scales per track, not per take.** One `TrackTakeMixerProcessor` per track (4 total) mixes every enabled take/pass sublayer for that track. A single project-level `TakePageBudgetManager` owns total take-cache memory, sized from simultaneously audible layers plus required read-ahead — no fixed per-take cache. The page worker prefetches before play, seek, reverse and loop wrap; misses and underruns are reported honestly; a take with missing audio is never marked playable. If activating another layer would exceed the budget, the activation is rejected or freeze/bounce is offered — never a silent underrun. The `AudioBufferSourceNode` fast path is **removed from 6A**; it is permitted later only for a provably trivial forward, constant-rate, non-looped take, after parity tests against `TrackTakeMixerProcessor`.

**M2. Transfer blocks are not storage chunks.** The worklet emits small pooled 4096-frame transferable blocks (size and pool benchmark-tunable); the *worker* aggregates them into ~2 s storage chunks. `process()` never allocates two seconds of PCM. The direct Worklet↔Worker `MessagePort` is feature-tested; when unavailable, pooled blocks relay through `AudioWorkletNode.port` preserving transfer where supported, diagnostics state that the main thread is relaying, and the same backpressure and dropped-frame tests run against both paths. Structured-cloning large PCM is never a silent fallback. Pool exhaustion or a breached high-water mark stops the take safely, preserves committed chunks, marks the manifest as failed-mid-take, reports the exact cause, and never shows the take as ready.

**M3. Three independent paths.**

```text
MediaStreamSource
  ├→ InputCaptureProcessor → recording writer      (always)
  ├→ monitorFxGain  → selected track pre-FX input
  └→ monitorDryGain → selected track post-FX sum, pre-fader
```

Exactly one monitor gain open at a time (off / dry / through-FX); both monitor paths still pass the track fader and solo bus; stored PCM stays dry; default off; headphone warning and hard gain ceiling; never silently enabled; the writer never produces a second live-through signal.

**M4. Loop-overdub monitoring (replaces the earlier mute rule).** Live monitoring is **never** muted when a completed pass begins playback. The pass starts at the next eligible loop seam with a seam crossfade, only once durably stored and prefetched; otherwise it waits for a later seam. The musician hears previous passes plus their live performance. Monitoring is fully independent of recording state, and no partially finalised pass may play.

**M5. Bare-Track state table (existing 450 ms hold; declarative rows in `stemTapeV1Map.ts`).**

| State | Tap | Hold ≥450 ms | Double-tap |
| --- | --- | --- | --- |
| Empty | no-op/status | arm first take | no-op |
| Loaded idle | mute/unmute | arm overdub (6B) | recoverable delete |
| Armed/waiting | cancel arm | already-armed ack | cancel; never delete |
| Recording | stop take | no-op | stop only; never delete |
| Overdubbing | stop overdub | no-op | stop only; never delete |
| Stopping/finalising | status ack | no-op | never delete |
| Failed mid-take | show recovery | no-op | never delete |

Crossing 450 ms emits `rec.arm`; normal release leaves the track armed with no continued hold required. Pointer cancel before 450 ms never arms; cancel while waiting safely cancels the arm; cancel while recording requests a safe stop only if the gesture is not already latched; no pointer loss may leave a stuck held-control state; normal release is not cancellation. One armed target: arming another track while *waiting* transfers the arm with an ack naming old and new targets; arming another while recording/stopping/finalising is rejected. The same external input is never captured to two tracks in 6A.

**M6. Input-disabled path.** A Track hold before input is enabled emits `rec.requestInput(trackId)`, opens the Input Drawer, remembers the pending target, and requires an explicit **Enable Input** tap to call `getUserMedia`. On success the originally requested track arms; on denial the pending target clears and the LED restores. Permission is never requested on page load.

**M7. Transport-stopped onset.** The first valid onset on an armed track starts transport and recording on the *same scheduled audio frame*, from the current transport position (no forced restart unless already at zero), respecting the current loop/window; the accepted start frame appears in diagnostics. Playing without a grid: onset with pre-roll. Playing with a grid: look-back / next-boundary punch, using audio-thread frames rather than UI timestamps.

**M8. Stop.** A tap while recording emits `rec.stop`: free recording stops immediately through the approved anti-click fade; with a loop, at the next loop seam; with a grid and no loop, at the approved grid punch-out boundary. During stopping/finalising no tap can start another take or delete audio. After finalisation the Track returns to normal loaded-track mute behaviour.

**M9. FX-overlay exclusion enforced in `chordArbiter` before any `rec.*` is emitted.** With the overlay open, Tracks 1–4 are Filter / Echo / Reverb / Beat Repeat only: no arm, arm transfer, cancel, stop or delete.

**M10. Grid, Heads, PRINT, master recording.** No new grid button: the existing Function ×4, re-tap-over-loop, tap-then-quick-hold and ×4-then-hold gestures remain the grid controls; grid audio behaviour is 6C, but its commands and tutorial metadata are schema-compatible from 6A. Heads/PRINT reserve only Function + triple-tap Play, Function + Faders, and the two Heads Track holds — off by default, experimental, excluded from parity; the double-tap reverse control stays unresolved pending physical testing. Master-performance recording stays in the Performance Drawer (optional `Shift + R`), is 6B, and captures exactly what is heard.

**M11. LED priority.** `error > failed mid-take > recording > overdubbing > finalising > armed/waiting > momentary FX > latched FX > solo > unlinked > active > muted > base`, with breathing (armed), fast pulse or solid-bright (recording), double/alternating pulse (overdub), rapid chase (finalising) and the error pattern. Audio-thread state is authoritative; LEDs change only after accepted acks. Diagnostics show full underlying state, the winning pattern, its arbitration priority, and why it won.

**M12. iOS export limit is benchmark-derived**, not a fixed 300 MiB assumption: measure whether `getFile()`/download spikes memory, then offer 16-bit or segmented export with an honest statement when safe single-file delivery is unavailable.

**M13. Phase 6A scope only** — permissions and lifecycle, device enumeration with actual settings, shared `InputCaptureProcessor`, preallocated block pool, direct port plus tested fallback, metering, monitor modes with warning and ceiling, Track-hold arm mapping, single armed target, RMS + peak onset with minimum-duration gate, pre-roll/look-back, first take on an empty track, recording state machine, safe stop and finalisation, exact take segments, chunked OPFS/IndexedDB persistence, per-track `TrackTakeMixerProcessor` with the shared page budget, reload and paged playback, mid-take recovery, dry 16/24-bit WAV export, recording LED tiers, diagnostics, complete tutorial metadata, storage schema migration. **Not in 6A:** loaded-track overdub, multiple loop passes, master performance recording, operational grid punch, Heads, PRINT, reverse recording, Web MIDI — schema compatibility preserved for each.

**M14. Acceptance and verification** follow the approval's sections 18–19 verbatim: the mapping/state suite, the audio/recording suite, and the objective thresholds (≤2-frame alignment, exact internal duration, zero unexplained dropped frames, residual ≤ −60 dBFS, fade deviation ≤ 0.25 dB, valid WAV header/size/channels/rate/depth, `tsgo --noEmit` clean, zero console errors, no user audio in any network request, no false CPU-utilisation claims). Mobile results are reported as **emulation**; real-iPhone verification is claimed only after the user runs the device checklist. Work stops with the Phase 6A completion report covering every field in section 20, and 6B does not begin without approval.
