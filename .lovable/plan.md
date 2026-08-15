# Stem Tape — Final Control Direction (revised plan, no code)

Provenance: **S1** = TE guide image (stock-guide-verified). **S2** = stock walkthrough video (documentary; physical-verification-required). `v26map.ts` / `stemTapeV1Map.ts` = Tape Looper + Stem Tape contracts, never evidence of stock.

## 1. Gesture grammars

**G1 modifier-first** — FUNCTION goes down **before** the target and is still down when the target acts. Arrival budget `modifierArrivalMs = 400` (`chordArbiter.ts:46`). FUNCTION's own tap action is cancelled the moment a target arrives while it is down.

**G2 release-order latching** — no separate "tap FUNCTION" grammar. A G1 operation still running when both controls come up is resolved by **which control is released first**.

### Global scrub — exact down/up sequence

| # | Event | Guard | Emitted |
|---|---|---|---|
| 1 | `function` down, nothing else down | arms G1 | — (no `fn.*`) |
| 2 | `rocker-fwd` down ≤400 ms | arbiter claims `function` **and** the rocker before dispatch | `transport.scrub.start{dir}` |
| 3 | rocker held | integrates | `transport.scrub.rate` |
| 4a | **rocker up first** (FUNCTION still down) | normal end | `transport.scrub.end` (≤2-frame handoff, `engine.ts` `integrateScrubTo`) |
| 4b | **`function` up first**, rocker still down | latch | `transport.scrub.latch` |
| 5 | rocker up after 4b | latched | — |
| 6 | rocker re-press | latch active | `transport.scrub.end` |

Non-firing obligations, asserted across the whole stream from #1 to #6: no `rate.set` (rocker varispeed is suppressed while `fn` is held, `surface.ts:790`), no `transport.play/stop/cue`, no `fn.tempoGrid` or any bare FUNCTION action (the claim set at #2 survives until that control's next **down**, `chordArbiter.ts:200-203`), no `song.next/prev`.

The same release-order rule defines the **global-loop latch**: PLAY held with the loop running, FUNCTION down/up → latch; PLAY released first → normal loop end; PLAY tap while latched → release latch, transport keeps playing. **FX latch** already works this way (`chordArbiter.ts:210-215, 268-284`). **FN + fader** has no latch: release parks.

**Active-track selection, with visible behaviour:** FUNCTION tap with nothing else down **arms** selection for `TRACK_SELECT_WINDOW = 1200 ms`; **all four** top LEDs pulse while armed. A Track tap inside the window selects that track: its LED goes **solid** and stays solid, the other three **resume live meter activity**, the arm expires, and that track becomes the **Tape and FX target** (`activeStem`). FUNCTION tap while an operation is running is a latch, never a select.

## 2. Timing model

Latency is per family — not a universal 300 ms.

| Family | Constant | Value | Commit latency after last finger-up |
|---|---|---|---|
| Track buttons (tap / double / triple / hold) | `trackDecisionMs` — one shared gap **and** decision window | **200 ms** (tunable 200–220) | ≤220 ms for all counts |
| FN + PLAY multi-tap | `fnPlayDecisionMs` | **300 ms** | ≤330 ms (single-tap half-speed must not feel sluggish) |
| Bare PLAY, Volume, rocker, faders, FX momentary | none | **no intentional arbitration timer** — the command is emitted **synchronously in the same input-event turn** (FX momentary on pointer-**down**, `chordArbiter.ts:217-226`) | engine/audio acknowledgement is measured **separately**, never conflated with input latency |
| tap → hold | `holdMs` | 450 ms | — |
| G1 arrival | `modifierArrivalMs` | 400 ms | — |

The 200/300 conflict is resolved by deleting the separate `multiTapGapMs` for Track buttons: one constant is both the inter-tap gap and the decision delay, so a legal second tap can never arrive after dispatch. `multiTapGapMs = 300` survives only for controls with no deferred arbitration.

## 3. Revised final mapping

| Gesture | Grammar | Current (file:line) | Final | Class |
|---|---|---|---|---|
| Hold PLAY (playing) | bare | `transport.cue` frame 0 unconditional — `surface.ts:745-752` | **global one-bar loop** while held | stock (S1 LOOP) |
| Hold PLAY (stopped) | bare | same | cue to frame 0 | extension |
| FN + Vol −/+ (no Track held) | G1 | `volume.chopWindow` — `surface.ts:797` | **global-loop division** — its only job, in **every** mode including FX; sets the pending division when no loop runs | S2 |
| PLAY + Vol −/+ | — | `stem.select` — `chordArbiter.ts:335-342` | **retired** with `supersedes: ["stem.select"]` + `originalBehaviour`; superseded by FN-arm + Track select | obsolete |
| **PLAY + Track (solo, <700 ms; link/unlink, ≥700 ms)** | chord | `stem.solo` / `stem.link` — `chordArbiter.ts:344-353` | **RETIRED — structurally unreachable.** The global loop begins at `holdMs = 450`, so a ≥700 ms link overlap can never complete and a solo can be cancelled mid-gesture. Track hold already gives 1–4-stem momentary/group solo. Remove the **hardware mappings** only, each with `supersedes` + `originalBehaviour`; the underlying `stem.solo` / `stem.link` engine commands and any saved per-stem solo/link state stay intact and loadable | obsolete |
| PLAY held + rocker | — | `rocker.chop.play` — `surface.ts:656-667, 790` | **move the global loop** ±1 division; chop retired with `supersedes` | S2 |
| Short FN → Track 1–4 | G1 arm | FN tap = tempo tap — `surface.ts:492-518` | **active-track select** with LED arm/solid | stock (S2) |
| FN ×4 grid rounding | — | `surface.ts:828-836` | **removed**; manual correction moves to Projects | obsolete |
| Track tap (Tape) | bare | mute toggle | releases that lane's loop if one is active, otherwise mutes | extension |
| Track hold, any 1–4 group | bare | audition chord — `surface.ts:756-782` | unchanged | stock solo + extension |
| Track double · FN+Track double · FN+Track+Vol · FN+fader | bare / G1 | lane loop, reverse, resize, scrub | unchanged, and **reachable inside FX Overlay** | extension |
| FN + Rocker | G1 + release-order | global scrub | + Vol ± sets scrub speed; latch per §1 | extension |
| Rocker, stopped / playing | bare | varispeed always | **prev/next song** when stopped; varispeed unchanged when playing | stock (S1) / S2 |
| FN + PLAY ×1 / ×2 / ×3 | G1 | ×2 snap, ×3 Heads — `surface.ts:468-484` | ×1 half-speed, ×2 snap 1×, ×3 Heads | ×1 S2, rest extension |
| Vol− + Vol+ short / ≥2 s | bare | FX overlay / `system.pairing` | overlay unchanged; pairing **hardware-only**, non-interactive | extension / hardware |

**Global-loop release is physical-verification-required.** S1/S2 establish only that hold-PLAY loops and that release ends it. Plan of record: reuse Stem Tape's hidden-timeline rejoin (`engine.ts` `scheduleLoopRelease`, `loopRejoin.test.ts`), flagged **PVR — Stem Tape choice, not stock-verified** in the map and the Guide card.

## 4. Precedence and coexistence

**TAPE**, highest first: 1 cancel/lost pointer · 2 Vol−+Vol+ ≥2 s (hardware note) · 3 global loop (PLAY held or latched) · 4 global scrub · 5 FN-qualified lane ops · 6 Track deferred group · 7 bare transport, varispeed or song skip, master volume. **Hold PLAY is exclusively responsible for the global loop**; no PLAY+Track chord tier exists.
Suppressed by 3: `transport.play/stop/cue`, `rate.set`, `loop.chop`, `song.*`. Suppressed by 4: `rate.set`, `song.*`, `transport.play/stop`. Retired everywhere: `stem.solo` / `stem.link` as **gestures**.

**FX OVERLAY**: 1 cancel · 2 Vol−+Vol+ closes · 3 FN + bank = latch, all four + FN = clear latches · 4 FN-qualified **lane** gestures — FN+fader scrub, FN+Track double-tap reverse, FN+Track+Vol resize — all remain reachable · 5 FN+Vol with **no** Track held = global-loop division, never disabled here · 6 bare Track = bank select + momentary on pointer-down (**FX owns bare Track**: mute, lane loop and audition suppressed) · 7 Vol ± with a bank selected = macro (hold) or algorithm cycle (tap). Transport, global loop and global scrub stay reachable.

**HEADS**: 1 cancel · 2 FN+PLAY ×3 exit · 3 Track n = head n — tap **releases that head's loop if one is active, otherwise mutes**; holding **any 1–4 group** auditions exactly those heads **including while the transport is paused**, and release restores the prior state exactly; triple-tap latches independent head playback; double-tap captures a one-bar head loop; FN + double-tap reverses that head; FN + Track + Vol resizes its loop · 4 fader n = head n gain, FN + fader n = head n scrub/park · 5 **FX processes the entire Heads bus** — the rack is inserted on the heads bus as a whole and does **not** target the hidden normal-bus `activeStem`; overlay, banks, latches and macros all function and are audible · 6 bare transport still advances the hidden song clock.
Suppressed in Heads: normal-bus stem mute/solo/lane-loop/lane-reverse, the global one-bar loop, `stem.select`. The normal bus is gated to exactly 0 (`engine.ts` `crossfadeBuses`, `busIsolation.test.ts`) while its stems advance silently. On exit every heads-only loop, reverse, latch and mute is **discarded** and the `headsEntrySnapshot` is restored over a 40 ms crossfade: **mixer and control state — mute, solo, gains, direction, loops and FX state — restores exactly**; **transport position is excluded from the snapshot comparison**, because the normal stems rejoin the **current advancing hidden-timeline frame**, never a head position.

### Global-loop × lane-loop transition table

"Audible pointer" = the frame the listener hears; "hidden target" = the frame that lane's silent song clock holds for rejoin. All boundaries are the next shared bar; all landings ≤2 frames.

| Case | Audible pointer | Hidden target |
|---|---|---|
| **A. Lane loop already active when the global loop starts** | that lane keeps cycling its own captured bar, unrestarted and unrecaptured; the other three cycle the global bar. Both cycles are phase-locked to the same detected grid | every lane's hidden clock keeps advancing at song rate, unaffected by either loop |
| **B. Lane loop captured during a global loop** | the capture window is the bar the **global loop** is currently sounding, starting at its bar-start frame; that lane then leaves the global cycle and repeats the captured bar | that lane's hidden clock continues advancing from the frame it held at capture |
| **C. Lane loop released during a global loop** | at the next bar boundary that lane resumes at the **global loop's current bar-start frame**, not at its hidden frame, so it re-locks with the other three | the hidden clock keeps advancing and stays unused until the global loop ends |
| **D. Global loop released while lane loops remain active** | non-looping lanes resume at their **hidden-timeline frame** on the next bar boundary (`scheduleLoopRelease`; PVR flag applies). Lanes still holding a loop keep cycling untouched and each rejoins its own hidden frame only when its loop is later released | every lane's hidden clock has advanced throughout and is the authoritative rejoin frame |

## 5. FX: what actually changes

Current twelve (`fx12.ts:68-112`): TONE — Filter, Isolator, Dirt/Crusher; MOD — Reel Flange, Formant Shift, Rhythmic Gate; MOTION — Tempo Echo, Pitch Echo, Granular Scatter; SPACE — Reverb, Shimmer, Spectral Freeze.

Evidence check: **Beat Repeat and Pump are already removed and already replaced** by the two approved replacements — Reel Flange and Formant Shift (Rhythmic Gate is the third MOD slot). Proof: `fxMod.test.ts:18-19` asserts neither `beatRepeat` nor `pump` exists as an `AlgorithmId`; `fx12.ts:247` keeps `beatRepeat` only as a legacy **migration key** mapping old saved state to `{bank:1, algorithm:2}`. `public/beat-repeat-processor.js` is no longer referenced.

Therefore the remaining FX work is genuinely **not DSP**: it is (a) exposing stock-recognisable names in the Guide and overlay copy — Filter (TONE 1), Delay/Echo (MOTION 1), Distortion → the existing Dirt/Crusher, Gate/Shutter → the existing Rhythmic Gate — as **display aliases only**, leaving `AlgorithmId` untouched so saved projects keep loading; (b) no map change beyond the alias table; (c) tests asserting alias↔id stability and that the legacy migration key still resolves. **No algorithm is invented, removed or renamed at the id level.** If you intended a *thirteenth/fourteenth* algorithm beyond this set, that is an **unresolved decision** — nothing in project history names one.

## 6. Guide inventory (public `featureId`s) — derived counts, total **80**

Every entry is one card carrying all seven fields: purpose · exact down/up gesture order · visual result (LED / overlay / readout) · audible result · restrictions (mode, suppression, PVR flags) · keyboard equivalent (or "none — pointer only") · animated faithful `stem-tape-sp1-outline.svg` illustration. A generic rectangle fails review. Coverage is bidirectional: a card without a reachable feature fails, and a reachable public feature without a card fails.

**Tape — 21:** `tape.transport.toggle`, `tape.transport.cue`, `tape.loop.global.hold`, `.division`, `.move`, `.latch`, `.release`, `tape.loop.interaction` (global × lane transition table), `tape.speed.rocker`, `.glide`, `.semitone`, `.snap`, `.half`, `tape.scrub.global`, `.speed`, `.latch`, `tape.song.skip`, `tape.track.select`, `tape.track.select.leds`, `tape.master.volume`, `tape.grid.auto` (automatic BPM/beat-phase/bar detection; local, deterministic, non-AI).
**Lane — 8:** `lane.mute`, `lane.audition`, `lane.audition.chord`, `lane.loop.capture`, `lane.loop.resize`, `lane.reverse`, `lane.scrub.fader`, `lane.fader.volume`.
**Heads — 9:** `heads.enter`, `heads.exit`, `heads.head.gain`, `heads.head.mute`, `heads.head.solo`, `heads.head.latch`, `heads.head.scrub`, `heads.head.loop`, `heads.head.reverse`.
**FX — 21:** `fx.overlay.open`, `fx.overlay.close`, `fx.bank.select`, `fx.momentary`, `fx.latch`, `fx.clearLatches`, `fx.algorithm.cycle`, `fx.macro`, `fx.signalOrder` (TONE → MOD → MOTION → SPACE, `fx12.ts:1-16`), plus twelve algorithm cards `fx.alg.filter`, `.isolator`, `.dirt`, `.reelFlange`, `.formantShift`, `.gate`, `.echo`, `.pitchEcho`, `.scatter`, `.reverb`, `.shimmer`, `.freeze`.
**Keyboard — 1:** `kbd.map`, the full table from `keyboardMap.ts`, explicitly preserving Y/H, U/J, I/K, O/L as faders 1–4 (simultaneous).
**Projects — 7:** `project.new`, `project.rename`, `project.stem.load`, `project.stem.map`, `project.stem.replace`, `project.trash.restore`, `project.grid.correct` (new manual-correction home). Memory budget is a **readout**, not a lesson — removed.
**Loading / mapping — 4:** `load.demo`, `load.bulk`, `load.individual`, `load.format.rules`.
**Session — 2:** `session.save`, `session.restore`. `.stemtape` and performance-WAV export have **no reachable UI** (`src/audio/export/*` and `src/workers/wavWorker.ts` are unreferenced by any component) — excluded until wired.
**Guide — 3:** `guide.navigate`, `guide.animation.replay`, `guide.diagram.legend`.
**System — 4:** `system.diagnostics`, `system.hitzones`, `system.commandLog`, `system.mapExport` (reachable at `DiagnosticPanel.tsx:218-227`). `system.privacy` removed.

**Excluded internal commands (reasons):** `rollback` (transaction repair, never user-initiated), `emitHold` / mask plumbing (state transport), `heads.play.hold` ack and `applyHeadsFeedback` (engine acknowledgement), `system.noop` (diagnostics band), `commandTail` ordering, worklet budget messages.

**Hardware-only reference block** (labelled, non-interactive, no animation, no keyboard row): power 5 s hold, charging, battery display, headphone muting, Bluetooth pairing chord, PO sync out, MIDI clock mode, auto-off.

## 7. Smallest file changes

- `src/input/gestures.ts` — `trackDecisionMs` stays 200 as the single shared Track gap+decision constant; add `fnPlayDecisionMs = 300`; every other path keeps **no arbitration timer** and emits in the same event turn.
- `src/machine/chordArbiter.ts` — retire the `stem.select`, `stem.solo` and `stem.link` **gesture tiers** (`chordArbiter.ts:332-357`), keeping the `PerfIntent` types and reducer handlers so saved solo/link state still loads; global-loop tier; release-order scrub latch on FUNCTION up while rocker down; FX Track-ownership list that explicitly **excludes** FN-qualified lane gestures and FN+Volume.
- `src/machine/surface.ts` — hold-PLAY by transport state; global-loop state + division/move/latch/release with the transition table; FN arm + LED pulse + active-track select; delete tempo-tap, FN×4, chop, `play.loopMode`; rocker-stopped song skip; Heads Track tap loop-release-else-mute; paused-transport head audition.
- `src/machine/stemTapeV1Map.ts`, `src/machine/v26map.ts` — row edits; every deletion carries `supersedes` + `originalBehaviour`.
- `src/audio/engine.ts` — global loop via `scheduleLoopRelease`; per-case audible-pointer / hidden-rejoin handling; scrub-speed steps; half-speed; keep FX rack live on the heads bus.
- `src/audio/headLanes.ts` — head loop release, paused-transport audition, discard-on-exit.
- `src/machine/fx12.ts` — display-alias table only.
- `src/audio/ProjectDrawer.tsx` — manual grid correction.
- `src/device/keyboardMap.ts`, `src/device/useDeviceSurface.ts`, `src/device/KeyboardPanel.tsx` — bindings for every changed gesture (global loop division/move/latch, scrub latch, FN-arm select, song skip, half-speed), dismissible desktop panel updated; Y/H, U/J, I/K, O/L untouched.
- `src/device/ControlsGuide.tsx`, `Sp1GuideIllustration.tsx`, `narrate.ts` — featureId-driven Guide with the seven-field card schema (last).

## 8. Targeted tests

- `globalLoop.test.ts` — division, move, latch, PLAY-tap release without stopping.
- `loopCoexistence.test.ts` — all four transition-table rows; asserts the audible pointer and the hidden rejoin frame per row, ≤2 frames.
- `latchOrder.test.ts` — the six-step release-order scrub table plus non-firing assertions (`rate.set`, `transport.*`, `fn.*`, `song.*`).
- `tapTiming.test.ts` — Track single/double/triple commit ≤220 ms; FN+PLAY ×1/×2/×3 commit ≤330 ms; bare PLAY / Volume / rocker / fader / FX momentary assert **no arbitration timer** and synchronous emission inside the same input-event turn (engine acknowledgement measured in a separate audio assertion).
- `fnContext.test.ts` — FN+Volume is division in Tape **and** in FX; FN-arm → Track select; all four LEDs pulse while armed, selected LED solid, other three back to live meters, `activeStem` becomes the Tape/FX target.
- `fxLaneAccess.test.ts` — FN+fader, FN+Track double-tap, FN+Track+Volume all reachable in FX; bare Track is FX-owned.
- `headsBehaviour.test.ts` — tap releases head loop else mutes; audition of any 1–4 group while paused and restore; triple-tap latch; **FX processes the whole heads bus, not `activeStem`**; discard-on-exit restores mute/solo/gains/direction/loops/FX exactly, **excludes transport position** from the comparison, and lanes rejoin the advancing hidden-timeline frame.
- **Delete `playTrackSolo.test.ts`.** Replace with `playTrackRetired.test.ts` — PLAY+Track emits **no** `stem.solo` / `stem.link` command at any overlap (0–1500 ms), Hold PLAY remains the sole producer of the global loop, and the retired rows carry `supersedes` + `originalBehaviour` while saved solo/link state still deserialises.
- `precedence.test.ts` — per-mode suppression lists.
- `fxAlias.test.ts` — alias↔id stability, legacy `beatRepeat` migration intact.
- `guideCoverage.test.ts` — bidirectional featureId coverage, seven required fields per card, hardware block excluded from tutorials, counts 21/8/9/21/1/7/4/2/3/4.
- `keyboardParity.test.ts` — every changed gesture has a binding; Y/H, U/J, I/K, O/L unchanged and simultaneous.
- Rewrite `chopRemap.test.ts` as a removal test; keep `loopRejoin`, `busIsolation`, `headsV2`, `laneControls`, `laneScrub`, `globalScrub`.
- Playwright: audible bar-locked global loop, latch survives PLAY tap, scrub latch/release, FN+Track select LEDs, FX in Heads, Guide at 420 px and desktop without clipping.

## 9. Implementation order

1. Timings + grammars + release-order latching (`gestures.ts`, `chordArbiter.ts`).
2. Map rows and `surface.ts` state, including removals and PLAY+Track retention.
3. Audio: global loop and coexistence table, scrub speed, half-speed, Heads corrections, FX-in-Heads.
4. Keyboard map, `useDeviceSurface.ts`, Keyboard Controls panel.
5. Projects grid correction + FX alias table.
6. Guide rebuild gated by the coverage test.
