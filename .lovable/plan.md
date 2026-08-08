# Phase 5 — Tape Manipulation Engine (5A node core, 5B worklet core)

Revised with the binding corrections. Scope approved: preflight fix, 5A as corrected, 5B behind a feature flag that is **not** default until the activation gate (§9) passes on the real ~199 MiB iPhone project.

## 0. Preflight

`memory-meter` still hydration-mismatches: server `384.0 MB` vs client `192.0 MB`, because `SSR_BUDGET` (`src/audio/memory.ts:26`) is desktop-shaped and the iOS budget resolves in `useEffect`. Fix by gating the threshold row on hydration in `ProjectDrawer`, and correct the residual `MB` label to `MiB`.

## 1. What survives from Phase 4 (audited)

| Component | Verdict |
| --- | --- |
| `commands.ts` ordered stream + watermark drain | unchanged |
| `Ack` channel | unchanged; new command types added |
| `surface.ts` txn snapshot/rollback | unchanged; `TxnSnapshot` already carries `speed`, `chopDiv`, `chopWindowOffset`, `window`, `loopMode` |
| `controlBus.ts` | unchanged; `window` and `headScrub` channels finally consumed |
| Derived playhead anchor | **modified** — must become ramp-aware (§3) |
| `ProjectStore` (IDB + OPFS), Memory Saver | unchanged; gains portable loop persistence (§5) |
| Per-track `gain → filter → analyser → master` | **restructured** (§2) |

## 2. Audio graph restructure — true dry bypass

`BiquadFilterNode` in `allpass` is not a bypass: it is flat in magnitude but rotates phase, which is audible on correlated stems and at the crossfade seams. Centre position must route through a genuinely dry path.

```text
sourceA ─ envA ─┐
                ├─ dry ─────────────┐
sourceB ─ envB ─┘                   ├─ trackGain ─ analyser ─ master
                └─ filter (LP/HP) ──┘
```

- `dryGain` and `wetGain` sum into `trackGain`. Centre = dry 1 / wet 0, with the biquad disconnected-in-effect (wet at 0) so its phase never reaches the bus.
- Entering or leaving LP/HP equal-power crossfades dry↔wet over ~30 ms; while wet, the biquad cutoff is log-mapped (down: LP 20 kHz→200 Hz, up: HP 20 Hz→2 kHz) and smoothed with `setTargetAtTime`.
- **`trackGain` is reserved for the physical fader and mute only.** No loop seam, reverse flip, chop change or head crossfade is ever allowed to automate it. Every seam fade happens on `envA`/`envB` (§4). This is an invariant asserted in tests, not a convention.

## 3. Rate math and ramp-aware playhead

Rocker steps are **linear in effective BPM**, never compounding:

```ts
const effectiveBpm = baseBpm * speed;
const nextSpeed = (effectiveBpm + direction) / baseBpm; // === speed + direction / baseBpm
```

`rocker.semitone` stays multiplicative (`speed *= 2^(±1/12)`) and BPM-independent. `baseBpm` comes from the Phase 4.1 hybrid model (`tempo-grid ?? manual ?? provisional 120`); changing `baseBpm` later changes the size of future steps only, never the current audible rate.

Playhead: `position()` currently assumes a single constant rate from the anchor, which is wrong during a ramp. Model the ramp explicitly:

```ts
// linear ramp from r0 at t0 to r1 at t1: distance = (r0 + r1) / 2 * (t1 - t0)
```

The engine keeps an active-ramp record `{t0, t1, r0, r1}`; `position()` integrates the ramp segment plus the constant-rate remainder, and the anchor is rewritten exactly at ramp completion so no error accumulates.

Acceptance: from 120 BPM, two forward steps produce exactly 122.000 BPM (`speed === 122/120` to float exactness of the linear form), and the derived playhead stays within 1 ms of an offline-rendered reference across a rate sweep.

## 4. Dual-source boundary scheduler (5A)

Native `loop`/`loopStart`/`loopEnd` edits alone cannot guarantee click-free arbitrary windows. 5A therefore uses two alternating source instances per track:

- `sourceA`/`sourceB`, each with its own `envA`/`envB` gain.
- The incoming source is created and `start(scheduledSeam, offset)`-ed **before** the outgoing one ends; the two envelopes run an equal-power crossfade across the seam.
- Crossfade length is adaptive: 5–20 ms nominal, clamped to at most ~25% of the loop length so very short chops (1/16 of a short window) still articulate.
- Generation counters continue to guard stale `onended`; a superseded incoming source is stopped and disconnected on the spot.
- Fader and mute automation on `trackGain` is untouched by every transition, by construction.

Discontinuity measurement uses an **absolute objective threshold** on peak sample-to-sample delta and short-window energy step in an `OfflineAudioContext` render, not a native loop wrap as reference (the native wrap may itself click). Each transition type additionally gets a captured-output WAV retained as a listening artefact.

## 5. Loop / chop / window model, and portable persistence

Runtime state is in decoded-context sample frames:
`windowStart`, `windowEnd`, `windowShift`, `chopDiv`, `chopWindowOffset`, `direction (+1/−1)`.

- **Window** = user-defined region of the source (faders FN+1/2/3).
- **Chop** = binary subdivision **of the active window**: `loopLength = (windowEnd − windowStart) / chopDiv`. The `barFrames / chopDiv` form is withdrawn. A tempo grid may optionally *quantize* window and chop boundaries; any such behaviour is tagged `grid-dependent` in state, diagnostics and UI, and never silently redefines the window.
- **Loop** = the chop slice positioned by `chopWindowOffset` inside the window.
- **Reverse** inverts direction of travel through the loop, not the window bounds.

Persistence is **sample-rate portable**. Raw decoded frames are never written. Each persisted boundary is stored as:

```ts
{ frame: number; sourceSampleRate: number; normalized: number; sourceDuration: number }
```

i.e. source-native frame + original rate, with the normalized 0..1 position and source duration as the cross-check. On load, boundaries are converted into the current decoded buffer's frame coordinates (`round(normalized * decodedFrames)`, validated against the rate-scaled frame). Tests: save at 44.1 kHz → restore in a 48 kHz context, and the reverse, asserting musical positions within one frame of the rate-scaled expectation. A store schema bump carries the new shape with a migration for any v2 project.

**Unequal-length tracks.** A shared song-timeline loop preserves time-zero alignment. A track whose source ends before the loop end produces **silence** for the remainder of the loop — it does not wrap early and does not stretch. Per-track `variable` loops are the only way a track wraps on its own length, and that mode is provisional (§6).

## 6. Heads and loop-mode semantics — hardware verification gate

The v2.6 card says only "3 tracks replay the source, a quarter apart". That is ambiguous on: which three tracks; quarter note or quarter of the loop; three total heads or three per track; what the fourth track does; what PRINT does. **VERIFY ON PHYSICAL SP-1** before anything is claimed as v2.6 parity.

Phase 5 may ship an *experimental browser-only heads interpretation*, labelled as such in the UI, the diagnostics and the v2.6 row status (a new `experimental` status distinct from `state`/`audio`). It does not count toward parity and its default is off.

Likewise `fixed` vs `variable` loop mode is implemented as a **provisional interpretation** (fixed = one shared loop length; variable = per-track lengths, the documented polyrhythmic lights) and labelled provisional until verified.

Recording and PRINT remain Phase 6.

## 7. Command-to-audio matrix

New command types: `loop.set`, `loop.mode`, `chop.set`, `chop.window`, `window.set`, `window.reverse`, `filter.set`, `track.rate`, plus experimental `head.scrub` / `head.mode`.

| Row | Gesture | Reducer mutation | Engine action | Kind | Boundary | Anti-click | Rollback | Memory | Phase |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `rocker.speed` | fwd/rwd tap | `speed += dir/baseBpm` | linear rate ramp ~30 ms, ramp-aware re-anchor | continuous | immediate | rate ramp only, no re-arm | snapshot `speed` | 0 | 5A |
| `rocker.speed` hold | hold = glide | `speedGlide` | `setTargetAtTime` glide, ramp integral tracked | continuous | immediate | tau 60 ms | snapshot | 0 | 5A |
| `rocker.semitone` | double-click | `speed *= 2^(±1/12)` | same rate path, exact ratio | discrete | immediate | 20 ms ramp | full txn restore | 0 | 5A |
| `play.snap` | FN + tap ×2 | `speed = 1` | ramp to 1.0 | discrete | immediate | 20 ms ramp | txn restore to pre-×1 | 0 | 5A |
| `rocker.chopHalf/Double/Reset` | FN + fwd/rwd, FN + dbl | `chopDiv` | `loopLength = (windowEnd−windowStart)/chopDiv`; applied at next scheduled seam | discrete | next seam | dual-source crossfade | txn restore | 0 | 5A |
| `volume.chopWindow` | FN + vol | `chopWindowOffset` | slide chop start within window; hold = glide | continuous | next seam (5A) / immediate (5B) | crossfade | snapshot | 0 | 5A/5B |
| `fader.window` FN+1/2/3 | drag | `window.start/end/shift` | `controlBus` `window` preview → loop points; commit → reducer + portable persistence | continuous | next seam | crossfade | last committed | 0 | 5A |
| `fader.windowReverse` FN+1 past 2 | drag past end | `window.reverse` | 5A: gated reversed copy with mapped offsets; 5B: negate pointer step | discrete | next seam | crossfade | snapshot | 5A: +1× track PCM (gated) / 5B: 0 | both |
| `play.loopMode` | FN + play | `loopMode` | fixed = shared length; variable = per-track (**provisional**) | discrete | next seam | none | snapshot | 0 | 5A |
| `fader.filter` FN+4 | drag | filter position | dry/wet crossfade + log-mapped biquad; centre = true dry | continuous | immediate | 30 ms equal-power | last committed | 0 | 5A |
| `fader.trackVolume` | drag | fader value | `applyTrackGain` on `trackGain` only | continuous | immediate | existing tau | committed | 0 | shipped |
| Unequal-length track | derived | — | silence past source end, time-zero preserved | — | — | envelope at 0 | — | 0 | 5A |
| `heads.*` | heads mode | head offsets | **experimental interpretation**, off by default, labelled | continuous | next seam | crossfade | snapshot | 0 (shared buffer) | 5A exp / 5B |
| `track.*`, `transport.*`, `master.gain`, `song.load`, `rollback` | — | unchanged | unchanged | — | — | — | — | — | shipped |

Invariant retained: a rejected command never lights an LED, and audio is never inferred from a state diff.

## 8. Phase 5B — Worklet core and PCM ownership lifecycle

One `TapeProcessor` per track (per-track rate/loop/direction independence, and one stem's failure cannot silence the mix). Control only over `MessagePort`; no `SharedArrayBuffer`, no COOP/COEP dependency. Fractional read pointer with Hermite interpolation gives varispeed, zero-copy reverse and sub-block loop edits.

The memory claim is corrected to: **no duplicate PCM retained after a successful Worklet cutover.** The lifecycle is documented and measured before implementation:

1. `AudioBuffer.getChannelData()` returns storage owned by the browser's buffer — it is not reliably transferable, so `copyFromChannel` into freshly allocated `Float32Array`s is assumed required and the assumption is tested per browser.
2. Those owned arrays are `postMessage`-transferred; **peak memory while the node buffer and the worklet arrays coexist is 2× that track's PCM** and must pass an operation-level memory gate before migration starts.
3. Migration is **sequential, one track at a time**, with a yield between tracks.
4. The node representation for a track is released only after the worklet **acknowledges** ownership and reports first successful render.
5. If a processor fails after ownership transfer, fallback re-decodes that track from the stored blob (Memory Saver derived blob when present). Retaining a second PCM copy as insurance is rejected: it defeats the point. Fallback cost — decode latency and transient bytes — is measured and reported.
6. A failed migration leaves the currently playing 5A engine intact and audible; the flag reverts for that session.

iOS Safari has AudioWorklet from 14.5; absence forces 5A with a visible diagnostic, never a silent downgrade. OPFS chunked reads and worker preprocessing stay deferred.

Diagnostics gain: engine mode per track (`node`/`worklet`), reverse copies retained, loop length in frames and ms, seam count, worklet underrun count, migration peak bytes, fallback events.

## 9. Worklet activation gate (all must pass; feature detection alone is insufficient)

1. Four-track command parity with 5A across the whole matrix.
2. No loop, reverse or rate discontinuity beyond the objective threshold.
3. No unexpected peak-memory rejection during migration.
4. No underruns under aggressive fader, rocker and window gestures.
5. Save/reload restores identical musical positions (including 44.1↔48 kHz).
6. Background, lock-screen and AudioContext interruption recovery.
7. Forced processor failure safely restores or reinitializes playback.
8. The ~199 MiB four-stem project remains usable on iPhone.
9. No user audio in any network request.

## 10. Tests

Offline/unit: BPM linearity (120 → 122.000 exactly); ramp-integral playhead vs offline reference; window/chop frame arithmetic for `chopDiv` 1–16; portable persistence round-trips 44.1↔48 kHz both directions; unequal-length silence; dry-path phase check (centre filter position must be sample-identical to no filter).
Browser (Playwright, 375 px, iOS UA): every matrix row driven from the rendered SVG with acks captured verbatim; seam discontinuity capture per transition; txn rollback restores speed, chop, window and reverse together; fader responsiveness during loop edits; migration and forced-failure fallback on the real 199 MiB project; zero user-audio network requests; TypeScript clean.
Matrix: iOS Safari (worklet + forced node fallback), Android Chrome, desktop Chrome/Safari/Firefox.

## 11. Implementation order

1. Hydration and MiB-label fix.
2. Portable loop/window persistence model + schema migration.
3. Correct rate math and ramp-aware playhead.
4. True filter bypass (graph restructure).
5. 5A window, chop, loops, varispeed.
6. Dual-source seam scheduler + discontinuity tests.
7. Memory-gated 5A reverse.
8. Physical SP-1 verification of Heads and fixed/variable (blocking for parity claims only).
9. One-track Worklet migration prototype.
10. Sequential four-track migration and fallback proven on the 199 MiB project.
11. Worklet parity for reverse, loops, window, scrub.
12. Flip the default only after §9 passes.

Completion report will return the exact browser evidence, peak-memory readings, verbatim command acknowledgements, discontinuity measurements and fallback results.
