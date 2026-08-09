# Phase 7 — Learn Stem Tape: Corrected Final Plan

Corrections 1-12 applied. No production code until this is approved.

---

## 1. Verification levels

The single `verified-working` label is withdrawn. Every capability carries the highest level it has actually reached:

`code-present` · `unit-tested` · `browser-verified` · `real-device-verified` · `known-bug` · `blocked` · `experimental`

A reducer test never counts as browser or audible verification. The Creator dashboard shows the highest achieved level per capability, with the evidence artifact (test name, Playwright run, device checklist) attached.

## 2. Re-issued capability matrix

| Capability | Level | Evidence |
|---|---|---|
| Keyboard scrub F+Q / F+A | **known-bug** | User-observed on live Mac build: no sound, no playhead movement, no position change. Repository grain-scheduler code (`engine.ts:1684-1806`) does not supersede this. |
| FN+Rocker scrub command contract | unit-tested | `surface.ts:268-287`; `globalScrub.test.ts` |
| FN+Rocker audible scrub | **known-bug** (same defect until proven otherwise) | no browser proof exists |
| Keyboard faders Y/H U/J I/K O/L | code-present | `keyboardMap.ts:38-45,88-105` |
| Multi-pointer faders | code-present | `faderSessions.ts` per-pointer sessions + shared `batchFrame`; no test |
| One-tap play/stop/resume, hold-Play cue frame 0 | unit-tested | `surface.ts:400-405,594-603`; `engine.ts:2158,2233,2279`; `transportCue.test.ts` |
| Wind-up / wind-down inertia | code-present | `inertia.ts`; no curve assertion |
| rate.set ±1 BPM / semitone / snap / glide | unit-tested | `surface.ts:396-399,558-567,658-661`; `engine.ts:2409-2437` |
| Window start/end/shift/reverse, filter | unit-tested | `surface.ts:829-844`; `engine.ts:2460-2472,2565` |
| Chop | code-present, **mis-mapped** | Currently FN+Rocker double-tap (`surface.ts:544-548`); no Play+Rocker branch exists |
| 11 of 12 FX algorithms | code-present | `banks.ts:79,99,134,176,204,239,254,298,354,383,426` |
| Beat Repeat | **blocked pending trace** | `banks.ts:488-491` returns `null`; connection to `public/beat-repeat-processor.js` unproven |
| Pump envelope | code-present | `banks.ts:204-208`; no measured envelope |
| Heavy-FX gating | **known-bug** | `fx12.ts:48,96` declares `heavy`; grep for `.heavy` in `engine.ts`, `fx/rack.ts`, `workletBudget.ts` returns zero hits — never read |
| FX overlay stem switching | code-present | `engine.ts:2632` has no overlay side effect; untested |
| Heads enter/exit, offsets, level | unit-tested | `surface.ts:381-393,820-827`; `engine.ts:2750-2767`; `heads.test.ts` |
| Heads scrub | unit-tested | `engine.ts:2784-2797` `scrubHead`+`restartHeadVoice`; `scrub.ts`, `scrub.test.ts`; not browser-verified |
| Heads mute / reverse / source | code-present | `surface.ts:453-464,610-621`; `engine.ts:2758-2782` |
| PRINT commit path | unit-tested | `print.ts:33-47` encode → `ingestStem` → local blob |
| PRINT render fidelity, inclusion/exclusion | blocked (VERIFY) | `printHeads()` unproven |
| Input enable/cancel/recover, monitoring off/dry/fx | unit-tested | `surface.ts:524-527,752-768`; `engine.ts:2685-2711`; `InputPanel.tsx:134-140` |
| Grid tap / quantise punch | code-present | `grid.ts`; `engine.ts:2727-2748` |
| Latency compensation | code-present | `input/latency.ts:13-58` |
| Undo newest pass | **blocked — unreachable** | handler `engine.ts:2716`; `undoPass` appears only at `commands.ts:51` and that handler |
| Timeline RateChange / GlideChange / ChopChange / WindowChange / LinkChange | unit-tested | `engine.ts:2437,2418,2508,2472,2629` |
| Timeline `LoopWrap` | **not-implemented** | only emitted in `transportCue.test.ts:157` |
| Timeline `DirectionChange` | **known-bug** | consumed at `engine.ts:331`, never emitted — dead guard |
| Master performance recorder | code-present | `engine.ts:342-345`; UI at `InputPanel.tsx:250-256` |
| Shift+R | not-implemented | `useDeviceSurface.ts` handles only `Escape` (`:260`) and `shiftKey` grouping (`:512`) |
| WAV export 16/24-bit, dither, fallback | unit-tested | `wavStream.ts:57-60,113-120`; `exportTake.ts:31` |
| 16 songs / 4 banks, recoverable trash, High Memory Mode, Memory Saver | unit-tested | `surface.ts:325,472`; `session.ts:38`, `store.ts:47-48`, `engine.ts:2375,2396`; `memory.ts:18,46,65-81`; `saver.ts`; `ProjectDrawer.tsx:171,200-204` |
| Portable `.stemtape` import/export | **not-implemented — out of scope for Phase 7** | reference-only mention, no completion gate, no `store.ts`/`ProjectDrawer.tsx` work |
| SVG highlight layer | not-implemented | no `highlight` in `DeviceSurface.tsx` |
| Registry tutorial metadata | partial | 16 of 18 `STEM_ROWS`; `V26_ROWS_AS_REGISTRY` (`stemTapeV1Map.ts:50-57`) carry none |

## 3. Step 0 — live scrub hotfix (before all other work)

Root-cause the whole live path in order, instrumenting each boundary, and stop at the first layer that breaks:

```text
keydown (real, Chromium + WebKit)
  -> key recognition in useDeviceSurface.ts (repeat filter, target filter, FN consumption)
  -> chord arbitration (FN claim, rocker claim, suppression)
  -> applyGlobalScrub -> transport.scrub.start command emitted
  -> engine drain by watermark (is the command reaching the engine at all?)
  -> ack: accepted / completed / rejected
  -> globalScrubTick scheduling (is the RAF/interval running?)
  -> per-track grain source creation and connection point
  -> read-pointer advance per track
  -> output routing (does the grain reach the track input node, faders, master?)
  -> position readout / waveform
  -> keyup -> transport.scrub.end -> rate restored at the new position
```

Test method: Playwright against the rendered production surface in **both Chromium and WebKit**, driving real `keyboard.down("KeyF")` / `keyboard.down("KeyQ")` events. No direct engine calls, no synthetic pointer substitutes.

Required proof, all of it, per browser:

1. `transport.scrub.start` present in the command log with the correct direction payload.
2. Ack status `completed` for that command id.
3. `scrubActive === true` on all four tracks.
4. Four read pointers move in the correct direction, each above a minimum displacement threshold, applied on a shared frame.
5. **Track-level output attributed to the scrub path** above the silence floor — measured on a per-track scrub tap, not master RMS, because normal stems can stay audible while the scrub processor is silent.
6. Visible position movement in the UI.
7. `transport.scrub.end` on release.
8. Prior musical rate restored, at the new position, with no wind-down artifact.

If the defect is in cleanup or arbitration rather than DSP, the fix is scoped there; the proof set is unchanged.

## 4. Correctness blockers (Step 1, after the hotfix)

1. Emit `LoopWrap` from the engine loop-wrap path.
2. Emit `DirectionChange` on reverse, activating the recorder guard at `engine.ts:331`.
3. **Undo Newest Pass in the Input/Performance Drawer only.** `surface.ts` gains no undo binding. Track double-tap keeps its approved state-safe table: loaded+idle → recoverable delete; armed/waiting → cancel; recording/overdubbing → stop; stopping/finalising → acknowledgement only (`recordingState.ts:198-205`). The drawer control emits `rec.undoPass` and removes only the newest pass; the lesson teaches it there.
4. Heavy-FX operational gating (see §6).
5. `Shift+R` → dispatches `perf.record` through the command stream; `PerformanceRecorder` is never called from the key handler; `event.repeat` and editable targets ignored.
6. Chop remap and arbitration (see §5).
7. `DeviceSurface` highlight layer, drawn from existing hit-zone geometry into a `pointer-events:none` overlay.

Then **Step 2: re-audit through the real browser** — Beat Repeat, Pump, FX stem switching, Heads scrub, PRINT, SOS, recording, export — and upgrade each capability's verification level from the evidence, not from code reading.

## 5. Chop remap and arbitration

| Gesture | Command | Suppresses |
|---|---|---|
| FN + rocker (single) | `transport.scrub.start/end` | fn tap actions |
| Play + rocker fwd/rwd | `loop.chop` half / double | `transport.play/stop/cue`, pending play txn |
| Play + rocker ×2 | `loop.chop` reset | same |
| Hold Play + rocker | `loop.chop` glide | same |
| Bare rocker | `rate.set` | — |

`chordArbiter.ts` claims Play the moment a rocker deflection arrives while Play is physically down, cancels the pending Play transaction **before dispatch**, and routes the rocker to the chop family. FN+rocker loses its `g.count === 2` chop case. The 37-row v2.6 suite stays green; each remap is recorded as `supersedes`.

## 6. Heavy-FX rejection semantics (corrected)

On rejection: the command acks `rejected`; the requested algorithm is **not** marked active, momentary, or latched; the previously selected algorithm keeps playing; the bank stores a visible rejection reason and status; every other bank continues running unaffected; the UI offers a named cheaper alternative and a recovery action. The lesson teaches exactly this recovery.

## 7. Audible assertion contracts

- **Global scrub** — `scrubActive` on all targeted tracks; expected direction and minimum displacement per read pointer; shared apply frame; track-level output attributed to the scrub path; restoration on release.
- **Heads scrub** — selected head pointer movement; head-voice generation/restart evidence; output contribution measured on the head path; unrelated head pointers must not move unless intentionally grouped.
- **FX** — algorithm-specific wet-path output; measurable wet/dry difference against a controlled fixture; a pass may not come from the unaffected dry signal.
- **Pump** — measured periodic gain modulation, expected relationship to effective BPM, minimum modulation depth.
- **Beat Repeat** — selection traced through the bank adapter into `public/beat-repeat-processor.js`; because `banks.ts:488-491` returns `null`, the actual connection is proven explicitly or the algorithm is marked `blocked`.

## 8. Registry separation

`stemTapeV1Map.ts` stays authoritative for **SP-1 and keyboard gestures only**. Drawer-only and system actions are never forced into physical mapping rows.

A new `src/tutorial/features.ts` holds the unified feature registry. Each `FeatureEntry` references, optionally: an SP-1 mapping row id, a keyboard mapping, a drawer action id, a system action id, a semantic command, its verification level and evidence, and its lesson ids — plus the instructional fields (what this does / do this / watch for / listen for / why / try in performance), highlight control ids, LED explanation, restrictions.

Derived from it, never re-authored: tutorial instructions, Reference atlas, keyboard panel copy, coverage dashboard, mapping JSON export, guide corrections. A coverage test asserts every `AudioCommandType` maps to ≥1 feature entry or an explicit exclusion with a reason.

## 9. Uploaded-audio analysis (corrected)

Chunked, sequential, bounded. For each stem: iterate the decoded buffer in ~4-second chunks via `copyFromChannel` into **one reusable working buffer**, downmix to mono in place, accumulate features, discard. No second full-length PCM copy, no second decode. Existing waveform/peak data is reused wherever it already exists.

Retained maps (50 Hz / 20 ms hop): RMS-peak envelope, transient index list, sustain score, activity/silence mask, cross-stem overlap.

**Memory, corrected and reported separately:**

- Retained maps, 4-minute song, 4 stems: `50 samples/s × 4 bytes × 4 stems × 240 s = 192,000 bytes ≈ 0.18 MiB`, plus transient indices and masks ≈ 0.05 MiB → **≈ 0.23 MiB retained**.
- Peak working memory: one reusable chunk buffer, 4 s × 48 kHz × 4 bytes × 2 channels = **≈ 1.5 MiB**, released after region selection.

Stem role comes from the **uploaded role assignment**, not from inference. Spectral centroid is not computed from low-rate data; higher-bandwidth spectral features are computed only over bounded candidate sections, and only where they materially improve the pick. All temporary buffers are released after region selection.

Region selection: mixing → highest four-stem overlap; vocal FX → vocal-role stem, highest sustain in a bounded spectral check; drum FX/chop → highest transient density; Freeze/Scatter → longest sustain, lowest flux; grid/recording → transient-rich; scrub → longest continuous non-silent range. Every lesson shows the range and offers "choose another section". Tutorial loops and cues are temporary unless kept.

## 10. Input-aware completion

Milestones declare `supportedInputs`. A multi-fader milestone accepts either evidence form, and the diagnostic record labels which:

- **Touch** — ≥2 distinct `pointerId`s, overlapping movement, same or overlapping `batchFrame`.
- **Keyboard** — ≥2 distinct fader channels, overlapping held-key intervals, shared `batchFrame`. Pairs: Y/H, U/J, I/K, O/L.

Keyboard evidence is never labelled multipointer. The advanced module verifies two-, three- and four-fader movement including opposing directions, in whichever input mode is active.

General completion rules are unchanged: `enteredAtSequence` watermark on `AudioCommand.id`; required ack status observed on the ack stream; `suppressedCommands` present in the window fail the milestone; engine assertion reads engine state; audible assertion uses the §7 contracts; visual assertion reads LED arbitration (`surface.ts:958-973`).

### Quick Start (10) — updated criteria

| # | Lesson | Completion | Cleanup |
|---|---|---|---|
| 1 | Enable audio, confirm four stems and roles | context `running`, 4 decoded tracks | none |
| 2 | Play / stop / resume | 3 transport commands, each `completed` | leave playing |
| 3 | Two faders at once | touch: 2 pointer ids overlapping in one batchFrame · keyboard: 2 fader channels with overlapping held-key intervals in one batchFrame — plus gain change on both | restore fader values |
| 4 | Mute and unmute | `track.mute` then `track.unmute`, same index | restore mute map |
| 5 | Hold Play to cue, then launch | `transport.cue` completed at frame 0, then `transport.play` | none |
| 6 | Select, solo, link/unlink | `stem.select`, `stem.solo`, `stem.link` | clear solo, restore mask |
| 7 | ±1 BPM, semitone, snap 1.0 | 3 ordered `rate.set`, last exactly 1.0 | rate → 1.0 |
| 8 | Four-stem scrub (FN+Rocker or F+Q/A) | full §3 proof set at lesson scope: scrub start, completed ack, 4 pointers moving, track-level scrub output, end, rate restored | none |
| 9 | Window/filter, then Play+Rocker chop | `filter.set` + `loop.chop`, **zero** transport commands | restore window/filter/chop |
| 10 | FX overlay: apply, then switch stems inside it | `fx.momentary.start/end` + `stem.select` with no `fx.overlay` close | close overlay, clear momentary |

Modules 0-12 and the capstone follow the approved brief; Module 9 teaches Undo Newest Pass from the drawer; Module 11 marks portable import/export as a future capability with no completion gate.

## 11. Copy-on-write Tutorial Copy

Entering PRINT or Recording training creates a **new project manifest that references the existing immutable source blob keys**. No blob duplication, no second decode, no duplicated decoded PCM. Copy-on-write applies only to tutorial-created takes, PRINTs, derived audio and changed metadata. Estimated memory impact is shown **before** entering the module.

The user chooses which track is temporarily emptied, and the chooser names the musical material that will be absent ("Drums — the beat will drop out; the other three stems keep playing"). Restoration is by project identity and reducer snapshot, never by duplicating audio. Tracked for rollback: windows, loops, chop, FX state, mutes, takes, PRINT buffers, created blob keys, `inputEnabledByTutorial`. After every experiment: Keep this change / Restore lesson / Retry / Exit tutorial. On exit: stop tutorial mic tracks, delete tutorial-created artifacts unless kept, restore. Reload with a live session offers Resume or Restore Original.

## 12. Bug discovery, diagnostics, dashboard (retained)

Failure classification at the first failing layer: gesture-not-recognized, arbitration-conflict, wrong-command, command-rejected, reducer-changed-engine-did-not, engine-changed-audio-silent, visual-state-incorrect, persistence-failed, cleanup-failed, browser-limitation. The lesson never auto-advances and never blames the user.

Local diagnostic JSON: lesson and feature ids, UA/device, input method **and evidence form**, raw gesture sequence, ordered chord, suppressed commands, command ids, acks, state before/after, engine mode, AudioContext state, playhead/read-pointer telemetry, per-path output measurements, console errors, timings, memory/storage verdict. Never contains user audio. Actions: Retry, Skip and report, Open diagnostics, Download JSON.

Creator dashboard ships as a SYSTEM sub-tab: passed/failed/skipped lessons, highest verification level per capability with its evidence artifact, first failing layer per failure, coverage against the feature registry.

## 13. Files

**Add** — `src/tutorial/features.ts` (unified feature registry), `src/tutorial/registry.ts` (derivations + coverage), `src/tutorial/lessons/{quickstart,module0…module12,capstone}.ts`, `src/tutorial/runtime.ts`, `src/tutorial/completion.ts`, `src/tutorial/events.ts` (input-aware evidence), `src/tutorial/diagnostics.ts`, `src/tutorial/tutorialProject.ts` (copy-on-write), `src/tutorial/regions.ts`, `src/workers/analysisWorker.ts`, `src/tutorial/useTutorial.ts`, `src/tutorial/TutorialOverlay.tsx`, `src/tutorial/CoachPill.tsx`, `src/tutorial/CoverageDashboard.tsx`, `src/tutorial/__tests__/{coverage,completion,rocker,regions,project,diagnostics,inputEvidence}.test.ts`, `src/audio/__tests__/{scrubLive,fxMeasure,pump,beatRepeat,timelineSegments,multiFader}.test.ts`, `tests/e2e/{scrub,quickstart,layout}.spec.ts`.

**Change** — `engine.ts` (scrub hotfix, `LoopWrap`/`DirectionChange` emission, heavy-FX gating, per-path telemetry taps), `useDeviceSurface.ts` (scrub key path fix, Shift+R, Escape safety), `chordArbiter.ts` (rocker claims Play), `surface.ts` (chop remap only — **no undo-pass binding**), `stemTapeV1Map.ts` (chop rows, `supersedes`, gesture metadata for v2.6 rows), `fx/rack.ts` + `fx/banks.ts` + `workletBudget.ts` (read `heavy`, prove Beat Repeat routing), `controlBus.ts` + `faderSessions.ts` (tutorial evidence emission), `DeviceSurface.tsx` (highlight layer), `InputPanel.tsx` (Undo Newest Pass control), `keyboardMap.ts` (derive from feature registry), `SystemPage.tsx` (coverage sub-tab, registry-derived mapping export), `routes/index.tsx` (Learn entry, overlay mount, registry-derived GUIDE).

**Not changed for portability work** — `store.ts` and `ProjectDrawer.tsx` gain no `.stemtape` import/export.

**Storage** — new `tutorial` IndexedDB store: `progress`, `session`, `diagnostics` (capped ring). Additive, versioned; existing project stores untouched.

## 14. Test matrix

| Area | Test |
|---|---|
| Step 0 scrub | Playwright Chromium **and** WebKit, real key events, full 8-point proof set; failing at any point blocks the phase |
| Chop remap | Play+rocker changes chop with **no** transport command; FN+rocker still scrubs; hold Play alone still cues frame 0; bare rocker still sets rate; across tape / FX-overlay / Heads layers |
| v2.6 regression | 37-row suite green; remaps asserted as extensions with `supersedes` |
| Undo | drawer control removes only the newest pass; `surface.ts` emits no `rec.undoPass`; Track double-tap table unchanged |
| Heavy FX | rejected ack; algorithm not active/momentary/latched; previous algorithm still audible; bank shows rejection reason; other banks unaffected; alternative offered |
| FX / Pump / Beat Repeat | §7 contracts, controlled fixtures, no dry-signal false pass |
| SOS | `RateChange`, `GlideChange`, `LoopWrap`, `LinkChange`, `DirectionChange` all emitted during a varispeed overdub; constant-rate manifest rejected |
| Multi-fader | touch: 2/3/4 pointer ids in one batchFrame · keyboard: 2/3/4 channels with overlapping held intervals; sequential single-channel rejected; opposing directions verified |
| Completion | pre-entry state never completes; ordered milestones; ack required; suppression violation fails |
| Tutorial Copy | no blob duplication, no second decode, memory delta reported; user project restored by identity; mic released; artifacts deleted; reload recovery |
| Keyboard | Shift+R via command path; auto-repeat ignored; editable targets ignored; Escape preserves latches, recording, project |
| Layout | Playwright at 375/390/420 px, tablet, desktop — no overlay covers a taught control |
| Privacy | network log contains no user audio across a full Quick Start run |
| Regression | full suite green, `tsgo --noEmit` clean, zero console errors |

Emulated runs are labelled emulation; `real-device-verified` requires the physical iPhone/iPad checklist.

## 15. Implementation order

1. **Step 0** — live scrub hotfix with the full browser proof set.
2. Correctness blockers: `LoopWrap`, `DirectionChange`, drawer-based undo, heavy-FX gating, Shift+R, chop remap + arbitration, SVG highlight layer.
3. Real-browser re-audit: Beat Repeat, Pump, FX stem switching, Heads scrub, PRINT, SOS, recording, export — update verification levels from evidence.
4. Separated mapping / feature / tutorial registries.
5. Input-aware tutorial events and feature-specific telemetry.
6. Bounded uploaded-audio analysis and region selection.
7. Completion runtime, bug diagnostics, persistence, Creator dashboard.
8. Overlay, highlights, Quick Start.
9. Copy-on-write Tutorial Copy.
10. Recording, PRINT, SOS, remaining modules, capstone.
11. Full feature-validation matrix; generate guide corrections **only from confirmed behavior**.

## 16. VERIFY / reference-only

PRINT inclusion-exclusion; continuous wind reversal and source lifetime; exact dim/full-light hold duration; reverse-direction recording rejection (after `DirectionChange` lands); Bluetooth pairing hold timing; iOS delivery limits for large WAV exports; portable project import/export. Each ships as a read-only reference lesson with an `unverified` or `not-implemented` badge and no completion gate until proven.
