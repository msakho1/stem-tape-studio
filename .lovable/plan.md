# Stem Tape — Final Control Direction (revised plan, no code)

Provenance: **S1** = TE guide image (stock-guide-verified). **S2** = stock walkthrough video (documentary; physical-verification-required). `v26map.ts` / `stemTapeV1Map.ts` = Tape Looper + Stem Tape contracts, never evidence of stock.

## 1. Gesture grammars (two, mutually exclusive)

**G1 modifier-first** — FUNCTION goes **down before** the target control and is **still down** when the target acts. Arrival budget `modifierArrivalMs = 400` (`chordArbiter.ts:46`). FUNCTION's own tap action is cancelled the instant a target arrives while it is down.

**G2 latch-after** — the operation is **already running** (its control still down), then FUNCTION is **tapped and released** while it runs. FUNCTION down is a no-op on its own; the latch fires on FUNCTION **up** only if an operation is active.

Disambiguation rule: FUNCTION down with **nothing else down** → arm G1 and start a *pending select*; if FUNCTION is released with no target and no active operation, the pending select resolves as "FN armed, awaiting Track 1–4" for `TRACK_SELECT_WINDOW = 1200 ms`. A Track tap inside that window is active-track select; after it, the arm expires.

### Global scrub latch — exact sequence, with proof of no stray command

| # | Event | Guard | Emitted |
|---|---|---|---|
| 1 | `function` down (nothing else down) | arms G1; **no** command | — |
| 2 | `rocker-fwd` down ≤400 ms later | `fn` held → arbiter claims `function` **and** `rocker-fwd` before dispatch | `transport.scrub.start{dir}` |
| 3 | rocker held | scrub integrates | `transport.scrub.rate` |
| 4 | `function` up (rocker still down) | active op present → G2 | `transport.scrub.latch` |
| 5 | `rocker-fwd` up | latched → no end | — (scrub continues) |
| 6 | `rocker-fwd` down again | latch active | `transport.scrub.end` (existing ≤2-frame handoff, `engine.ts` `integrateScrubTo`) |

Non-firing proof obligations (unit-asserted): between #1 and #6 the command stream contains **no** `rate.set` (rocker varispeed is suppressed whenever `fn` is held — `surface.ts:790`), **no** `transport.play` / `transport.stop` / `transport.cue`, **no** `fn.tempoGrid`, **no** `song.next/prev`. Step 4 must not emit a bare FUNCTION action because the arbiter marked `function` claimed at step 2 and claims survive until that control's **next down** (`chordArbiter.ts:200-203`).

The same 6-step shape defines: **global loop latch** (op = hold PLAY loop, FN tap latches, PLAY tap releases), **lane scrub park** (op = FN+fader; release parks — no latch), **FX latch** (op = held bank button, FN tap latches — already implemented, `chordArbiter.ts:210-215, 268-284`).

## 2. Authoritative timing model

One model, no per-family exceptions:

| Constant | Value | Meaning |
|---|---|---|
| `multiTapGapMs` | **300** | max gap between consecutive taps of one multi-tap; opens/extends the group |
| `tapDecisionMs` | **300** | time after the **last** tap-up before the group is dispatched (renames `trackDecisionMs: 200`, `gestures.ts:78`) |
| `holdMs` | 450 | tap → hold |
| `modifierArrivalMs` | 400 | G1 arrival budget |
| `powerHoldMs` / `longHoldMs` | 1200 / 5000 | unchanged |

The 200/300 contradiction is resolved by making the deferred-arbitration window **equal** to the multi-tap gap (300 ms): a 200 ms commit could dispatch a single tap while a legal second tap was still inside its 300 ms gap. Measured worst-case action latency after the final finger-up: single **300 ms**, double **300 ms**, triple **300 ms** (total gesture wall time: single 300, double ≤600+, triple ≤900+). Test asserts `decisionLatencyMs` (`gestures.ts:163`) ≤330 ms for all three and that no intermediate mute/loop command precedes a double or triple.

## 3. Revised final mapping

| Gesture | Grammar | Current (file:line) | Final | Class |
|---|---|---|---|---|
| Hold PLAY (playing) | bare | `transport.cue` frame 0 unconditional — `surface.ts:745-752` | **global one-bar loop** while held | stock (S1 LOOP) |
| Hold PLAY (stopped) | bare | same | cue to frame 0 | extension |
| FN + Vol −/+ | G1 | `volume.chopWindow` glide — `surface.ts:797` | **global-loop division** (only job; sets the pending division when no loop runs) | S2 |
| PLAY + Vol −/+ | — | `stem.select` — `chordArbiter.ts:335-342` | **removed** | obsolete |
| PLAY held + rocker | — | `rocker.chop.play` — `surface.ts:656-667, 790` | **move the global loop** ±1 division; chop removed | S2 |
| FN tap while PLAY held | G2 | none | latch global loop | S2 |
| PLAY tap while loop latched | bare | toggles transport — `surface.ts:485-487` | release latch, transport keeps playing; **transport toggle suppressed** | S2 |
| Short FN → Track 1–4 | G1 arm | FN tap = tempo tap — `surface.ts:492-518` | **active-track select** | stock (S2) |
| FN ×4 grid rounding | — | `surface.ts:828-836` | **removed**; manual grid correction moves to Projects UI | obsolete |
| Track tap | bare | mute toggle | mute toggle, or release that lane's loop if one is running | extension |
| Track hold (1–3 together) | bare | momentary audition chord — `surface.ts:756-782` | unchanged | stock solo + extension |
| Track double-tap | bare | one-bar lane loop — `stemTapeV1Map.ts:223` | unchanged | extension |
| FN + Track double-tap | G1 | lane reverse | unchanged | extension |
| FN + Track + Vol ± | G1 | lane loop resize — `surface.ts:684-700` | unchanged (Track held disambiguates from global division) | extension |
| FN + Fader n | G1 | lane scrub/park — `scrub.ts`, `engine.ts` laneFaderScrub | unchanged | extension |
| FN + Rocker | G1 + G2 | global scrub — `stemTapeV1Map.ts:139-141` | + Vol ± sets scrub speed, FN tap latches, rocker re-press releases | extension |
| Rocker, stopped | bare | varispeed | **prev/next song** | stock (S1) |
| Rocker, playing | bare | varispeed / glide / semitone | unchanged | S2 |
| FN + PLAY ×1 / ×2 / ×3 | G1 | ×2 snap 1×, ×3 Heads — `surface.ts:468-484` | ×1 half-speed toggle, ×2 snap 1×, ×3 Heads | ×1 S2, rest extension |
| Vol− + Vol+ short | bare | FX overlay | unchanged | extension |
| Vol− + Vol+ ≥2 s | bare | `system.pairing` | **hardware-only**, non-functional note | hardware |

**Global-loop release is physical-verification-required.** S1/S2 show only that hold-PLAY loops and release ends it; neither establishes whether stock rejoins a hidden timeline or resumes from the loop start. Plan of record: implement release using Stem Tape's existing hidden-timeline rejoin (`engine.ts` `scheduleLoopRelease`, proven in `loopRejoin.test.ts`) and label the row **PVR — Stem Tape choice, not stock-verified** in the map and Guide.

## 4. Precedence matrix

Highest first, per mode.

**TAPE**
1. cancel / lost pointer.
2. Vol−+Vol+ ≥2 s (hardware note).
3. Global loop (PLAY held or latched): owns Vol ± (division), rocker (move), FN tap (latch); **suppresses** `transport.play/stop/cue`, `rate.set`, `stem.select`, `loop.chop`, song skip.
4. Global scrub (FN + rocker, held or latched): **suppresses** `rate.set`, song skip, `transport.play/stop`.
5. Lane operations (FN + fader scrub, FN + Track dbl reverse, FN + Track + Vol resize).
6. Track deferred group (tap / double / hold).
7. Bare transport, rocker varispeed or song skip, master volume.

Global loop vs existing stem loops: they **coexist**. Global loop bounds the shared transport; lane loops keep their own bar windows inside it and are released independently by their Track tap. A global loop release re-anchors lanes at the hidden-timeline frame; a lane loop release does not disturb the global loop.

**FX OVERLAY**
1. cancel. 2. Vol−+Vol+ (closes overlay). 3. FN + bank button = latch (`chordArbiter.ts:210-215`). 4. all four + FN = clear latches. 5. bare bank button = select + momentary on pointer-down. 6. Vol ± with a bank selected = macro (hold ≥450 ms) or algorithm cycle (tap).
Track buttons are **owned by FX** here: mute, lane loop, audition and reverse are all **suppressed**. FN + Vol does **not** set loop division in FX. Transport, global loop and global scrub remain reachable (PLAY and rocker are not claimed by FX).

**HEADS**
1. cancel. 2. FN + PLAY ×3 = exit. 3. Track n = head n (tap mute, hold momentary solo, triple-tap latch playback, double-tap capture one-bar head loop, FN + double-tap head reverse, FN + Track + Vol resize head loop). 4. Fader n = head n gain; FN + fader n = head n scrub/park. 5. bare transport still drives the hidden song clock.
Suppressed in Heads: stem mute/solo/lane-loop/lane-reverse on the normal bus, global one-bar loop, `stem.select`, FX overlay entry. The normal four-stem bus is gated to exactly 0 (`engine.ts` `crossfadeBuses`, `busIsolation.test.ts`) while its stems advance silently. Exit crossfades back over 40 ms and restores the `headsEntrySnapshot` bit-identically; the transport rejoins at the hidden-timeline frame, not at any head position.

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

- `src/input/gestures.ts` — `trackDecisionMs` stays 200 and becomes the single shared Track gap+decision constant; add `fnPlayDecisionMs = 420`; leave every other path immediate.
- `src/machine/chordArbiter.ts` — retire `stem.select`; retain PLAY+Track solo/link at its new precedence; global-loop tier; release-order scrub latch on FUNCTION up while rocker down; FX Track-ownership list that explicitly **excludes** FN-qualified lane gestures and FN+Volume.
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
- `tapTiming.test.ts` — Track single/double/triple commit ≤220 ms; FN+PLAY ×3 ≤440 ms; bare PLAY / Volume / rocker / fader / FX momentary commit ≤1 frame.
- `fnContext.test.ts` — FN+Volume is division in Tape **and** in FX; FN-arm → Track select; LED arm-pulse → solid.
- `fxLaneAccess.test.ts` — FN+fader, FN+Track double-tap, FN+Track+Volume all reachable in FX; bare Track is FX-owned.
- `headsBehaviour.test.ts` — tap releases head loop else mutes; audition of any 1–4 group while paused and restore; triple-tap latch; FX audible in Heads; discard-on-exit with lanes rejoining the advancing hidden timeline.
- `playTrackSolo.test.ts` — PLAY+Track solo/link still fires and never collides with the global loop.
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
