# Guide Audit + Onboarding Wizard

## Part 1 — Guide v1.0 vs codebase (audit result)

Verified against `src/machine/*`, `src/audio/*`, `src/device/useDeviceSurface.ts`, `public/*-processor.js`, tests.

### Accurate (no change needed)
Transport phases and hold-to-cue frame zero (`surface.ts:370-374`, `564-573`); Play+Volume stem select and Play+Track solo/link at 700 ms (`chordArbiter.ts:312-337`, `47`); rocker ±1 BPM / glide / exact semitone (`surface.ts:528-537`, `628-631`); Function+Faders window + LP/dry/HP (`surface.ts:799-814`); Function+Volume chop slide/glide (`surface.ts:542-546`, `632-634`); Function+Play release/×2/×3/long (`surface.ts:351-369`, `662-669`); grid learning from Function ×4 and clear/round (`surface.ts:378-412`, `646-660`); FX overlay chord and bank buttons Track1 TONE / Track2 MOTION / Track3 SPACE / Track4 RHYTHM (`chordArbiter.ts:296-300`, `fx12.ts:68-109`) — the internal signal order TONE→RHYTHM→MOTION→SPACE is separate from button order and is documented in code; all twelve algorithm names, wrap-around cycling, per-algorithm macros, momentary/latch/clear-latches (`fx12.ts:73-108`, `197-212`, `chordArbiter.ts:206-258`); Heads offsets 0/25/50/75 %, head level/scrub/mute/reverse/source/PRINT (`surface.ts:356-362`, `423-433`, `580-590`, `779-797`); punch late window `min(120 ms, 1/8 beat)` (`grid.ts:124`, tested `phase6.test.ts:34-45`); 16 song slots and Function+Track navigation (`surface.ts:436-447`); IndexedDB + OPFS-preferred storage (`store.ts:78`, `102-135`); Memory Saver / High Memory Mode in MiB (`memory.ts:12-70`, `saver.ts`).

### Inaccurate — guide claims the code does not support
1. **Track double-tap "undo newest pass".** Code only performs recoverable delete / arm-cancel / stop (`surface.ts:452-474`, `recordingState.ts:198-204`). `rec.undoPass` exists in `commands.ts:49` and `engine.ts:2541` but no gesture emits it — dead path.
2. **Chop on Play + Rocker.** Code puts chop half/double/reset/glide on **Function + Rocker** double-tap and hold (`surface.ts:510-526`, `629`); Function + Rocker tap is the global scrub. The guide's "Function+Rocker = scrub only, chop lives on Play+Rocker" split does not exist.
3. **Shift + R master performance record.** `PerformanceRecorder` exists (`performanceRecorder.ts:17-76`) but no `KeyR`/shift handler exists anywhere; `KEY_MAP`/`FADER_KEYS` in `useDeviceSurface.ts:33-58` have no R and no ESC.
4. **ESC releases held controls.** Not bound.
5. **LED priority order.** Actual tiers (`surface.ts:958-973`): error 98 > failedPrint 95 > printing 93 > recording 90 > overdubbing 89 > armed 88 > momentaryFx 82 > latchedFx 78 > heads 76 > soloed 74 > unlinked 70 > active 66 > muted 30 > base 10. The guide omits printing/failedPrint and orders FX above recording states.

### Unconfirmed (needs a targeted read before being asserted)
Continuous wind reversal without source recreation (`engine.ts` transport internals); exact 5 s threshold for dim/full lights; whether PRINT's captured buffer truly excludes master/solo/global speed/FX; explicit rejection of reverse-direction recording; permission-never-on-load guarantee in the input panel.

### In the code, absent from the guide
Bluetooth pairing gesture (Vol− + Vol+ held ≥2 s → `system.pairing`, `chordArbiter.ts:297-298`); per-song snapshot memory (`surface.ts:280-299`); the diagnostics/coverage subsystem (`HitZoneAudit`, `DiagnosticPanel`, `fired`/`coverage` logs); FX schema v3→v4 migration.

### Reconciliation choice
Fix the **code** for items 3 and 4 (bind Shift+R and ESC — small, isolated, and the features already exist). Fix the **guide text inside the app** for items 1, 2 and 5 so the onboarding wizard teaches only behavior the code actually performs. The PDF itself is not editable from here; the in-app guide becomes the source of truth.

## Part 2 — Step-based onboarding wizard

### Shape
A dismissible overlay driven by a declarative step list, launched from a `learn` entry point on the TAPE tab and from the GUIDE tab. Each step is *completion-gated by real machine state*, not by a Next button: the wizard watches the same `state`/`status`/command stream the instrument already emits, so a step only clears when the user actually performs the gesture on the SP-1.

### Files
- `src/onboarding/steps.ts` — step definitions: `id`, `title`, `instruction` (gesture text), `highlight` (control ids from `geometry.ts` `CONTROL_LABELS`), `keyHint`, `done(ctx)` predicate over `{ state, status, sess, lastCommands }`, optional `setup()` (e.g. load demo), `skippable`.
- `src/onboarding/useOnboarding.ts` — cursor, per-step completion evaluation on each surface state change, `localStorage` persistence (`stemtape.onboarding.v1`: completed step ids + dismissed flag), `restart()`.
- `src/onboarding/OnboardingOverlay.tsx` — bottom-docked card (mobile) / right rail card (desktop): step n of N, instruction, live "waiting for…" vs "done" indicator, Skip step / Exit / Restart.
- `src/device/DeviceSurface.tsx` — accept an optional `highlight: ControlId[]` prop that renders a pulsing outline on the named hit zones (reuses existing hit-zone geometry; no new coordinate system).
- `src/routes/index.tsx` — mount overlay, add `learn` button in the header, replace the static "first moves" list in the GUIDE tab with a launcher plus the corrected control atlas.

### Step sequence (each gated on real state)
1. **Enable audio** — done when `status.contextState === "running"`.
2. **Load stems** — done when four decoded tracks exist; offers Try Demo as `setup()`.
3. **Start the tape** — tap Play; done when transport enters playing.
4. **Ride the mix** — move any fader; done when a fader value changes by >0.05.
5. **Mute and return** — tap a Track twice; done when a track mutes then unmutes.
6. **Cue frame zero** — hold Play; done on `transport.cue`.
7. **Varispeed** — rocker tap/hold; done when `state.speed` leaves 1.0, then Function+Play ×2 to snap back.
8. **Window and filter** — Function + Fader 1/2, then Function + Fader 4; done when window bounds and filter mode both change.
9. **Chop** — Function + Rocker double-tap; done when `chopDiv` changes. (Corrected per audit item 2.)
10. **Teach the grid** — Function ×4; done when `state.grid.source === "tap"`.
11. **FX overlay** — Vol− + Vol+ chord, select a bank, cycle an algorithm, hold for momentary; done when a momentary FX has been engaged at least once.
12. **Heads** — Function + Play ×3, raise head faders, Function + Fader scrub; done when heads mode entered and a scrub occurred.
13. **PRINT** — hold an empty Track in Heads; done on print completion.
14. **Recording** — Enable Input in System, hold an empty Track 450 ms to arm; done when armed. Marked skippable (needs a microphone).
15. **Save the project** — done when `sess.saved` is true.

Steps 11-15 form an "advanced" second half the user can defer; the wizard offers a natural stop after step 10 ("you can perform now").

### Behavior rules
- Never blocks the instrument: the overlay is non-modal and pointer events pass through to the SP-1.
- Auto-advances ~600 ms after a step completes, with a brief confirmation line, so the user hears what they just did.
- Skipping a step marks it skipped, not completed, and it reappears in the GUIDE tab's checklist.
- Reduced-motion respected on the highlight pulse; overlay is keyboard reachable and announced with `aria-live="polite"`.
- First visit auto-opens at step 1 only if no stems are loaded and nothing is persisted.

### Also in scope
- Bind `Shift+R` to `PerformanceRecorder` start/stop and `ESC` to release all held controls in `useDeviceSurface.ts`, then add both to `KEY_HINTS` (audit items 3, 4).
- Correct the in-app guide text for Track double-tap, chop location, and the LED priority table so it matches `surface.ts:958-973`.

### Tests
`src/onboarding/onboarding.test.ts` — each step's `done()` predicate against synthetic surface states (true only for the intended gesture), persistence round-trip, and skip semantics. A Playwright pass drives steps 1-6 on the real surface and screenshots the overlay at 420 px and desktop width.
