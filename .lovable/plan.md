# Stem Tape — Final Control Direction (plan only)

Firmware work is paused. Provenance: **S1** = TE guide image (`user-uploads://IMG_4259.jpeg`) — stock-guide-verified. **S2** = stock walkthrough `https://youtu.be/zynYy35AdE0`, presenter reading TE's internal manual on an unmodified unit — documentary, physical-verification-required. `v26map.ts` is a Tape Looper contract, **not** stock; nothing below is inferred from it.

## 1. Current vs final mapping (verified in source)

| Gesture | Current behaviour (file:line) | Final | Class |
|---|---|---|---|
| Hold PLAY | `transport.cue` frame 0, unconditional — `surface.ts:745-752` | playing → **global one-bar loop**; stopped → cue 0 | stock (S1 "LOOP", S2 @34:24) |
| Vol −/+ while holding PLAY | Play+Vol = `stem.select` — `chordArbiter.ts:335-342` | **loop division** while loop held | S2 @47:15 |
| Rocker while holding PLAY | `rocker.chop.play` half/double + glide — `surface.ts:656-667, 790` | **move the loop** | S2 @48:19; chop **removed** |
| FN tap while holding PLAY | none | **latch loop** | stock grammar, S2 @49:08 |
| Release PLAY | ends nothing (cue already fired) | ends unlatched loop | S2 @47:15 |
| Tap PLAY with loop latched | toggles transport — `surface.ts:485-487` | release latch, transport continues | S2 @49:08 |
| FN + Vol −/+ (not looping) | `volume.chopWindow` glide — `surface.ts:797` | **preset next loop division** | S2 @47:47; chop removed |
| Track tap | mute/unmute — `surface.ts:440+` | mute/unmute **or release that stem's loop** | extension |
| Track hold (1–n) | momentary audition chord, restores mix — `surface.ts:756-782` | unchanged | stock solo (S1) + extension grouping |
| Track double-tap | one-bar lane loop — `stemTapeV1Map.ts:223` | unchanged; rejoin at next bar | extension |
| FN + Track + Vol | lane loop resize — `stemTapeV1Map.ts:260`, `surface.ts:684-700` | unchanged | extension |
| FN + Fader | audible lane scrub, stored volume untouched — `scrub.ts:37`, `engine.ts` laneFaderScrub | unchanged | extension |
| FN + Track double-tap | lane reverse — `stemTapeV1Map.ts:246-255` | unchanged | extension |
| FN + Rocker | global scrub — `stemTapeV1Map.ts:139-141` | + **Vol selects scrub speed**, FN tap latches, rocker again releases | extension (stock antecedent: S1 "TAPE EFFECT") |
| FN + PLAY ×1 / ×2 / ×3 | ×2 snap 1×, ×3 heads — `surface.ts:468-484`; ×1 unused | ×1 **slow playback** (S2 @34:57), ×2 snap, ×3 Heads | ×1 stock, ×2/×3 extension |
| Rocker stopped | varispeed always | **prev/next song** | stock (S1 SKIP TRACK) |
| Rocker playing | varispeed/glide/semitone | unchanged | S2 @50:34 |
| Vol− + Vol+ short | FX overlay — `chordArbiter.ts:319-320` | unchanged | extension |
| Vol− + Vol+ long | `system.pairing` — `chordArbiter.ts:317` | **hardware-only**; web shows a non-functional note | hardware-only |
| Short FN → Track | FN tap = tempo tap — `surface.ts:492-518` | **active-track select** (S2 @39:18) | stock |
| FN + Vol (alternative) | — | active-track select ±1 | stock |
| `play.loopMode` fixed/variable | `surface.ts:856-860`, `v26map.ts:53` | **removed**, replaced by global + per-stem loops | obsolete |
| Recording / track delete / PRINT | already removed | stay removed | — |

FX: keep 12 algorithms, but TONE must expose an identifiable **Filter**, MOD a **Delay/Echo**, and add **Distortion** and **Gate/Shutter** names (`fx12.ts:68-112`, `fx/banks.ts`). Momentary, latch, per-stem unlatch and clear-all stay (`chordArbiter.ts:206-284, 361-388`). Heads keeps every current feature (`headLanes.ts`, `busIsolation.test.ts`).

## 2. Conflicts, order rules, timings, suppression

1. **FUNCTION is overloaded.** It currently means tempo-tap (`surface.ts:492-518`) and ×4 grid rounding (`surface.ts:828-836`). Stock needs FN tap for active-track select and for latching. **Resolution:** FN tap is contextual by *what is already held* — PLAY held → latch loop; rocker deflected → latch scrub; track held in FX → latch FX; **nothing held → active-track select**. Tempo tap moves off FN entirely (grid is automatic, `gridAnalysis.ts`); FN×4 grid correction is retired. Report as the single riskiest change.
2. **Control order is explicit:** the qualifier must be down **before** the acted control, within `modifierArrivalMs = 400` (`chordArbiter.ts:46`); latching FN must arrive **while the acted control is still down**.
3. **Hold PLAY branch** needs the transport state read at `holdStart` (hold level ≈ 450 ms, `stemTapeV1Map.ts:120`) — playing → loop, stopped → cue.
4. **Suppression:** while the global loop is held, PLAY's tap transaction, `stem.select`, varispeed and chop are all suppressed; while FN + rocker scrub is active, varispeed and song-skip are suppressed; Track double-tap suppresses the intermediate mute (deferred arbitration 200 ms, `gestures.ts`).
5. **Play + Vol = stem.select** conflicts with loop division → division wins while looping; stem.select is retired in favour of FN + Vol.
6. Multi-tap windows unchanged: 300 ms tap grouping, 450/600/700/900 ms thresholds (`stemTapeV1Map.ts`).

## 3. Smallest file changes

- `src/machine/surface.ts` — hold-PLAY branch, global-loop state + division/move/latch, FN-tap context switch, rocker-while-stopped song skip, delete chop + `play.loopMode`.
- `src/machine/chordArbiter.ts` — new PLAY-loop precedence tier above Play-first chords; FN-latch claim rules; retire `stem.select`.
- `src/machine/stemTapeV1Map.ts`, `src/machine/v26map.ts` — row add/remove with `supersedes` + `originalBehaviour` for every deletion.
- `src/audio/engine.ts` — global one-bar loop (reuse `scheduleLoopRelease` / hidden timeline), scrub-speed steps, slow-playback rate.
- `src/audio/fx/banks.ts`, `src/machine/fx12.ts` — algorithm naming only.
- `src/device/ControlsGuide.tsx`, `Sp1GuideIllustration.tsx`, `narrate.ts` — Guide rebuild (last).

## 4. Test plan

Unit: `globalLoop.test.ts` (division, move, latch, release-without-stop), `fnContext.test.ts` (FN tap resolves by held control), `songSkip.test.ts`, plus existing `loopRejoin`, `busIsolation`, `headsV2`, `laneControls`, `chopRemap` (rewritten as a removal test). Browser (Playwright): hold-PLAY loop audible and bar-locked; latch survives PLAY tap; FN+rocker scrub speed steps; FN tap selects active track; Heads unchanged; mobile + desktop Guide layout, diagrams unclipped. Guide coverage test asserts a 1:1 map between reachable commands/shortcuts and Guide entries, failing on orphans both ways.

## 5. Order

1. Arbitration + map rows (no audio change). 2. Audio behaviour. 3. Guide rebuild to the stated per-entry schema, accordion sections as specified, faithful outline SVG, hit-zone overlay kept separate.

## Uncertainty

Loop length/division set, exact release semantics, FX routing (per-stem vs active-stem) and simultaneous-hold behaviour are **physical-verification-required** — S2 only. Bluetooth long-chord is hardware-only.
