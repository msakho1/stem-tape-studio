# Heads isolation + frame-accurate loop rejoin

Two independent defects, both confirmed by reading the engine graph and the loop command path.

## Root causes (verified in source)

**Defect 1 — normal mix leaks under Heads.**
`buildGraph()` (`src/audio/engine.ts:462-521`) wires each stem `... → soloGain → analyser → master`, and `headLanes` is constructed with `destination: () => this.master` (`engine.ts:1513`). There is no bus between the stems and the master, so the heads bus is purely additive. `applyAudibility()` (`engine.ts:1348-1367`) carries the comment "Heads no longer ride the per-stem chains" and gates only on mute/solo/audition — it never gates the stems while `heads.active`. Result: four normal stems + four heads sum into master.

**Defect 2 — loop release rejoins at the wrong position.** Two compounding causes:

1. `relocate()` (`engine.ts:923-935`) calls `this.timeline.anchor(at, toPosition)`. `relocate` is invoked from `loop.set` whenever a loop is enabled (`engine.ts:3311`). Capturing a one-bar loop on ONE lane therefore drags the **shared** song timeline back to that lane's loop start — the other three lanes' seam math, the playhead and the "hidden" position are all corrupted. There is no per-lane hidden pointer at all: `position()` is the single shared timeline.
2. `loop.release` maps to `loop.set {enabled:false}` (`engine.ts:3615`), and `loop.set` only relocates when `enabled` is true (`bounds && enabled`). On release nothing respawns: the lane keeps playing forward from wherever the last seam wrapped it, permanently offset by (loopStart − hidden position). No bar-boundary scheduling, no crossfade, no generation invalidation.

## Fix 1 — explicit bus handoff

- Add `normalBus` and `headsBus` gain nodes in `buildGraph()`: every stem `analyser → normalBus → master`; `headLanes` destination becomes `headsBus → master`.
- `enterHeadsMode` / `exitHeadsMode` schedule a short (~20 ms) complementary crossfade on the two bus gains at one shared context time. No mute, solo, fader, loop or FX state is touched.
- FX-return leak: FX racks live upstream of the stem analyser, so gating `normalBus` also gates wet returns; verified by measuring the analyser after the racks. Reverb/echo tails therefore fade with the bus rather than leaking.
- Hidden timeline keeps advancing: normal sources stay live and scheduled while `normalBus = 0`, so exit rejoins at the current hidden position with no respawn/rewind.
- On exit, after the fade completes, assert one live source per stem and destroy heads voices (`headLanes.exit()` already stops voices; add a post-fade sweep + count assertion).

## Fix 2 — per-lane hidden pointer and bar-boundary rejoin

- Stop `relocate()` from anchoring the shared timeline. Split it: `relocateShared()` (keeps `timeline.anchor`, used by transport/scrub handoff) and `relocateLane()` (no anchor, used by `loop.set`, `loop.chop`, reverse). This alone restores the hidden song pointer for every lane.
- Give `TrackRuntime` an explicit `hidden` pointer derived from the shared integrated-rate timeline (`timeline.positionAt(ctx.currentTime)`), so varispeed and glides are inherited for free; the audible loop pointer stays the seam-driven loop read.
- `loop.release` becomes a real handler (no longer an alias of `loop.set {enabled:false}`):
  1. compute `releaseAt = nextBarAfter(grid, now)` on the shared grid (fallback: now + lookahead);
  2. compute the lane's hidden song position at exactly `releaseAt` through the integrated rate curve;
  3. spawn the replacement source at that frame with an equal-power fade-in scheduled at `releaseAt`;
  4. equal-power fade the loop source out over the same window, `stop()` it at fade end, bump `t.generation` to invalidate the loop generation, and clear `committedSeamAt`;
  5. clear `loop.enabled` only at the boundary so no seam is committed past it.
- Worklet engine: mirror the same handoff via `setWindow`/`applyAtContextFrame` using the shared apply frame, so both engines land on the same frame.

## Tests

New `src/audio/__tests__/headsIsolation.test.ts` and `src/audio/__tests__/loopRejoin.test.ts` over the existing offline/mocked graph:

- Heads: per-path contribution (normal bus tap ≈ 0, heads bus tap > 0), FX-return tap ≈ 0, hidden timeline advances while silent, exit landing error, source counts, no duplicate paths.
- Loop rejoin: landing error ≤ 2 source frames against the hidden timeline and against the other stems' read frames; matrix over lanes 1-4, 1/2/3/4 simultaneous loops, ¼/½/1/2/4/8 bars, 0.5×/1×/2×, a glide while looping, a loop captured after per-lane scrub, 100 capture/release cycles asserting no source leak, both node and worklet modes.

## Browser acceptance

Playwright (Chromium, then WebKit) driving the rendered SP-1 with real pointer events only. Per scenario the log records: pointer sequence → resolved gesture → command → ack → scheduled context frame → audible source frame → hidden frame → bus gains → path count → measured RMS. Reported metrics: normal-bus RMS during Heads, heads-bus RMS, hidden advancement, exit landing error, per-stem release landing error, path counts, max seam discontinuity, console errors.

## Files expected to change

`src/audio/engine.ts` (graph, bus gains, applyAudibility, relocate split, loop.release handler, diagnostics), `src/audio/headLanes.ts` (destination + exit sweep), `src/audio/useAudioEngine.ts` / `src/lib/diagnostics.ts` (per-bus RMS + path counts for the proof), plus the two new test files. No gesture, arbitration or control mapping changes.
