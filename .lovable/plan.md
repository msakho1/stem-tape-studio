# Phase 7 — Learn Stem Tape: Full Interactive Tutorial + Functionality Validation

Plan only. Every current-state claim below is backed by a read of the repository in this turn; each is cited `file:line`.

---

## 1. Capability audit (verified this turn)

### 1.1 Command chain is complete
All 48 `AudioCommandType` values (`commands.ts:14-66`) have a matching `case` in the engine dispatch (`engine.ts:2157-2810`), plus a `default:` that acks `"unknown command type"` (`engine.ts:2808`). Commands carry a monotonic `id` (`commands.ts:72`, `nextCommandId()` `commands.ts:95-97`), an optional `txnId`/`rowId`, and acks use `AckStatus = "accepted" | "completed" | "rejected" | "failed"` (`commands.ts:83`) broadcast from `engine.ack()` (`engine.ts:369-374`). This is exactly the substrate the completion model needs — no new command plumbing required.

### 1.2 Matrix

| Feature | Class | Evidence |
|---|---|---|
| FN+Rocker four-stem scrub (command contract) | verified-working | `surface.ts:268-287` emits paired `transport.scrub.start/end`, rejects while recording; `globalScrub.test.ts` |
| …its audible output | implemented-but-unverified | `engine.ts:1684-1806` real grain scheduler (`GLOBAL_SCRUB_RATE=3`); no automated audio assertion |
| Keyboard scrub F+Q / F+A | implemented-but-unverified | `keyboardMap.ts:16-17`, intercepted in `useDeviceSurface.ts` |
| Keyboard faders Y/H U/J I/K O/L | implemented-but-unverified | `keyboardMap.ts:38-45,88-105`; no test |
| Multi-pointer simultaneous faders | implemented-but-unverified | `faderSessions.ts` per-pointer sessions, `flush()` shared `batchFrame`; no multi-pointer test file exists |
| One-tap play/stop/resume | verified-working | `surface.ts:400-405`; `engine.ts:2158,2233`; `transportCue.test.ts`, `tape.test.ts` |
| Hold Play → cue frame 0 | verified-working | `surface.ts:594-603`; `engine.ts:2279`; `transportCue.test.ts` |
| Wind-up / wind-down inertia curve | implemented-but-unverified | `inertia.ts`; no dedicated curve assertion found |
| rate.set ±1 BPM / semitone / snap 1.0 / glide | verified-working | `surface.ts:558-567`, `396-399`, `658-661`; `engine.ts:2409-2437` emits `RateChange`+`GlideChange` |
| Window start/end/shift/reverse | verified-working | `surface.ts:829-844`; `engine.ts:2460`, `WindowChange` emitted at `engine.ts:2471-2472` |
| Filter fader | verified-working | `surface.ts:831-834`; `engine.ts:2565` |
| Chop half/double/reset/glide | implemented, **mapped to FN+Rocker double-tap** | `surface.ts:544-548` (`if (fn) { if (g.count===2) … }`); `engine.ts:2500` emits `ChopChange:2508`. **There is no Play+Rocker chop branch anywhere.** |
| 11 of 12 FX algorithms | verified-working (DSP present) | `banks.ts` — filter `79`, isolator `99`, dirt `134`, gate `176`, pump `204-208`, echo `239`, pitchEcho `254`, scatter `298`, reverb `354`, shimmer `383`, freeze `426` |
| Beat Repeat inside banks stage | audio-not-connected in `banks.ts` (delegated) | `banks.ts:488-491` returns `null`; DSP lives in `public/beat-repeat-processor.js` |
| Pump gain envelope | implemented-but-unverified | `banks.ts:204-208` real LFO VCA, duck depth 0.325; no measured-envelope test |
| Heavy-FX rejection | **known-bug / not-enforced** | `fx12.ts:48,96` declare `heavy: true` for scatter/shimmer/freeze, but grep for `.heavy` in `engine.ts`, `fx/rack.ts`, `workletBudget.ts` returns **zero hits** — the flag is never read |
| FX overlay stem switching | implemented-but-unverified | `stem.select` (`engine.ts:2632`) has no overlay side effect; no test asserts overlay stays open |
| Heads enter/exit, 0/25/50/75 %, level | verified-working | `surface.ts:381-393`, `820-827`; `engine.ts:2750-2767`; `heads.test.ts` |
| Audible head scrub | verified-working (engine restarts voices) | `engine.ts:2784-2797` `scrubHead` + `restartHeadVoice`; `scrub.ts`; `scrub.test.ts` |
| Heads mute / reverse / source | implemented-but-unverified | `surface.ts:453-464`, `610-621`; `engine.ts:2758-2782` |
| PRINT commit path | verified-working | `print.ts:33-47` encode → `ingestStem` → local blob, nothing uploaded; `engine.ts:2800-2807` accepted-then-completed acks |
| PRINT render fidelity / inclusion-exclusion | VERIFY | `printHeads()` render path not proven; the guide claim cannot be taught until measured |
| Input enable / cancel / recover | verified-working | `surface.ts:752-768`, `524-527`; `engine.ts:2685-2711` |
| Monitoring off / dry / fx | verified-working | `InputPanel.tsx:134-140`, `recorder.ts` `MonitorMode` |
| Grid tap / quantise punch | implemented-but-unverified | `grid.ts`; `engine.ts:2727-2748` |
| Latency compensation | implemented-but-unverified | `input/latency.ts:13-58` model + impulse detection; not wired into an assertion |
| **Take undo (`rec.undoPass`)** | **mapped-but-unreachable** | Handler at `engine.ts:2716`; grep for `undoPass` across `src` returns only `commands.ts:51` and that handler — **nothing ever emits it** |
| Timeline `RateChange`, `GlideChange`, `ChopChange`, `WindowChange`, `LinkChange` | verified-working | `engine.ts:2437, 2418, 2508, 2472, 2629` |
| Timeline **`LoopWrap`** | **not-implemented** | Only emitted inside `transportCue.test.ts:157`; no engine emit site |
| Timeline **`DirectionChange`** | **known-bug** | Consumed at `engine.ts:331` (recorder rejects reverse) but never emitted by engine — that guard is dead code |
| Master performance recorder | implemented-but-unverified | `PerformanceRecorder` `engine.ts:342-345`; UI trigger exists at `InputPanel.tsx:250-256` |
| **Shift+R keybinding** | **not-implemented** | `useDeviceSurface.ts` handles only `Escape` (`:260`) and `shiftKey` for fader grouping (`:512`) |
| WAV export, 16/24-bit, dither, fallback | verified-working | `wavStream.ts:57-60,113-120`; `exportTake.ts:31`, used at `InputPanel.tsx:223` |
| 16 songs / 4 banks | verified-working | `surface.ts:325,472` |
| Recoverable trash | verified-working | `session.ts:38`, `store.ts:47-48`, `engine.ts:2375,2396` |
| High Memory Mode / Memory Saver | verified-working | `memory.ts:18,46,65-81`; `saver.ts`; `ProjectDrawer.tsx:171,200-204` |
| **`.stemtape` import/export** | **not-implemented** | Case-insensitive grep for `stemtape`, `exportProject`, `importProject` across `src` returns nothing |
| `DeviceSurface` highlight layer | not-implemented | No `highlight` reference in `DeviceSurface.tsx`; only `tutorial.highlight` data exists in the registry |
| Registry tutorial coverage | partially-implemented | 16 of 18 `STEM_ROWS` have `tutorial:`; `V26_ROWS_AS_REGISTRY` (`stemTapeV1Map.ts:50-57`) carry none |
| `controlBus` pointerId + batchFrame | verified-working | `controlBus.ts:27,36`; `faderSessions.ts` monotonic `batchFrame` |

### 1.3 Blocking defects to fix before the course is executable
Per your decision (**fix first, then teach**), these ship inside Phase 7:

1. `rec.undoPass` unreachable — no gesture or UI emits it.
2. `LoopWrap` never emitted — Module 10 cannot prove SOS segment tracking.
3. `DirectionChange` never emitted — reverse-during-record guard is dead.
4. Heavy-FX `heavy` flag never read — no rejection path to teach or test.
5. No `Shift+R` binding.
6. No `.stemtape` import/export — Module 11 lesson is otherwise fiction.
7. Chop ownership contradicts the approved map (FN+Rocker double-tap today; approved map says Play+Rocker).
8. `DeviceSurface` cannot highlight a control.

---

## 2. Chop remap and arbitration

Approved final map, replacing `surface.ts:541-548`:

| Gesture | Command | Suppresses |
|---|---|---|
| FN + rocker (single) | `transport.scrub.start/end` | fn tap actions |
| Play + rocker fwd/rwd | `loop.chop` half/double | `transport.play/stop/cue`, pending play txn |
| Play + rocker ×2 | `loop.chop` reset | same |
| Hold Play + rocker | `loop.chop` glide | same |
| Bare rocker | `rate.set` | — |

`chordArbiter.ts` gains a rocker-ownership claim: the moment a rocker deflection arrives while Play is physically down, Play is marked *claimed*, its pending tap/hold/multi-tap transaction is cancelled **before dispatch**, and the rocker routes to the chop family. FN+rocker keeps scrub and loses its `g.count === 2` chop case. The 37-row v2.6 suite stays green; each remap is recorded in `stemTapeV1Map.ts` as `supersedes`.

---

## 3. Unified feature/tutorial registry

`stemTapeV1Map.ts` becomes the only source of truth. `StemTapeRow.tutorial` is widened from today's single `plainLanguage` string to:

```ts
interface TutorialMeta {
  gestureName: string;
  whatThisDoes: string; doThis: string; watchFor: string;
  listenFor: string; whyUseIt: string; tryInPerformance: string;
  highlight: ControlId[];          // geometry.ts ids
  keyboard?: string[];             // "KeyF+KeyQ"
  ledExplanation: string;          // winning tier + what it hides
  restrictions?: MapLayer[];
  verification: "verified" | "unverified" | "blocked" | "reference-only";
  lessonIds: string[];
  eligibility: "quick-start" | "curriculum" | "excluded";
  excludedReason?: string;
}
```

Derived, never re-authored: lesson instructions, GUIDE atlas, `keyboardMap.ts` detail text, mapping JSON export, coverage dashboard. A coverage test asserts every `AudioCommandType` maps to ≥1 row with complete non-placeholder metadata or an explicit exclusion (`rollback`, schema-migration paths). Today's gap: `V26_ROWS_AS_REGISTRY` rows get metadata for the first time.

---

## 4. Uploaded-audio analysis (full feature analysis, per your choice)

Runs once at tutorial start in `src/workers/analysisWorker.ts`, fed **transferred copies of already-decoded channel data, downmixed to mono and decimated to 1 kHz before transfer** — no second full-rate PCM copy is retained. Memory cost: 1 kHz × 4 bytes × 4 stems × duration ≈ **0.9 MiB for a 4-minute song**; feature maps add ~0.3 MiB. Analysis buffers are released after the maps are built.

Computed per stem: RMS/peak envelope (20 ms hop), spectral-flux transient list, spectral-centroid tonality, sustain score, silence mask; cross-stem overlap map and loop-safe candidate regions (bar-aligned when a grid BPM exists, otherwise transient-aligned).

Region selection: mixing → highest four-stem overlap; vocal FX → highest tonality on the vocal-role stem; drum FX/chop → highest transient density; Freeze/Scatter → longest sustain, low flux; grid/recording → transient-rich; scrub → longest continuous non-silent range. Every lesson shows the chosen range on the waveform with a "choose another section" control; picks and temporary loops/cues are discarded unless kept.

---

## 5. Three surfaces

- **Quick Start** — 10 lessons, ends "You can perform now."
- **Learn Stem Tape** — Modules 0-12 plus capstone.
- **Reference** — searchable, registry-derived atlas: gesture, keyboard, layer, expected LED, expected audio, restrictions, verification badge, launch-lesson button.

Progress (`completed` / `skipped` / `failed`, distinct states) persists in IndexedDB and survives reload.

---

## 6. Completion model

Implemented as specified in your brief (`TutorialLesson` / `TutorialMilestone`). Enforcement rules:

- `enteredAtSequence` watermark from `AudioCommand.id`; only `cmd.id > watermark` counts.
- `requiredAck` must be observed on the ack stream (`engine.onAck`).
- `suppressedCommands` present in the window → milestone **fails**, not ignored.
- `engineAssertion` reads engine state, never the reducer snapshot alone.
- `audibleAssertion` reads a new master/track RMS + read-pointer telemetry tap (already partially exposed via `useAudioEngine`); scrub and heads lessons require read-pointer movement on all four engines, not a gesture match.
- `visualAssertion` reads the LED arbitration result from `surface.ts:958-973`.
- Continuous controls have no semantic command; a tutorial event stream on `controlBus`/`faderSessions` supplies `{channel, pointerId, value, batchFrame, t}`. Multi-finger lessons require ≥2 distinct `pointerId`s inside one `batchFrame` — a sequential single-pointer run is rejected.

### Quick Start (10)

| # | Lesson | Completion | Cleanup |
|---|---|---|---|
| 1 | Enable audio, confirm four stems and roles | context `running`, 4 decoded tracks | none |
| 2 | Play / stop / resume | 3 transport commands, each `completed` | leave playing |
| 3 | Two faders at once | 2 pointer ids in one batchFrame + gain change | restore fader values |
| 4 | Mute and unmute | `track.mute` then `track.unmute`, same index | restore mute map |
| 5 | Hold Play to cue, then launch | `transport.cue` completed at frame 0, then `transport.play` | none |
| 6 | Select, solo, link/unlink | `stem.select`, `stem.solo`, `stem.link` | clear solo, restore mask |
| 7 | ±1 BPM, semitone, snap 1.0 | 3 ordered `rate.set`, last exactly 1.0 | rate → 1.0 |
| 8 | FN+Rocker four-stem scrub | scrub start/end + RMS > floor + 4 read pointers move | none |
| 9 | Window/filter, then Play+Rocker chop | `filter.set` + `loop.chop`, **zero** transport commands | restore window/filter/chop |
| 10 | FX overlay: apply, then switch stems inside it | `fx.momentary.start/end` + `stem.select` with no `fx.overlay` close | close overlay, clear momentary |

Modules 0-12 and the capstone follow your brief exactly; per-lesson milestone specs are authored into `src/tutorial/lessons/*` with the same fields.

### Lessons runnable today vs blocked

Runnable now: Modules 0, 1, 2, 3 (scrub audibility becomes a proven assertion), 4 (after chop remap), 6 except heavy-FX rejection, 7, 11 except import/export, 12 except Shift+R.
Blocked until the §1.3 fixes land: take undo (M9), SOS segment proof (M10), heavy-FX rejection (M6), Shift+R (M12), portable project import/export (M11), PRINT inclusion/exclusion claims (VERIFY).

---

## 7. Tutorial Copy and restoration

Performance lessons run on the user's loaded project. PRINT and recording modules require a Tutorial Copy: original blobs untouched, reducer state and project identity snapshotted to IndexedDB, one track emptied — **the user chooses which of the four roles to empty** before the module starts, with the remaining three as backing.

Tracked for rollback: windows, loops, chop, FX state, mutes, takes, PRINT buffers, created blob keys, `inputEnabledByTutorial`. After every experiment: Keep this change / Restore lesson / Retry / Exit tutorial. On exit: stop tutorial mic tracks, delete tutorial artifacts unless kept, restore project and reducer state. Reload with a live session offers Resume or Restore Original. Nothing writes into the original project without explicit confirmation.

---

## 8. Bug-discovery system

On a gesture that arrives but fails downstream, the lesson does not advance and does not blame the user. It classifies the first failing layer: gesture-not-recognized, arbitration-conflict, wrong-command, command-rejected, reducer-changed-engine-did-not, engine-changed-audio-silent, visual-state-incorrect, persistence-failed, cleanup-failed, browser-limitation.

Diagnostic record (local only, never contains user audio): lesson and feature ids, UA/device, input method, raw gesture sequence, ordered chord, suppressed commands, command ids, acks, state before/after, engine mode (Node vs Worklet), AudioContext state, playhead/read-pointer telemetry, console errors, timings, memory/storage verdict. Actions: Retry, Skip and report, Open diagnostics, Download bug-report JSON.

**Creator dashboard ships in this phase** as a fourth SYSTEM sub-tab: passed/failed/skipped lessons, unverified features, first failing layer per failure, overall feature coverage against the registry.

---

## 9. Desktop and touch

Desktop: dismissible Keyboard Controls panel (already built, `KeyboardPanel.tsx`) gains lesson-scoped emphasis and held-key highlighting; simultaneous fader-key lessons require overlapping keydowns. Touch: genuine multipointer lessons for 2/3/4 fingers, scroll prevented only on the performance surface, pointercancel always releases. Input method is auto-detected and user-overridable at any point in the course. Emulated runs are labelled as emulation, distinct from real-device checks.

Lesson card: non-modal, `pointer-events:none` wrapper with only its own controls opting in, target-aware placement (docks to the edge farthest from the highlighted control; collapses to a coach pill during multi-touch), `aria-live="polite"`, keyboard-navigable, reduced-motion respected.

---

## 10. Files

**Add** — `src/tutorial/registry.ts`, `src/tutorial/lessons/{quickstart,module0…module12,capstone}.ts`, `src/tutorial/runtime.ts`, `src/tutorial/completion.ts`, `src/tutorial/diagnostics.ts`, `src/tutorial/tutorialProject.ts`, `src/tutorial/events.ts`, `src/tutorial/regions.ts`, `src/workers/analysisWorker.ts`, `src/tutorial/useTutorial.ts`, `src/tutorial/TutorialOverlay.tsx`, `src/tutorial/CoachPill.tsx`, `src/tutorial/CoverageDashboard.tsx`, `src/tutorial/__tests__/{coverage,completion,rocker,regions,project,diagnostics}.test.ts`, `src/audio/__tests__/{fxMeasure,timelineSegments,multiFader}.test.ts`.

**Change** — `stemTapeV1Map.ts` (full metadata, chop rows, `supersedes`), `surface.ts` (chop remap, `rec.undoPass` emitter, LED explanations), `chordArbiter.ts` (rocker claims Play), `engine.ts` (`LoopWrap`/`DirectionChange` emission, heavy-FX gating, telemetry tap), `fx/rack.ts` + `workletBudget.ts` (read `heavy`), `controlBus.ts` + `faderSessions.ts` (tutorial event emission), `useDeviceSurface.ts` (Shift+R via command path, Escape safety rules), `DeviceSurface.tsx` (highlight layer), `store.ts` + `ProjectDrawer.tsx` (`.stemtape` import/export), `SystemPage.tsx` (coverage sub-tab, registry-derived mapping export), `routes/index.tsx` (Learn entry, overlay mount, registry-derived GUIDE), `keyboardMap.ts` (derive from widened metadata).

**Storage/schema** — new `tutorial` IndexedDB store: `progress` (lesson id → state, timestamps), `session` (active `TutorialSession` for reload recovery), `diagnostics` (capped ring of bug records). Versioned migration, additive only; existing project stores untouched.

---

## 11. Test matrix

| Area | Test |
|---|---|
| Chop remap | Play+rocker changes chop and emits **no** transport command; FN+rocker still scrubs; hold Play alone still cues frame 0; bare rocker still sets rate; verified in tape / FX-overlay / Heads layers |
| v2.6 regression | 37-row suite green; each remap asserted as an extension with `supersedes` |
| Completion | pre-entry state never completes; ordered milestones; ack required; suppression violation fails |
| Multi-fader | 2/3/4 distinct pointer ids in one batchFrame accepted; sequential single-pointer rejected |
| FX | all twelve algorithms produce measurable wet/dry difference offline; Pump produces a tempo-related gain envelope; heavy-FX rejection returns `rejected` and does not mark the bank rejected |
| SOS | timeline emits `RateChange`, `GlideChange`, `LoopWrap`, `LinkChange`, `DirectionChange` during a varispeed overdub; constant-rate manifest rejected |
| Undo | `rec.undoPass` reachable from real UI; removes only the newest pass |
| Scrub / Heads | Playwright: master RMS above floor, all four read pointers move, head scrub changes read position |
| Tutorial Copy | user project byte-identical after restore; mic released; artifacts deleted; reload recovery |
| Keyboard | Shift+R emits `perf.record` through the command path; auto-repeat ignored; editable targets ignored; Escape preserves latches, recording and project |
| Layout | Playwright screenshots at 375/390/420 px, tablet, desktop — no overlay covers a taught control |
| Privacy | network log contains no user audio during a full Quick Start run |
| Regression | full suite green, `tsgo --noEmit` clean, zero console errors |

Real-device iPhone/iPad checks are a separate manual checklist and are labelled as such.

---

## 12. Implementation sequence

1. Fix the eight blockers in §1.3 with tests (undoPass, LoopWrap, DirectionChange, heavy gating, Shift+R, import/export, chop remap + arbitration, highlight layer).
2. Widen registry metadata; coverage test; regenerate GUIDE, keyboard panel, mapping export.
3. Tutorial event stream for continuous controls + telemetry tap.
4. Analysis worker and region selection.
5. Runtime, completion engine, diagnostics, persistence.
6. Overlay, coach pill, highlight rendering, responsive placement.
7. Quick Start ten lessons + Playwright end-to-end.
8. Tutorial Copy snapshot/restore; Modules 8-10.
9. Remaining modules, capstone, coverage dashboard, full acceptance matrix.

---

## 13. VERIFY items (reference-only lessons until proven)

PRINT signal inclusion/exclusion; continuous wind reversal and source lifetime; exact dim/full-light hold duration; reverse-direction recording rejection once `DirectionChange` is emitted; audible Heads scrubbing at browser level; Bluetooth pairing hold timing; iOS delivery limits for large WAV exports. Each gets a targeted read plus an automated or Playwright test in step 1; anything unresolved ships as a read-only reference lesson with no completion gate and an `unverified` badge in the atlas.
