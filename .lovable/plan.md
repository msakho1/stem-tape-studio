# Phase 5 — Tape Manipulation Engine (5A node core, 5B worklet core)

No DSP is implemented until this is approved.

## 0. Pre-flight fix (small, before 5A)

The preview still reports a hydration mismatch on `memory-meter`: server renders `384.0 MB`, client renders `192.0 MB`. The SSR budget (`SSR_BUDGET`, `src/audio/memory.ts:26`) is desktop-shaped while the iOS client resolves 192 MiB in `useEffect`. Fix: render the threshold row only after hydration (`useHydrated()` / mount gate) in `ProjectDrawer`, and correct the residual `MB` label to `MiB`. This lands first because Phase 5 adds more numbers to that panel.

## 1. What survives from Phase 4 (audited, unchanged)

| Component | Verdict |
| --- | --- |
| `commands.ts` ordered stream + watermark drain | survives verbatim |
| `Ack` channel | survives; gains new command types |
| `surface.ts` txn snapshot/rollback | survives; `TxnSnapshot` already carries `speed`, `chopDiv`, `chopWindowOffset`, `window`, `loopMode` |
| `controlBus.ts` continuous channels | survives; `window` and `headScrub` channels finally consumed |
| Derived playhead (`anchorCtxTime` + `anchorPos`) | survives in 5A, replaced by frame counters in 5B |
| `ProjectStore` (IDB + OPFS), Memory Saver | survives; adds loop/window persistence |
| Persistent per-track `gain → filter → analyser → master` | survives; `filter` stops being `allpass` |

Extends: `EngineStatus` (loop, window, chop, reverse, per-track rate, engine mode), memory approval per *operation* (reverse copies), per-track transport.

## 2. Command-to-audio matrix

New command types: `loop.set`, `loop.mode`, `chop.set`, `chop.window`, `window.set`, `window.reverse`, `filter.set`, `track.rate`, `head.scrub`, `head.mode`.

| Row | Gesture | Reducer mutation | Engine action | Kind | Boundary | Anti-click | Rollback | Memory | Phase |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `rocker.speed` | fwd/rwd tap | `speed *= (bpm±1)/bpm` | `playbackRate.linearRampToValueAtTime` over 30 ms, re-anchor playhead | continuous | immediate | rate ramp, no restart | snapshot `speed` | 0 | 5A |
| `rocker.speed` hold | hold = glide | `speedGlide=true` | rAF-free `setTargetAtTime` glide on rate | continuous | immediate | tau 60 ms | snapshot | 0 | 5A |
| `rocker.semitone` | double-click | `speed *= 2^(±1/12)` | same rate path, exact ratio, BPM-independent | discrete | immediate | 20 ms ramp | full txn restore | 0 | 5A |
| `play.snap` | FN + tap ×2 | `speed=1` | ramp rate to 1.0 | discrete | immediate | 20 ms ramp | txn restore to pre-×1 | 0 | 5A |
| `rocker.chopHalf/Double/Reset` | FN + fwd/rwd, FN + dbl | `chopDiv` | recompute loop length = `barFrames / chopDiv`; apply at next loop boundary | discrete | next wrap | boundary-aligned, 5 ms fade at seam | txn restore | 0 | 5A |
| `volume.chopWindow` | FN + vol | `chopWindowOffset` | slide chop start within source; hold = glide | continuous | next wrap (5A) / immediate (5B) | seam fade | snapshot | 0 | 5A/5B |
| `fader.window` (FN+1/2/3) | drag | `window.start/end/shift` | preview via `controlBus` `window` channel → engine loop points; commit → reducer | continuous | next wrap | seam fade; re-arm source | 0 | last committed value | 5A |
| `fader.windowReverse` (FN+1 past 2) | drag past end | `window.reverse=true` | 5A: play the reversed buffer copy with mapped offsets; 5B: negate pointer step | discrete | next wrap | seam fade | snapshot | 5A: +1× buffer (gated) / 5B: 0 | both |
| `play.loopMode` | FN + play | `loopMode fixed\|variable` | fixed = shared master loop length; variable = per-track independent lengths (polyrhythm) | discrete | next wrap | none needed | snapshot | 0 | 5A |
| `fader.filter` (FN+4) | drag | `filter` position | mid = bypass (`allpass`), down = LP 20k→200 Hz, up = HP 20→2k, log-mapped, `setTargetAtTime` | continuous | immediate | param smoothing | last committed | 0 | 5A |
| `fader.trackVolume` | drag | fader value | existing `applyTrackGain` | continuous | immediate | existing tau | committed | 0 | shipped |
| Per-track loop (`variable`) | derived | per-track `loopStart/loopEnd` | independent `AudioBufferSourceNode.loop` per track | — | per-track wrap | per-track seam fade | txn | 0 | 5A |
| `heads.replay` / `heads.scrub` | heads mode | head offsets | 3 extra sources per track at −1/4, −2/4, −3/4 loop, faders scrub offsets | continuous | next wrap (5A) / immediate (5B) | crossfade heads | snapshot | 0 (shared buffer) | 5A limited, 5B full |
| `track.mute/unmute/delete/restore`, `transport.*`, `master.gain`, `song.load`, `rollback` | — | unchanged | unchanged | — | — | — | — | — | shipped |

Rule kept from Phase 4: a rejected command never lights an LED, and audio is never inferred from a state diff.

## 3. Phase 5A — node-based tape core

- Keep `AudioBufferSourceNode` per track; use native `loop`, `loopStart`, `loopEnd` for the window/chop. Loop point changes take effect at the next wrap; when the user demands immediate effect, the source is re-armed at a scheduled boundary with a 5 ms equal-power fade through the track gain.
- Varispeed = `playbackRate` (speed and pitch move together, correct tape behaviour). Playhead re-anchors on every rate change so the derived position stays exact.
- Filter: the existing per-track `BiquadFilterNode`, `allpass` at centre.
- Reverse in 5A = a lazily built reversed copy of the buffer, created only when reverse is first requested for that track, gated by `preDecodeGate` against the reverse cost, and dropped when reverse is released. This is the honest cost: reverse doubles that track's PCM in 5A.
- Limits accepted in 5A: sub-loop-boundary edits, per-head sample-accurate scrub, and zero-copy reverse are not achievable. Those are the reason 5B exists.

## 4. Phase 5B — AudioWorklet core

- One `TapeProcessor` per track (not one shared processor): per-track rate, loop and direction are independent, and per-track isolation keeps a single stem's failure from silencing the mix.
- PCM transfer: channel `Float32Array`s are copied once into the worklet at adopt time via `port.postMessage` with transfer, so no duplicate retained copy. No `SharedArrayBuffer` and no COOP/COEP requirement — the hosting environment is not assumed to send cross-origin isolation headers, so the design must not depend on it.
- Interpolated read pointer in frames (fractional, cubic/Hermite interpolation) gives varispeed, reverse (negative step) and sub-block loop edits with no extra buffers. **Reverse costs zero additional memory in 5B.**
- `MessagePort` carries control only (rate, loop points, direction, head offsets) at gesture rate, not audio; audio-rate values are ramped inside the processor.
- iOS Safari: AudioWorklet is supported from iOS 14.5; feature-detect `ctx.audioWorklet` and fall back to the 5A node path with a diagnostic line, never a silent downgrade.
- Startup/teardown: module load is awaited during unlock; a processor that throws is replaced once and then the track falls back to 5A. OPFS chunked reads and worker-side preprocessing are deferred — the buffer is already in RAM and Phase 4.1 proved 199 MiB is affordable.
- GestureEngine, `surface.ts` and all SVG code are untouched by 5B: only the engine's implementation of the same command types changes.

## 5. Loop / chop model and terminology

All state in **sample frames** of the source buffer, never seconds:
`sourceFrames`, `windowStart`, `windowEnd`, `windowShift`, `chopDiv`, `chopWindowOffset`, `direction (+1/−1)`, `loopLength = (windowEnd − windowStart) / chopDiv`.
- **Window** = the user-defined region of the source (faders FN+1/2/3).
- **Chop** = a binary subdivision of the window (`1/1 … 1/16`).
- **Loop** = the region actually cycling = chop slice positioned by `chopWindowOffset` within the window.
- Reverse inverts the direction of travel through the loop, not the window bounds.
- `fixed` loop mode locks all tracks to the master loop length; `variable` lets each track wrap on its own length (the documented polyrhythm light behaviour).

## 6. Memory safety

- Reverse is an *operation* that must pass the memory gate in 5A; the drawer already reports "reverse-copy cost" and it becomes live rather than hypothetical.
- 5B removes the reverse cost entirely; the drawer states which engine mode is active and therefore which cost applies.
- Heads mode adds nodes, not buffers.
- Diagnostics: engine mode (`node` / `worklet`), reverse copies retained, per-track loop length in frames and ms, seam-fade count, and dropped-frame/underrun count from the worklet.

## 7. Click prevention and how it is proved

Every discontinuity (loop re-arm, chop change, reverse flip, head crossfade) is either boundary-aligned or covered by a 5 ms equal-power fade. Proof is not "sounds fine": an `OfflineAudioContext` render of each transition is analysed for peak sample-to-sample delta; the test fails if any transition exceeds the threshold measured on a clean loop wrap.

## 8. Tests and acceptance criteria

Offline/unit: loop-point arithmetic in frames; chop divisions 1–16 round-trip; reverse maps offsets exactly; rate ramp keeps derived playhead within 1 ms of `ctx.currentTime`-based truth; discontinuity thresholds per transition.
Browser (Playwright, 375 px, iOS UA): each matrix row driven from the rendered SVG, with the ack text captured verbatim; fixed vs variable loop wrap timing; faders stay responsive during loop edits; txn rollback restores speed, chop, window and reverse together; save/reload restores loop, window, chop, reverse and BPM; zero network requests carrying user audio; TypeScript clean.
Matrix: iOS Safari (worklet + forced node fallback), Android Chrome, desktop Chrome/Safari/Firefox.

Acceptance: every row in §2 audibly does what the v2.6 card says, no transition exceeds the click threshold, 5B adds no memory for reverse, 5A's reverse cost is gated and reported, and a 199 MiB project still loads and plays with all of it active.

## 9. Order of work

0. Hydration/label fix. 1. Frame-based loop/chop/window model + reducer wiring. 2. 5A loop, chop, window, filter, per-track rate. 3. 5A reverse (gated copy) + heads. 4. Discontinuity harness. 5. 5B `TapeProcessor` behind a feature flag, parity-tested against 5A. 6. Flip the default to worklet where supported.
