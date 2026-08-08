# Transport, Cue, Varispeed SOS and Control-Conflict — Investigation + Implementation Plan

## 1. Reproduced regression: Play stops producing audio

Reproduced in a headless Chromium against the live dev build (demo project, Node engine, desktop pointer): after the first Play → Stop cycle the transport readout stays at `0:01 / 0:09` and the **rate readout reads `0.00×` on every subsequent tap**. The gesture layer is innocent — the surface log shows exactly one `tap ×1 · play` per tap, no duplicated or missing gesture.

### Root cause (confirmed by code path, matches the observed `0.00×`)

The wind-down leaves the tape's *musical* rate destroyed.

- `src/audio/engine.ts:1828-1845` `beginWind()` reads the target from `this.timeline.targetRate()`, and builds a wind-down whose `to` is `INERTIA_MIN_RATE` (≈0).
- `src/audio/engine.ts:1950-1962` the wind-down completion timer calls `this.timeline.endInertia(t, wind.to)` — i.e. it settles the timeline on **0×**, not on the musical rate.
- `src/audio/tape.ts:131-136` `endInertia()` assigns that value to the timeline's constant `rate`. The 1.0× the user was playing at is now gone from state.
- Next Play (`engine.ts:1887-1926`) calls `beginWind("windUp")`, which reads `targetRate()` → ≈0, so it schedules a ramp from ≈0 **to ≈0**. Sources start, but at zero playback rate: silent, playhead frozen, UI shows `0.00×`. Only a further stop/play cycle (which can short-circuit `beginWind` via the `seg.durationS <= 0.011` guard at `engine.ts:1841`) produces audible playback — this is exactly the "second tap works" symptom.

`TapeTimeline.musicalRate()` (`src/audio/tape.ts:106-108`) exists precisely for this and is never called.

Contributing/secondary defects found on the same path, to be fixed together:

- `engine.ts:1892` Play calls `cancelWind()`, which only clears the timer (`engine.ts:1816-1822`). A Play during an in-flight wind-down therefore skips the deferred `stopSources()` and the `transportGain` restore at `engine.ts:1962`, leaving a scheduled ramp-to-zero and orphaned sources.
- `engine.ts:1889` rejects Play whenever `ctx.state !== "running"`, but `useAudioEngine.ts:60` only unlocks when `!engine.ready`. A suspended context that was previously running is rejected instead of resumed → a second tap is required after backgrounding.
- There is no `cued` phase; `transport.restart` (`engine.ts:1973`) starts playback immediately, so Play-hold is a restart, not a cue.

### Fixes

1. `endInertia` on wind-down settles on the **musical** rate, and the transport phase carries whether the tape is stopped; `beginWind("windUp")` targets `timeline.musicalRate()`.
2. `cancelWind()` becomes `settleWind()`: it runs the pending completion work immediately (sources, envelope, phase) before the new command, so a reversal rebases from real state.
3. Play resumes a suspended context inline (`await ctx.resume()`) and then executes the same command; the hook stops treating `ready` as the only unlock trigger.
4. Explicit phase machine in the engine, single source of truth:
   `stopped → cued → windingUp → playing → windingDown → stopped`, with a reversal edge windingDown → windingUp and windingUp → windingDown.
5. Bare Play dispatches on tap count 1 with no multi-tap wait (already true at `surface.ts:369`) — add a guard so FN-qualified Play multi-taps can never reach the bare branch, and assert hold/cancel suppression.

### Regression test

Deterministic engine-level harness: 100 Play/Stop cycles asserting one transition per tap, `requestedPlaying`, phase, source count, timeline rate and position after each; repeated for Node, Worklet, FX overlay open, Heads on, suspended context. Plus a browser fixture that reads engine truth after each tap (rate ≠ 0, position advancing).

## 2. True stop-and-cue

New semantic command `transport.cue` (`stopAndCue`), distinct from `transport.restart` (kept, re-bound):

- Bare Play **hold** → stop all targeted stems, wind down, anchor every stem to song frame 0, phase `cued`, grid phase reset. No playback on release.
- Next single Play tap starts all four stems on **one** scheduled context frame.
- Link/unlink, faders, mutes and FX state untouched. Rejected during recording/stopping/finalising.
- Diagnostics: `cued @ frame 0`; LEDs distinguish cued (slow pulse) from stopped (dark).

### Cue launch profiles

- **Exact** — start at target rate at the scheduled frame; anti-click fade only (2 ms). Audio start frame and outgoing sync/Start share `startFrame`.
- **Tape pre-roll** — the wind occurs in virtual negative time: the worklet is started at `startFrame − windFrames` with tape position held at 0 and output muted while rate integrates from `INERTIA_MIN_RATE` to target; the integral of the inertia curve is *not* consumed from the song (position clamps to 0 until the wind completes). Audible frame zero and sync Start both land on `startFrame`. Pre-roll duration = preset start time (Classic 300 ms), displayed as a countdown.

Both are computable from the existing `inertia.ts` integral; no new DSP.

## 3. Varispeed sound-on-sound — current capability: **partial**

Present: tape-coordinate segment model and exact integrals (`src/audio/input/takes.ts:98-165`), paged take mixer, undo-pass, PRINT, per-track arm.

Missing (verified by grep — no production caller):

- `RecordingController` writes exactly **one** segment at take start (`recorder.ts:337-345`, `{ kind: "constant", value: currentRate() }`) and never appends a segment afterwards. `finishSegments()` (`recorder.ts:374-378`) only closes the last one.
- `followRate()` (`recorder.ts:536-553`) has **zero call sites in `engine.ts`** — `rate.set`, glides, inertia, direction change, loop wrap, window/chop change and relink never notify the recorder or the take mixer.

So today an overdub stores real-time PCM anchored at the tape position it started from and follows transport only superficially. It is not varispeed SOS.

### Work

Emit a segment on every event listed in the brief, driven from the engine's authoritative `TapeTimeline` (`rate.set`, glide, inertia, reverse, loop wrap, window/chop, link) and mirror each to the mixer; store each loop pass as its own sublayer; keep capture PCM dry; undo removes the newest pass only; reverse recording stays rejected.

### Objective tests (must pass before the feature is claimed)

440 Hz at 0.5× → ≈880 Hz at 1×; 440 Hz at 2× → ≈220 Hz; glide 0.5→1.5× compared against the analytic integral; 100 loop wraps frame-exact; repeated rate changes mid-take; save/reload reproduces positions; undo newest pass; PRINT one cycle frame-for-frame against the SOS mix.

This is **four-track sequential recording from one armed input**, not four simultaneous inputs.

## 4. Rocker remap and arbitration

New declarative rows (provenance `extension`, replacing the v2.6 `rocker.chop` / `rocker.chopReset` rows at `src/machine/v26map.ts:28-31`):

| Gesture | Command |
| --- | --- |
| Rocker fwd/back | `rate.set` ±1 BPM (unchanged) |
| Rocker double-click | exact semitone (unchanged) |
| FN + Rocker fwd/back | `transport.scrub` — global four-stem shuttle |
| Play + Rocker fwd/back | `loop.chop` half/double |
| Play + Rocker double-click | `loop.chopReset` |
| Hold Play + Rocker | `loop.chopGlide` |
| Hold bare Play | `transport.cue` |

Conflicts to resolve in `chordArbiter.ts`: Rocker must claim `play` **before** the Play tap or the Play-hold cue fires (claim-before-dispatch, no rollback); FN + Rocker must suppress chop, FX latch and song navigation; Play + Rocker must never toggle transport. Global scrub moves all four timelines on one shared frame, preserves offsets/link/mutes/Heads, does not mutate loop windows, crossfades in/out, and is rejected while recording.

## 5. Pump silence

Two concrete causes:

1. **Tempo collapse.** `effectiveBpm()` (`engine.ts:1156-1160`) is `baseBpm * |timeline.currentRate()|`, and `|rate| || 1` does not catch the ≈0 left by defect §1 — the LFO is programmed at ~0.01 Hz, i.e. a DC gain, inaudible as a pump. Fix: clamp to the musical rate and never below a floor; keep provisional 120 BPM when no grid exists.
2. **Depth too shallow to read as Pump.** `buildLfoGate` (`fx/banks.ts:196-206`) uses depth 0.28 / offset 0.72 → 0.44…1.0. Required default is ≈65 % depth with a click-free boundary; macro must map to depth as well as division.

Also verify during the fix: RHYTHM bank button index (`fx12.ts:76-84` declares `button: 3`, the brief says Button 4), latch survival when the overlay closes, and Heads output path. A rejected algorithm must not display as active (`banks.ts:538-548` already records per-algorithm rejection). Acceptance test measures the output envelope of a constant signal, not a flag.

## 6. Stem switching inside FX mode

The arbiter row exists — `chordArbiter.ts:311-321` claims `play` + volume and emits `stem.select` before the FX volume paths, and `surface.ts:829-833` applies it. What is unverified is whether the overlay session state survives: confirm the selected bank is overlay state (not per-stem reset), that each stem keeps its own algorithms/macros/latches, that a held momentary stays bound to the stem captured at pointer-down, and add the temporary `STEM n NAME` LED/readout that reverts to bank status. If a live check shows the chord failing inside the overlay, the conflicting row will be reported verbatim before the fix.

## 7. Ordered implementation

1. Transport phase machine + rate-preserving inertia (§1) — unblocks §5 too.
2. `transport.cue` + both launch profiles (§2).
3. Rocker remap + arbitration rows (§4).
4. Pump tempo/depth (§5) and FX-mode stem selection polish (§6).
5. Varispeed SOS segmentation (§3) with the objective test suite.
6. Full investigation matrix capture (raw pointer → gesture → claim → command → seq → reducer → engine → ctx state → source generation → scheduled frame → ack → LED) for Node/Worklet, demo/user project, overlay, Heads, linked/unlinked, running/suspended, desktop and mobile emulation.

## Migration / persistence

`transport.cue` is additive to the command union; saved projects gain a cue position (default 0). Take layers gain a segment list per pass — existing single-segment takes load unchanged. FX macro defaults change for Pump only.

## Blocking questions

1. Which cue-launch profile is the default — Exact or Tape pre-roll?
2. Is RHYTHM on hardware Button 3 (current registry) or Button 4 (your brief)?
3. On Play during an in-flight wind-down, should the tape reverse from its current rate (continuous) or restart the wind-up from zero?
