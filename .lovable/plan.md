# Phase 7 — Learn Stem Tape: Onboarding Wizard + Control-Map Reconciliation

Revised plan. No production code until approved.

## 1. Reconciled final gesture table (authoritative)

Rocker block, as approved. "Suppresses" = commands the arbiter must block **before dispatch**.

| Row id | Controls (ordered) | Command | Suppresses | Provenance | Change |
|---|---|---|---|---|---|
| `rocker.speed` | rocker fwd/rwd | `rate.set` ±1 BPM | — | v2.6 stock | unchanged |
| `rocker.glide` | rocker hold | `rate.set` glide | — | v2.6 stock | unchanged |
| `rocker.semitone` | rocker ×2 | `rate.set` ×2^(±1/12) | — | v2.6 stock | unchanged |
| `rocker.scrub` | function + rocker | `transport.scrub` (all four stems, continuous) | `fn.*` tap actions | extension, supersedes `rocker.chop` | **now exclusive — chop removed from FN layer** |
| `chop.step` | play + rocker fwd/rwd | `loop.chop` half/double | `transport.play`, `transport.stop`, `transport.cue`, play multi-tap txn | extension | **new binding** |
| `chop.reset` | play + rocker ×2 | `loop.chop` reset to window | same as above | extension | **new binding** |
| `chop.glide` | hold play + rocker | `loop.chop` continuous | same as above | extension | **moved off FN** |

Current code contradicts this at `surface.ts:508-526`: `fn` + rocker double-tap owns chop, and there is no play+rocker branch. The fix is a rocker-ownership claim in `chordArbiter.ts` that (a) marks Play as *claimed* the moment a rocker deflection arrives while Play is physically down, (b) cancels the pending Play tap/hold/multi-tap transaction before any command is emitted, and (c) routes the rocker to the chop family. Function+rocker keeps the scrub branch and loses its `g.count === 2` chop case.

Other reconciliations:

- **Track double-tap** — taught exactly as implemented (`surface.ts:452-474`, `recordingState.ts:198-204`): loaded+idle → recoverable delete; armed/waiting → cancel only; recording/overdubbing → stop only; stopping/finalising → acknowledgement only. `rec.undoPass` (`commands.ts:49`) stays unreachable from hardware; if the Input/Performance drawer exposes it, the lesson lives there and is marked VERIFY until the control is confirmed present.
- **LED priority** taught in full: error > failed PRINT > PRINT in progress > recording > overdubbing > armed/waiting > momentary FX > latched FX > Heads > soloed > unlinked > active > muted > base (matches `surface.ts:958-973`). Lessons state both the winning indication and the state hidden under it.

## 2. Source of truth

`stemTapeV1Map.ts` becomes the single registry. Existing `StemTapeRow` gains a complete, non-optional-by-default `tutorial` block:

```ts
interface TutorialMeta {
  gestureName: string;          // "Play + Rocker forward"
  doThis: string;               // imperative, plain language
  watchFor: string;             // visual result
  listenFor: string;            // audible result
  highlight: ControlId[];       // geometry.ts ids, ordered
  keyboard?: string[];          // "Space + Q"
  ledExplanation: string;       // winning tier + what it hides
  safety?: "modifies-audio" | "modifies-project" | "requires-input" | "none";
  restrictions?: MapLayer[];    // contexts where the row is inert
  completion: CompletionSpec;   // §4
  cleanup?: CleanupSpec;        // commands to restore entry state
  eligibility: "quick-start" | "curriculum" | "excluded";
  excludedReason?: string;      // required when excluded
}
```

Derived (never re-authored): onboarding instructions, GUIDE-tab atlas, `KEY_HINTS`, the mapping JSON export, and `guideCorrections.json`. Composite lessons reference row ids and add ordering only.

Coverage test: every `AudioCommandType` in `commands.ts` maps to ≥1 registry row with complete non-placeholder `tutorial`, or appears in an explicit exclusion list with a reason (`rollback`, `rec.recover`, schema-migration paths).

## 3. Completion model (event + ack based)

```ts
interface LessonRuntime {
  enteredAtSequence: number;      // AudioCommand.id watermark at lesson entry
  baseline: TutorialBaseline;     // transport phase, rate, mutes, active stem, heads, fx, project id
  milestones: LessonMilestone[];  // ordered
  observedCommands: AudioCommand[];
  acceptedAcks: Ack[];
  cleanup?: TutorialCleanup;
}
interface LessonMilestone {
  match: (cmd: AudioCommand, ack: Ack | null, ctx: TutorialCtx) => boolean;
  requireAck: AckStatus[];        // e.g. ["completed"]
  assert?: (ctx: TutorialCtx) => boolean;  // engine/state result
  ordered: true;
}
```

Rules: a milestone only counts for `cmd.id > enteredAtSequence`; it needs the required ack status from the engine ack stream (`useAudioEngine` already surfaces `acks`); and its `assert` must observe the engine result, not the reducer snapshot alone. Stale state can never complete a lesson.

Continuous controls have no semantic command today (faders go through `controlBus`). Add a **tutorial event stream**: `controlBus` and the fader session manager emit `{ channel, pointerId, value, batchFrame, t }` to a subscribable ring buffer used only by tutorials and diagnostics. Multi-finger lessons require ≥2 distinct `pointerId`s with movement inside the same `batchFrame`.

Worked examples: Play → new `transport.play` + `completed` ack + `status.position` advancing. Mute/return → `track.mute` then `track.unmute` on the same track index, both after entry. Cue → `transport.cue` completed + phase `cued` + position frame 0. Varispeed → `rate.set` away from 1.0, then an independent `rate.set` landing exactly 1.0. FX stem switch → `stem.select` while `fx.overlay` remains open (no `fx.overlay` close command between). Heads scrub → `heads.scrub` accepted + audible read position change reported by the engine.

## 4. Quick Start (10 lessons, ends "You can perform now.")

| # | Lesson | Completion signal | Cleanup |
|---|---|---|---|
| 1 | Enable audio + load demo | context `running` + 4 decoded tracks | none |
| 2 | Play / stop / resume | three separate transport commands, each completed | leave playing |
| 3 | Two faders at once | 2 pointer ids, same batch frame | restore entry fader values |
| 4 | Mute and unmute | mute then unmute, same track | restore mute map |
| 5 | Hold Play to cue, then launch | `transport.cue` completed, then `transport.play` | none |
| 6 | Select, solo, link/unlink | `stem.select`, `stem.solo`, `stem.link` | clear solo, restore link mask |
| 7 | ±1 BPM, semitone, snap 1.0× | 3 ordered `rate.set` milestones | rate → 1.0 |
| 8 | Function + Rocker four-stem scrub | `transport.scrub` accepted, position moves on all four | none |
| 9 | Window / filter, then Play + Rocker chop | `filter.set` + `loop.chop` without any transport command | restore window, filter, chop |
| 10 | FX overlay: apply, then switch stems inside it | `fx.momentary.start/end` + `stem.select` with overlay open | close overlay, clear momentary |

## 5. Full curriculum inventory (modules)

- **A. Mix and performance** (9 lessons) — roles, levels, 2/3/4-finger moves, opposing moves, mute, active stem, solo, link, linked-vs-independent targeting. Desktop shift-group and keyboard alternatives per lesson.
- **B. Transport and tape** (11) — play/stop/resume, wind curves, cue, launch profiles (Exact ships; tape pre-roll marked VERIFY/deferred), ±1 BPM, glide, semitone, snap, four-stem scrub, reverse, rapid reversal.
- **C. Window, chop, loops, grid** (14) — window start/end/shift/reverse, filter fader, chop half/double/reset/glide on the final Play+Rocker map, chop-window slide, fixed vs variable loops, polyrhythmic wraps, FN×4 learning, clear, round, beatmatch + rejection feedback.
- **D. Twelve FX** (4 bank lessons + 8 concept lessons) — each bank lesson walks all three algorithms; concept lessons cover overlay open/close, bank select, ± cycling, macro, momentary, latch/unlatch, clear all latches, Play+Volume stem switching in-overlay, per-stem retained state, FX in Heads, heavy-effect rejection/recovery, Pump ↔ grid tempo relationship.
- **E. Heads and PRINT** (11) — enter/exit, 0/25/50/75 %, levels, FN+Fader audible scrub, mute, reverse, source, FX in Heads, PRINT to empty track, PRINT progress/failure/recovery, PRINT inclusion/exclusion (**VERIFY-gated**).
- **F. Recording and varispeed SOS** (14) — enable/release input, monitoring modes, arm, onset + look-back, stop/finalise, overdub, grid punch + late window, varispeed SOS, multiple passes, undo newest pass via its real UI, interrupted-take recovery, latency compensation, dry WAV export, master performance recording.
- **G. Projects and songs** (10) — save/open/delete, 16 slots + bank nav, per-song snapshots, local-only storage, OPFS/IndexedDB, storage vs decoded memory, High Memory Mode, Memory Saver, recoverable trash, portable export/import (VERIFY if unimplemented).
- **H. System and accessibility** (9) — audio unlock, Bluetooth pairing gesture, keyboard map, pointer/multitouch, hit zones, diagnostics, Node vs Worklet engine, background/interruption recovery, privacy guarantee.

Schema migration is excluded from lessons and stays in developer diagnostics.

## 6. Tutorial project snapshot / restore

```ts
interface TutorialSession {
  id: string; startedAt: number; mode: "quick-start" | "module";
  previousProject: { id: string; name: string; saved: boolean };
  previousState: SurfaceState;               // serializable reducer snapshot
  createdArtifacts: { prints: string[]; takes: string[]; blobKeys: string[] };
  inputEnabledByTutorial: boolean;
}
```

Quick Start uses the ordinary four-stem demo. Entering Heads/PRINT or Recording training prompts to switch to a **training project**: three short demo stems + one deliberately empty track. On switch: persist `TutorialSession` to IndexedDB, snapshot project identity and reducer state, never write to the user's project. On exit or explicit abandon: stop tutorial media tracks and release the microphone, delete tutorial prints/takes/blobs unless the user chose Keep, restore the previous project and reducer state, clear the session record. A reload with a live session record shows a recovery prompt ("resume training / restore my project"). Every lesson with `safety !== "none"` renders a visible banner.

## 7. Keyboard implementation

- `Shift + R` → dispatches a `perf.record` semantic command through the reducer and command stream; the engine's ack drives the UI. `PerformanceRecorder` is never called from the key handler.
- `Escape` → calls the existing pointer-cancel/blur cleanup path (release held controls, cancel momentary FX, cancel pending multi-tap transactions, reconcile fader sessions). It must not clear latches, delete audio, stop a valid recording, or reset project state.
- Both handlers ignore `event.repeat`, and ignore events whose target is `input`, `textarea`, `select`, or `[contenteditable]`. Both are added to `KEY_MAP`-derived hints and to the registry's `tutorial.keyboard`.

## 8. Overlay behavior

`DeviceSurface` gains `highlight?: ControlId[]`, drawn from existing hit-zone geometry into a `pointer-events:none` layer above the SVG. The lesson card wrapper is `pointer-events:none`; only its own controls opt back in. Placement is target-aware: on mobile the card docks to whichever edge is farthest from the highlighted control, and collapses to a coach pill during multi-touch lessons; desktop uses the right rail. Content is three lines — Do this / Watch / Listen for. `aria-live="polite"`, full keyboard nav, visible focus, reduced-motion respected on the highlight pulse. Auto-advance ~600 ms after the success confirmation. Skip records `skipped`, distinct from `completed`. Exit, restart lesson, restart module, restart all always available. First visit offers Quick Start but never covers a loaded project.

## 9. Files

Add: `src/tutorial/registry.ts` (derivations + coverage), `src/tutorial/lessons/{quickstart,moduleA…moduleH}.ts`, `src/tutorial/runtime.ts` (`LessonRuntime`, milestone evaluation), `src/tutorial/tutorialProject.ts` (snapshot/restore), `src/tutorial/events.ts` (continuous-control tutorial stream), `src/tutorial/useTutorial.ts`, `src/tutorial/TutorialOverlay.tsx`, `src/tutorial/CoachPill.tsx`, `src/tutorial/guideCorrections.ts` + generated `public/guideCorrections.json`, `src/tutorial/__tests__/{coverage,completion,rocker,project}.test.ts`.

Change: `src/machine/stemTapeV1Map.ts` (full tutorial metadata + new rocker rows), `src/machine/surface.ts` (remove FN+rocker chop; add Play+rocker chop family), `src/machine/chordArbiter.ts` (rocker claims Play with pre-dispatch suppression), `src/audio/commands.ts` (`perf.record`; chop payload), `src/audio/engine.ts` (perf.record handling + ack; expose audible-scrub position for assertions), `src/audio/controlBus.ts` + `src/input/faderSessions.ts` (tutorial event emission with pointer id + batch frame), `src/device/useDeviceSurface.ts` (Shift+R, Escape, safety rules, KEY_HINTS), `src/device/DeviceSurface.tsx` (highlight layer), `src/routes/index.tsx` (learn entry point, overlay mount, code-derived GUIDE tab), `src/device/SystemPage.tsx` (mapping JSON export from registry).

## 10. VERIFY items (excluded from completion-gated lessons until proven)

Continuous wind reversal and source lifetime; exact dim/full-light hold duration; PRINT signal inclusion/exclusion (`print.ts` only encodes the buffer handed to it — the exclusion claim depends on `engine.ts`'s render path); reverse-direction recording rejection; permission-never-on-load; audible Heads scrubbing at browser level; final FN+Rocker vs Play+Rocker arbitration under all layers; Bluetooth pairing hold timing. Each gets a targeted source read plus an automated or Playwright test in Step 1 below; anything unresolved ships as a read-only "reference" lesson with no completion gate.

## 11. Guide synchronization

`guideCorrections.json` (generated from the registry, not hand-written) carries: final rocker/chop mapping, Track double-tap state table, complete LED priority including PRINT tiers, Bluetooth pairing gesture, Shift+R and Escape once shipped, and any verified PRINT/recording limitations. The wizard does not reproduce the PDF; the PDF stays the reference manual.

## 12. Test and acceptance matrix

| Area | Test |
|---|---|
| Rocker remap | FN+rocker scrubs all four stems; Play+rocker changes chop and emits **no** transport command; hold Play alone still cues frame zero; bare rocker still changes speed; all verified in tape / FX-overlay / Heads layers |
| v2.6 coverage | existing 37-row suite green; each intentional remap asserted as a Stem Tape extension with `supersedes` |
| Completion | every lesson rejects pre-entry state; ordered milestones enforced; ack status required |
| Multi-fader | 2/3/4 distinct pointer ids in one batch frame recognised; single-pointer sequence rejected |
| FX | overlay stays open across `stem.select`; all twelve algorithms present in curriculum (registry assertion) |
| Tutorial project | enter/restore leaves user project byte-identical; mic released; temp artifacts deleted; reload recovery |
| Keyboard | auto-repeat ignored; editable-target ignored; Escape preserves latches/recording/project |
| Layout | no overlay covers a required control at 375 / 390 / 420 px, tablet, desktop (Playwright screenshots) |
| Persistence | skipped ≠ completed; both survive reload |
| Regression | audio, recording, Heads, FX, transport suites green; `tsgo --noEmit` clean; zero console errors; network log contains no user audio |

Mobile runs are labelled emulation; real-iPhone verification requires the physical checklist.

## 13. Implementation sequence

1. Resolve VERIFY items (targeted reads + tests); publish results before writing lessons.
2. Rocker remap + arbitration suppression + regression tests.
3. Keyboard: Shift+R via command path, Escape via cleanup path, safety rules.
4. Registry metadata schema + coverage test + derived guide/atlas/hints/export.
5. Tutorial event stream for continuous controls.
6. Runtime (`LessonRuntime`, milestones, persistence) with unit tests.
7. Overlay + highlight layer + responsive placement.
8. Quick Start ten lessons, Playwright end-to-end.
9. Tutorial project snapshot/restore, then modules E and F.
10. Remaining modules A–D, G, H; `guideCorrections.json`; full acceptance matrix.
