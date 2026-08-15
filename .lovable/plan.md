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

## 6. Guide inventory (public `featureId`s)

Every entry below gets exactly one Guide card. Coverage test is bidirectional: a `featureId` with no reachable implementation fails, and a reachable public feature with no card fails.

**Tape (18):** `tape.transport.toggle`, `tape.transport.cue`, `tape.loop.global.hold`, `tape.loop.global.division`, `tape.loop.global.move`, `tape.loop.global.latch`, `tape.loop.global.release`, `tape.speed.rocker`, `tape.speed.glide`, `tape.speed.semitone`, `tape.speed.snap`, `tape.speed.half`, `tape.scrub.global`, `tape.scrub.speed`, `tape.scrub.latch`, `tape.song.skip`, `tape.track.select`, `tape.master.volume`.
**Lane (7):** `lane.mute`, `lane.audition`, `lane.audition.chord`, `lane.loop.capture`, `lane.loop.resize`, `lane.reverse`, `lane.scrub.fader`, plus `lane.fader.volume`.
**Heads (9):** `heads.enter`, `heads.exit`, `heads.head.gain`, `heads.head.mute`, `heads.head.solo`, `heads.head.latch`, `heads.head.scrub`, `heads.head.loop`, `heads.head.reverse`.
**FX (18):** `fx.overlay.open`, `fx.overlay.close`, `fx.bank.select`, `fx.momentary`, `fx.latch`, `fx.clearLatches`, `fx.algorithm.cycle`, `fx.macro`, plus one card per algorithm: `fx.alg.filter`, `.isolator`, `.dirt`, `.reelFlange`, `.formantShift`, `.gate`, `.echo`, `.pitchEcho`, `.scatter`, `.reverb`, `.shimmer`, `.freeze`.
**Keyboard (1 card + table):** `kbd.map` covering every binding in `keyboardMap.ts`, including simultaneous fader keys.
**Projects (8):** `project.new`, `project.rename`, `project.stem.load`, `project.stem.map`, `project.stem.replace`, `project.trash.restore`, `project.memory.budget`, `project.grid.correct` (new home for manual grid correction).
**Loading / mapping (4):** `load.demo`, `load.bulk`, `load.individual`, `load.format.rules`.
**Session (4):** `session.save`, `session.restore`, `session.export.stemtape`, `session.export.wav`.
**Guide (3):** `guide.navigate`, `guide.animation.replay`, `guide.diagram.legend`.
**System (4):** `system.diagnostics`, `system.hitzones`, `system.commandLog`, `system.privacy`.

**Excluded internal commands (with reasons):** `rollback` (transaction repair, never user-initiated), `emitHold` / mask plumbing (transport of state), `heads.play.hold` ack (engine acknowledgement), `system.noop` (diagnostics band), `commandTail` ordering, `applyHeadsFeedback`, worklet budget messages.

**Hardware-only reference section (clearly labelled, no tutorial affordance):** power 5 s hold, charging, battery display, headphone muting, Bluetooth pairing chord, PO sync out, MIDI clock mode, auto-off.

## 7. Smallest file changes

- `src/input/gestures.ts` — rename `trackDecisionMs` → `tapDecisionMs`, value 200 → 300; expose `decisionLatencyMs` per gesture type.
- `src/machine/chordArbiter.ts` — remove `stem.select`; add global-loop precedence tier above Play-first; add G2 latch resolution on FUNCTION **up**; add FX Track-ownership suppression list.
- `src/machine/surface.ts` — hold-PLAY branch by transport state; global-loop state (division / move / latch / release); FN arm + Track select; delete tempo-tap and FN×4 grid rows; rocker-stopped song skip; delete chop and `play.loopMode`.
- `src/machine/stemTapeV1Map.ts`, `src/machine/v26map.ts` — row add/remove, each deletion carrying `supersedes` + `originalBehaviour`.
- `src/audio/engine.ts` — global one-bar loop reusing `scheduleLoopRelease`; scrub-speed steps; half-speed toggle.
- `src/machine/fx12.ts` — display-alias table only.
- `src/audio/ProjectDrawer.tsx` — manual grid correction control.
- `src/device/ControlsGuide.tsx`, `Sp1GuideIllustration.tsx`, `narrate.ts` — featureId-driven Guide (last).

## 8. Targeted tests

`globalLoop.test.ts` (division, move, latch, PLAY-tap release without stopping, coexistence with a lane loop, hidden-timeline rejoin ≤2 frames), `latchGrammar.test.ts` (the 6-step scrub table plus its non-firing assertions for `rate.set` / transport / `fn.*`), `tapTiming.test.ts` (single/double/triple latency ≤330 ms, no intermediate command), `fnContext.test.ts` (FN arm → Track select; FN + Vol is always division), `songSkip.test.ts`, `fxAlias.test.ts` (alias↔id, legacy `beatRepeat` migration intact), `precedence.test.ts` (per-mode suppression lists), `guideCoverage.test.ts` (bidirectional featureId coverage, hardware-only section excluded from tutorials). Rewrite `chopRemap.test.ts` as a removal test. Keep `loopRejoin`, `busIsolation`, `headsV2`, `laneControls`, `laneScrub`, `globalScrub`. Playwright: audible global loop bar-locked, latch survives PLAY tap, scrub latch/release, FN+Track select, Heads unchanged, Guide at 420 px and desktop with no clipping.

## 9. Implementation order

1. Timing model + gesture grammars (`gestures.ts`, `chordArbiter.ts`) — no audio change, tests green.
2. Map rows + `surface.ts` state, including removals.
3. Audio behaviour (global loop, scrub speed, half-speed) + Projects grid correction.
4. FX alias table.
5. Guide rebuild against the featureId inventory, with the coverage test gating it.
