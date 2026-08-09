# Phase 7 (reduced) — Performance Essentials Tutorial

One guided tutorial, ten lessons, ~10–15 minutes, run on the user's own uploaded stems. Everything from the previous Phase 7 plan not listed below is dropped (feature registry, coverage dashboard, analysis worker, tutorial IndexedDB, modules 0–12, Tutorial Copies, recording/PRINT lessons, portable import/export, guide generation).

## 1. Preconditions (fix and browser-verify before lesson work)

| # | Defect | Current state (verified) | Gate |
|---|---|---|---|
| P1 | Global scrub audible | scrub path implemented; release handoff landed | re-verify in Chromium + WebKit |
| P2 | Scrub release overlap | handoff frame + generation invalidation implemented, unit-tested | browser-verify: no flam, 1 path/stem |
| P3 | Keyboard faders | `keyboardMap.ts:88-105` defines Y/H U/J I/K O/L; no browser proof | Playwright: 2+ channels overlapping in one batchFrame |
| P4 | Chop mapping | still FN+rocker double-deflect (`surface.ts:541-547`) — **not** Play+Rocker | remap + arbitration |
| P5 | FX stem switch inside overlay | `engine.ts:2956` `fx.overlay` acks with "tape audio unaffected"; no overlay-preserving side effect proven | verify `stem.select` keeps overlay open |
| P6 | Pump audible | `banks.ts` pump present; no measured envelope | measured periodic gain modulation |
| P7 | Heads scrub audible | `scrub.ts` + worklet chase implemented, unit-tested | browser-verify head-path output |

Lesson 6 (scrub) and Lesson 9 (heads) do not ship until P1/P2 and P7 pass. If a gate fails at build time the lesson ships marked *unavailable* rather than faked.

## 2. Tutorial session setup

1. Requires a loaded project with four stems (otherwise the Learn button routes to PROJECTS with a hint).
2. Prompt: play the song, stop at a section where several stems are audible, press **Use this section**.
3. Build a temporary loop 8–16 s around that position (window/loop commands only — no audio copy, no re-decode, no blob duplication).
4. Snapshot control state before entry (see §5).
5. On completion or exit: **Keep my changes** / **Restore my original settings**.

## 3. Lessons and milestones

Each milestone: entry watermark on `AudioCommand.id`, expected command type, required ack (`accepted`/`completed`), engine assertion.

| # | Lesson | Ordered milestones | Engine assertion | Restore |
|---|---|---|---|---|
| 1 | Meet the controls | highlight tour only; desktop opens Keyboard Controls panel | none | none |
| 2 | Play / stop / resume / cue | `transport.play` → `transport.stop` → `transport.play` → `transport.cue` (hold) → `transport.play` | transport state + cue at frame 0 | leave playing |
| 3 | Mix the stems | one fader move → two overlapping → opposing directions | gain change on each channel; touch: ≥2 pointerIds in one `batchFrame`; keyboard: ≥2 channels with overlapping held intervals in one `batchFrame` | fader values |
| 4 | Individual stems | `track.mute` → `track.unmute` → `stem.select` (Play+Volume) → `stem.solo` (Play+Track) → `stem.link` | mute map, selection, solo mask, link mask | solo + link |
| 5 | Tape speed | `rate.set` ±1 BPM → hold glide → double-deflect semitone → FN+Play ×2 snap `1.0` | last rate exactly 1.0 | rate → 1.0 |
| 6 | Scrub | `transport.scrub.start` → 4 read pointers moving on a shared frame → track-level scrub-path output above floor → visible position move → `transport.scrub.end` → prior rate restored, landing error ≤2 frames | per-track scrub telemetry | none |
| 7 | Shape and chop | `filter.set` (FN+Fader) → `loop.chop` half/double (Play+Rocker) → double-deflect reset → hold glide → FN×4 grid tap | chopDiv / window / grid state; **zero** transport commands during chop gestures | window, filter, chop |
| 8 | FX | `fx.overlay` open → algorithm change (+/−) → momentary (hold Track) → latch (FN+Track) → `stem.select` with overlay still open | wet-path output differs from dry; overlay never closes | close overlay, clear momentary/latch |
| 9 | Heads | FN+Play ×3 enter → fader head levels → FN+Fader head scrub (audible head-path output) → exit | head level + head pointer movement | head state |
| 10 | Guided performance | cue+launch, multi-fader, mute/solo, rate move, scrub, chop, FX throw, in-overlay stem switch, heads in/out, stop | each step reuses its lesson's assertion, single pass, no retry gating | per user choice |

Ending card: "You can perform with Stem Tape." → Perform again / Keep these settings / Restore my original settings / Exit tutorial.

## 4. Files

**Add (4)**
- `src/onboarding/performanceSteps.ts` — 10 lesson definitions + ordered milestone arrays (data only).
- `src/onboarding/tutorialState.ts` — snapshot/restore, temporary loop creation, localStorage completion/dismissal.
- `src/onboarding/usePerformanceTutorial.ts` — subscribes to the existing command/ack stream, watermarking, milestone evaluation, failure classification.
- `src/onboarding/PerformanceTutorial.tsx` — card, coach pill, highlight driver, Retry/Skip/Exit.

**Change (2)**
- `src/device/DeviceSurface.tsx` — `pointer-events:none` highlight overlay driven from existing hit-zone geometry.
- `src/routes/index.tsx` — Learn button + tutorial mount.

Nothing else changes. No new tests infrastructure beyond `src/onboarding/__tests__/{milestones,snapshot}.test.ts` plus `tests/e2e/tutorial.spec.ts`.

## 5. Snapshot / restore

Snapshot is a structured clone of the reducer's serialisable surface state plus the engine's control-level state — fader values, mute map, solo mask, link mask, selected stem, rate/glide, window/filter, chopDiv/offset, grid, FX bank+algorithm+latch/momentary, heads state, loop bounds, transport position. No audio, no blob keys, no takes.

Restore replays the inverse as ordinary semantic commands (same path the UI uses), then asserts state equality against the snapshot; a mismatch surfaces as a restore warning rather than a silent divergence. Temporary loop bounds are always released on exit regardless of Keep/Restore choice.

## 6. Failure handling

If the expected command, ack, or engine result does not arrive within the milestone window: "This control did not respond as expected." with Retry · Skip · Open Diagnostics. The lesson id and the latest command/ack pair are appended to the existing diagnostics log (`DiagnosticPanel` / SYSTEM). No auto-advance, no new report system, no downloads.

## 7. UI

```text
 ┌──────────────────────────────────────┐
 │  rendered SP-1 surface               │
 │     ▢ highlighted control (ring)     │
 │                                      │
 │                                      │
 ├──────────────────────────────────────┤
 │ 3/10  Mix the stems                  │  <- card, docked opposite the highlight
 │ Move two faders at the same time.    │
 │ Desktop: Y/H and U/J                 │
 │ [Retry] [Skip]            [Exit]     │
 └──────────────────────────────────────┘

 during a fader gesture the card collapses to:
 ( 3/10  two faders at once … )   <- coach pill, 40px, corner
```

Card docks to whichever edge is farthest from the active highlight so a taught control is never covered; verified at 375/390/420 px, tablet, desktop. Desktop shows key equivalents, touch shows finger instructions. Reduced motion honoured; card is focus-trappable and keyboard-operable.

## 8. Estimate

- Preconditions P3–P7: the bulk of the work — remap + arbitration for chop, FX overlay verification, pump/heads measurement, keyboard-fader browser proof.
- Tutorial itself: 4 new files, ~900–1200 lines total, 2 small edits.
- Roughly 1 implementation pass for preconditions, 1 for the tutorial, 1 for browser verification.

## 9. Blockers

1. **P4 chop remap** is a behaviour change to the locked v2.6/v1 map; Lesson 7 is unbuildable until Play+Rocker is authoritative and `supersedes` is recorded.
2. **P5** — no evidence yet that `stem.select` preserves the FX overlay; if it does not, Lesson 8's final milestone needs an engine change first.
3. Lessons 6 and 9 are contingent on browser proof, not code presence.
