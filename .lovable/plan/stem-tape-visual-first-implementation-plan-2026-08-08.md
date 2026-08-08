# Stem Tape — Visual-First Implementation Plan

Phase 0 (source audit) is **done** and reported below; everything from Phase 1 onward awaits approval. No production code written.

## 1. Feasibility verdict

| Area | Verdict |
| --- | --- |
| Inline-SVG device with depressing buttons, dragging faders, tilting rocker, animated LEDs | **Straightforward.** Standard SVG + Pointer Events + CSS. |
| Multitouch chords (Function + track, release-together) on touchscreens | **Straightforward on iOS/Android with correct `touch-action`.** Needs care, not research. |
| Multi-tap disambiguation (tap / ×2 / ×3 / ×4, tap-then-hold) without sluggish taps | **Technically challenging but solvable** — optimistic-fire + retroactive-revise, see §8. |
| Full Tape Looper v2.6 state model as a pure reducer | **Straightforward**, and the highest-value part of this phase. |
| Reproducing v2.6 *timing thresholds* accurately | **Not derivable from the control map.** Thresholds are parameters to be measured on your SP-1 (§13 checklist). |
| Deciding the final Stem Tape gesture map | **Deliberately deferred.** Conflict matrix first, candidates tested in the Mapping Lab. |
| Non-uniform-scaled SVG with reliable pointer math | **Needs change** — the current outer transform must go (§4). |
| Real audio, sample accuracy, MIDI clock, punch-in | **Out of scope this phase**; sketched in §12 with explicit unknowns. |

## 2. Attachment inventory and what each is trusted for

| File | Role | Status |
| --- | --- | --- |
| `sp1-control-map.png` | Authoritative Tape Looper v2.6 behaviour | Transcribed, §3 |
| `603438A7…jpeg` (KE guide rev.02, 2019.12.12) | Authoritative stock SP-1 behaviour | Transcribed, §4 |
| `F219BD6F…jpeg` | Authoritative physical layout | Used as the fidelity target for the SVG audit |
| `stem-tape-sp1-interactive.svg` | Implementation starting point, 158 lines, viewBox `0 0 720 940` | Audited, §6 — passes structurally, five required changes |

## 3. Tape Looper v2.6 transcription

**Rocker (tape speed):** fwd/rwd = speed and pitch, ±1 BPM steps · double-click = exact semitone · FN + fwd/rwd = chop half/double, hold = glide · FN + double-click = chop reset.
**Volume:** vol −/+ = master volume · FN + vol = slide the chop window, hold = glide.
**Faders:** 1–4 = track volumes; per head in heads mode. With FN held: 1 = window start, 2 = window end, 3 = shift, free sizes; **fader 1 past fader 2 ⇒ window plays in reverse**; 4 = filter, mid = off, down = LP, up = HP. In heads mode, FN + faders scrub the heads.
**Track buttons:** hold = record, starts on first sound · tap = stop the take / mute / unmute · double-tap = delete · FN + track = banks: jump, tap again = next song.
**Play:** tap = play/stop · hold = restart from top. With FN held: release together = fixed/variable loops · tap ×2 = snap to 1.0× · tap ×3 = heads mode · hold thru 5 s = dim/full lights.
**Function:** hold = power on/off · tap ×4 in rhythm = the song gets a tempo grid · re-tap over loops = beatmatch to your taps · tap, then quick hold = clear the grid · tap ×4 then hold = round to nearest whole BPM; **unless it's far off — all four blink, nothing moves**.
**Gridded songs:** tap the tempo, lights become a metronome and MIDI clock follows; takes punch in on the beat and compensate for a late press; the grid keeps learning the tempo.
**Heads mode:** FN + triple-tap play = on/off · 3 tracks replay the source a quarter apart · FN + faders scrub · double-tap = reverse · hold a track: loaded = tape, empty = PRINT.
**Lights:** dark = empty · faint = muted content · pulse = playing, each light pulses as its own loop wraps (polyrhythm) · song row: solid = song, blink = bank.
**Songs/transfer:** 16 songs, each remembering loops, speed, chop, mutes, grid; takes to 8:00, longer with the tape slowed; WAVs in and out on the transfer page. Flashing: hold track 1 + track 4, plug in USB.

## 4. Stock SP-1 transcription (KE guide rev.02)

**Power/volume:** hold function ~2 s on/off; press play to confirm master volume; play = start/pause.
**Modes:** four — basic; normal; normal + PO sync out; normal + MIDI thru/capture. Enter with function + play held ~2 s (4 tap LEDs flash), volume +/− to select, play to confirm.
**Charge/pair:** USB-C charging, side LEDs show battery; hold volume + and − ~2 s to scan for Bluetooth, scanning shown on side LEDs.
**Rocker/faders:** rocker = fast-forward/rewind; function + rocker = skip track forward/back; while using the rocker, volume buttons select playback speed; faders set individual stem volume.
**Track selection:** short click on function enters track select, top LEDs pulse, then volume +/− steps through tracks, solid top LED = active track; **function + track buttons = solo**.
**Effects:** press a track button *without* function to activate that track's effect; four variations each; hold the track button and use volume +/− to pick the variation. Printed labels read `rev`, then three more — legible as approximately `pitch`, `delay`, and a fourth that **cannot be read reliably in either scan**. Not named; marked `VERIFY ON HARDWARE`.
**Loop:** press and hold play to activate loop, shown on side LEDs; while holding play, volume sets loop fade-in and the rocker moves the loop; looping is indicated while play is held; if not latched and not looping, function + play + volume sets the loop divider.
**Latch:** effects, loop and fwd/rew latch with a short function press while holding the control. Play unlatches loop; the rocker unlatches fwd/rew. A short function click while holding a latched fx button un-latches fx on the active track; holding all track buttons while pressing function clears all fx latches.
**Sync:** mode 3 = PO sync via sync jack (PO clock 1-5-7); mode 4 = MIDI clock via 3.5 mm jack, or a 3.5 mm-to-DIN adapter.

**Stock classification.** *Essential creative:* four stems, four faders, solo, effects + variations, loop activate/move/fade/divide, playback-speed selection, latching. *System:* power, pairing, battery, mode select, PO/MIDI sync, capture. *Superseded by a richer v2.6 equivalent:* rewind/fast-forward (v2.6 varispeed), loop move/divide (chop window), song switching (banks), playback speed (tape speed). *Directly conflicting:* track tap/hold, FN + track, FN held gestures, volume combinations. *Firmware-relevant only:* pairing, battery, sync jack modes.

## 5. Ambiguity list (no behaviour invented)

1. Fourth stock effect label — illegible. Also `pitch`/`delay` spellings unconfirmed.
2. Whether stock supports **multiple simultaneous solos**.
3. Stock loop-divider value set and the exact meaning of "loop fade-in".
4. **Overdub is not documented anywhere in v2.6.** Track hold = record; nothing states layering. Any overdub is labelled *new Stem Tape extension*.
5. "Three tracks replay the source, a quarter apart" — a quarter note or a quarter of the loop; which three tracks; what the fourth does.
6. "loaded = tape, empty = PRINT" — PRINT is undefined by the map.
7. "Release together" tolerance window for fixed/variable loops.
8. "Tap ×4 in rhythm" tolerance, and how far is "far off" before the four-light rejection blink.
9. "Rocker double-click" on a three-position momentary rocker — presumably two rapid deflections in the same direction; unconfirmed.
10. Whether v2.6 retains any stock effects at all.
11. Interaction of stock latching with v2.6 (v2.6's map never mentions latch — likely removed).

Each of these appears in-app as a `VERIFY` badge and on the physical checklist.

## 6. SVG audit — findings

**PASS:** well-formed XML; `viewBox="0 0 720 940"`; **all 35 IDs unique** (verified by count); every element the brief expects is present (`volume-minus`, `volume-plus`, `rocker`, `play-button`, `function-button`, `fader-1..4`, `track-button-1..4`, `track-led-1..4`, `play-indicator`, `function-led-1/2`); no raster images, no `xlink:href`, no external assets, no web fonts; decorative children already carry `pointer-events="none"`; `data-control` / `data-track` / `data-state` hooks already exist; layout matches the reference JPEG (volume on top, rocker left, play top-right, function bottom-right, 4 faders, 4 LED dots, 4 track buttons, red triangle, two function dots).

**NEEDS CHANGE:**

1. **`<g id="device-art" transform="translate(-72 0) scale(1.2 1)">` — non-uniform scale.** It stretches X by 1.2 while leaving Y, so every `<circle>` (fader caps r=12, track LEDs r=7, function LEDs r=6.5) renders as an **ellipse**, and every `rx` corner is distorted. It also sits between screen coordinates and geometry for all pointer math. **Decision: bake it.** Apply `x' = 1.2x − 72` to every X coordinate and width, convert the four fader caps and six LED circles to `<ellipse>` only if the wider look is wanted (recommendation: keep them **circular** — the reference shows round caps and round dots), then delete the wrapper. Rationale: `getScreenCTM().inverse()` would handle the math correctly, but a non-uniform CTM makes radii, stroke widths and any future rotation lie, and it silently breaks `transform-box: fill-box` press animations differently per engine. Baking removes a whole class of cross-browser bugs for a one-time edit.
2. **Root `role="img"`.** An `img` role makes the entire subtree presentational, so the nested `role="button"` / `role="slider"` children are hidden from assistive tech. Replace with `role="group"` and keep `aria-labelledby`.
3. **Three unused defs:** `deviceShadow`, `controlShadow`, `deviceClip` are declared and never referenced. Remove. (`ledGlow` and all four gradients *are* used — keep.)
4. **`filter: url(#ledGlow)` on LEDs.** A Gaussian blur re-rasterising on every pulse frame is the known Safari performance cliff. Replace with a static, pre-blurred halo `<circle>` per LED whose `opacity` is animated — same look, compositor-only.
5. **`transform-box: fill-box` in the embedded `<style>`.** Support is inconsistent in older Safari, where the press animation silently does nothing. Wrap each pressable face in a `<g>` and animate that group's `transform` instead.

Also: the rail carries only the play triangle and two function dots — the **4-dot side LED column** visible in the control map (song/bank row) is **missing** and must be added as `side-led-1..4`. The rocker is one element; it needs two hit zones (fwd/rwd) over one visual body. The embedded `<style>` block and the def IDs both become global once inlined — scope the CSS and prefix def IDs with `useId()`.

**Integration recommendation:** a **hand-maintained typed React component**, `DeviceSurface.tsx`, converted once (SVGR for the mechanical `class→className` / `stroke-width→strokeWidth` pass, then curated). Not `<img src>` — no element access. Not raw SVGR output — it flattens semantics. Every control becomes a small subcomponent taking typed props (`pressed`, `value`, `ledState`) with no internal logic.

## 7. Control-conflict matrix (excerpt; the full table is a deliverable in the app)

| Control · gesture | Tape Looper v2.6 | Stock SP-1 | Stem Tape need | Conflict | Proposed resolution | Physical test |
| --- | --- | --- | --- | --- | --- | --- |
| Track · hold | Record (on first sound) | Activate that track's effect | Both | **Direct** | v2.6 wins the bare gesture; stock effects move to the Stem layer | Yes |
| Track · tap | Stop take / mute / unmute | (effect engage) | Both | Direct | v2.6 wins; state-dependent (§ below) | Yes |
| Track · double-tap | Delete | — | Destructive | Safety | Require an LED confirm window | Yes |
| FN + track | Banks: jump, tap again = next song | **Solo** | Both, constantly | **Direct, highest-value** | Solo relocates to the Stem layer; banks keep FN + track | Yes |
| Rocker fwd/rwd | Tape speed ±1 BPM | Fast-forward / rewind | Varispeed | Superseded | v2.6 wins; stock FF/RW dropped as redundant | No |
| FN + rocker | Chop half/double | Skip song | Both | Direct | v2.6 wins; song nav via FN + track banks | No |
| Rocker + volume | — | Playback-speed select | Superseded | None | Drop; rocker already sets speed | No |
| Play · hold | Restart from top | **Activate loop** | Both | **Direct** | v2.6 wins; loop creation is the chop window | Yes |
| FN + play · release together | Fixed/variable loops | Enter mode select (2 s) | Both | Direct + timing | Duration disambiguates; needs measurement | Yes |
| FN + play · tap ×2 / ×3 | 1.0× snap / heads | — | — | None | Keep | Yes |
| FN + play + volume | — | Loop divider | Chop | Overlap | FN + volume already slides chop | No |
| FN + volume | Slide chop window | Loop fade / mode select / variation step | Both | Direct | v2.6 wins | No |
| Volume + and − together | — | Bluetooth pairing | System | Free in v2.6 | Keep stock gesture | Yes |
| FN · hold | Power | Power | Same | None | Identical | No |
| FN · tap | Tempo grid (×4) | Enter track select | Both | **Timing ambiguity** — a single tap must not be swallowed by ×4 detection | Single FN tap is a strong Stem-layer candidate | Yes |
| FN short-press while holding a control | — | **Latch** | Sticky behaviour | v2.6 silent | Do not assume latch survives v2.6; verify | Yes |
| Hold all 4 tracks + FN | (flash mode uses track 1+4+USB) | Clear all fx latches | — | Partial | Reserve; risk of flash-mode collision | Yes |
| FN + faders | Window start/end/shift, filter, head scrub | — | Heavily used | None | Keep verbatim | No |
| Faders bare | Track volume | Stem volume | Same | **None — the load-bearing agreement** | Identical meaning; this is why the integration works | No |

**Track-tap interpretation is state-dependent** and must be a table, not an if-chain: empty → arm; armed-waiting → cancel arm; recording → stop take; loaded+playing → mute; loaded+muted → unmute.

## 8. Combined behaviour model and control-layer candidates

**Model:** the four imported stems *are* the four v2.6 tracks, sample-aligned at load, marked `loaded` and unmuted. Tape Looper v2.6 is the operational foundation and its map is preserved intact. Stock capabilities that survive (solo, per-stem effects + variations, stem select) are layered into unresolved control space. Song memory becomes the Stem Tape project. Every feature in the app is tagged `v2.6` / `stock` / `reinterpreted` / `new extension` — overdub is tagged **new extension** because v2.6 does not document it.

**Momentary layer, not a mode.** The layer changes only what buttons mean while it is active. It never stops the transport, unloads tracks, resets loops or speed, or swaps engines. This is stated as an invariant and asserted in tests.

**Candidate activation gestures, for testing rather than adoption:**

| Candidate | v2.6 collision | Stock collision | One-handed | Accident risk | LED cost |
| --- | --- | --- | --- | --- | --- |
| FN single tap (momentary while held after tap) | Shadows the ×4 grid tap | Track select | Good | Medium | 1 pattern |
| Double-tap FN then hold | Near "tap, then quick hold" = clear grid | None | Fair | **High** | 1 |
| Hold play + hold FN chord | Near mode-select and release-together | Mode select | Poor | Low | 1 |
| Hold two adjacent track buttons | Free in both maps | Near fx-latch-clear | Fair | Low | 2 |
| Latched layer via a rocker chord | Chop reset lives on FN + rocker dbl | FF/RW | Good | Medium | 1 |
| Companion-app-only (no gesture) | None | None | n/a | None | 0 |

Recommendation: prototype the **two-adjacent-track chord** and **FN-tap-hold** first; keep rarely-used settings companion-app-only. Nothing is declared final before hardware testing.

## 9. Architecture

```text
 SVG DeviceSurface (dumb, typed props)
        │ pointerdown/move/up/cancel  +  keyboard
        ▼
 PointerRuntime      pointerId → {control, startPos, startTime, moved, captured}
        │ RawInput events
        ▼
 GestureInterpreter  tap · multiTap · hold · tapThenHold · chordStart ·
        │            chordRelease(spreadMs) · drag · latch
        │ Gesture
        ▼
 MappingRegistry (v2.6 locked · stock locked · experimental) → Command
        ▼
 DeviceMachine (pure reducer)  ── slices: transport · tracks · loops/chop ·
        │                          heads · grid · bank/song · fx · layer
        ├─► LedArbiter  → LedFrame (priority-resolved)
        ├─► SimEngine   → playheads, meters, waveform scroll (rAF, refs only)
        └─► Diagnostics → human-readable state + conflicts
```

**State library:** a hand-written typed reducer plus a small event queue, **not XState**. Justification: the hard parts here are *timing* (multi-tap windows, chord-release spread, 5 s holds) and *conflict arbitration*, neither of which statecharts make easier, while XState's actor model adds a second mental model on top of a domain that is already a state machine. The reducer is pure, serializable, replayable and testable in milliseconds.

**Render isolation:** the reducer output feeds a store with per-slice selectors. High-frequency values (playhead positions, meter levels, fader drag, LED phase) never enter React state — they are written to refs and applied by a single rAF loop that mutates SVG attributes directly. A fader drag causes **zero** React re-renders until pointer-up commits the value.

**Multi-tap without sluggishness:** fire the tap's effect **optimistically** on release, and if a second tap arrives within the window, *revise* — undo the optimistic effect and apply the ×2 behaviour. This keeps single taps instant, which is non-negotiable musically. Controls whose single-tap effect is destructive or non-revisable (track double-tap = delete) are the exception and wait out the window. Windows are named constants in one file, tunable from the Mapping Lab, and the documented 5 s hold is measured against `performance.now()`, never a chain of timeouts.

**Suggested types** (abbreviated; full set in the code):

```ts
type Control = 'play'|'function'|'volume-minus'|'volume-plus'|'rocker-fwd'|'rocker-rwd'
             | `track-button-${1|2|3|4}` | `fader-${1|2|3|4}`;
type TrackContent = 'empty'|'armed'|'recording'|'loaded'|'muted';
type RockerPosition = 'rewind'|'center'|'forward';
interface ChopWindow { start: number; end: number; reversed: boolean }   // reversed when start > end
interface TrackSlice { content: TrackContent; volume: number; loopBeats: number;
  chop: ChopWindow; filter: {mode:'off'|'lp'|'hp'; amount:number}; reversed: boolean;
  fx: {slot: FxSlot; variation: 0|1|2|3} | null; soloed: boolean; linked: boolean; stem: StemRole }
interface Grid { bpm: number|null; taps: number[]; learning: boolean; rejected: boolean }
interface LedState { id: LedId; pattern: 'dark'|'faint'|'solid'|'pulse'|'blink'|'chase';
  phaseSource: 'own-loop'|'grid'|'system'; priority: number; expiresAt?: number }
```

## 10. LED arbitration

The v2.6 meanings are the base layer and are never silently overwritten. Every added indication declares priority, pattern, duration and exit.

| Priority | State | Pattern | Duration | Overrides loop pulse? | Exit |
| --- | --- | --- | --- | --- | --- |
| 100 | Rejected tempo | all four blink | ~1 s | Yes | auto |
| 90 | Delete confirm | fast blink on that track | window only | Yes | act or lapse |
| 80 | Recording | solid bright | while recording | Yes | stop take |
| 70 | Armed, waiting for sound | slow breathe | until first sound | Yes | sound or cancel |
| 60 | Stem layer active | all four faint offset chase | while held | Yes | release |
| 50 | Solo active | soloed solid, others dark | until cleared | Yes | clear solo |
| 40 | Selected stem | brighter solid | while in select | No — combines | leave select |
| 30 | Grid metronome | pulse on beat | while gridded | Combines | clear grid |
| 20 | **Playing** (v2.6) | pulse on own loop wrap | continuous | base | — |
| 10 | **Muted content** (v2.6) | faint | — | base | — |
| 0 | **Empty** (v2.6) | dark | — | base | — |

Brightness/animation only — **no new colours are assumed possible on hardware.** Anything colour-coded in the browser is flagged `browser-only visualization` in diagnostics.

## 11. Components, hit zones, input

```text
src/device/     DeviceSurface.tsx  Fader.tsx  TrackButton.tsx  PlayButton.tsx
                FunctionButton.tsx  VolumeButtons.tsx  Rocker.tsx  Led.tsx  HitZones.tsx
src/input/      pointerRuntime.ts  gestures.ts  keyboard.ts  chordSimulator.ts
src/machine/    machine.ts  slices/*.ts  commands.ts  ledArbiter.ts  simEngine.ts
src/mapping/    tapeLooperMap.ts (locked)  stockMap.ts (locked)  conflicts.ts  registry.ts
src/panels/     MappingLab  Diagnostics  DemoController  AmbiguityList  ConflictMatrix
src/routes/     index.tsx  /mapping  /diagnostics  /sources
```

**Hit zones:** a sibling `<g id="hit-zones">` rendered last, `fill="transparent"`, `pointer-events="all"`, each rect sized so it is ≥44 CSS px at the smallest supported render width (computed from the viewBox scale, not hardcoded), non-overlapping, and toggleable to a visible tint in dev mode. Visible art gets `pointer-events="none"`; zones own all input. The rocker gets two stacked zones over one visual body.

**Pointer handling:** `setPointerCapture` on down, released on up/cancel, with a `lostpointercapture` handler that force-releases machine state so a control can never stick. `touch-action: none` and `user-select: none` **only on the performance surface**; the rest of the page scrolls normally. `contextmenu` prevented on the surface only. Movement past ~6 px cancels a button press but never a fader drag (faders opt out of the hold timer entirely).

**Fader math:** local coordinates via `new DOMPoint(e.clientX, e.clientY).matrixTransform(svg.getScreenCTM()!.inverse())`, then `clamp(1 - (y - trackTop)/trackHeight, 0, 1)` — correct at any responsive size, and unambiguous once the non-uniform transform is baked out. Layer changes (bare → FN-held → heads) use **pickup/relative** semantics: the first movement after a layer change adjusts from the existing value rather than jumping to the finger position.

**Desktop chords:** hold `F` for Function, `Space` for Play, `1`–`4` for tracks, `↑/↓` for the rocker, `+/-` for volume — real held keys, so `F` + click on a track is a genuine chord. Plus a dev-only sticky-hold toggle and a chord simulator in diagnostics. None of this changes touch behaviour or the machine's logic.

## 12. Future audio architecture (sketch only)

Straightforward Web Audio: one `AudioContext`, four buffers started at a shared `startAt`, per-track `GainNode`, `BiquadFilterNode` for the LP/HP fader, `playbackRate` for tape varispeed (pitch follows speed — correct), reversed buffer copies for reverse. Requires **AudioWorklet**: click-free chop-window boundaries, bidirectional reads, heads mode, onset-triggered recording, WAV capture. Browser-dependent: codec support, autoplay unlock, mobile memory, `MediaRecorder` formats. **Hardware/firmware-only:** MIDI clock out on the sync jack, PO sync, eMMC persistence, LED hardware limits. **Needs DSP research:** tempo estimation, continuous grid refinement, look-back punch-in. No sample-accuracy claim will be made without the drift instrument that measures it.

## 13. Testing and physical verification

**Automated (no DOM):** every documented v2.6 gesture produces the documented state change; all five track-tap states; chord release-spread detection; the optimistic-tap revision path; LED arbitration ordering; conflict detection against known-colliding and known-safe bindings; the demo script reaching its expected end state. **DOM tests:** synthetic pointer sequences for stuck-control recovery, `lostpointercapture`, two-finger chords, fader drag precision. **Visual regression:** screenshots at phone/tablet/desktop widths in Chromium, WebKit and Firefox to catch the filter and `transform-box` failures the audit predicts.

**Physical SP-1 checklist** (printable in-app): measure the hold threshold for record; the ×4 tap-in-rhythm tolerance and the "far off" rejection boundary; the release-together window; the FN-tap-vs-×4 disambiguation delay; power-hold duration; the 5 s dim/full hold; rocker double-click timing and whether it is direction-specific; record-on-first-sound threshold and latency; what heads mode actually does to which tracks; PRINT behaviour on an empty track; bank/song navigation order; whether the grid keeps learning; tape-speed increment size; the fourth stock effect label; whether stock allows multiple solos; whether v2.6 retains latching.

**Browser matrix:** iOS Safari, Android Chrome, desktop Chrome/Safari/Firefox/Edge — multitouch reliability, fader smoothness, no page scroll during gestures, no stuck controls, proportional scaling, LED legibility, keyboard support, input-to-visual latency.

## 14. Phases and acceptance criteria

- **P0 Source audit — complete.** Both maps transcribed, 11 ambiguities logged, conflict matrix drafted, SVG audited with 5 required changes.
- **P1 SVG correction + React integration.** *Accept:* transform baked out and removed; circles render circular; 3 unused defs gone; root role fixed; side LED column added; hit zones ≥44 px at 375 px width; identical rendering in all three engines; typed component with zero `any`.
- **P2 Raw interaction.** *Accept:* every control depresses on pointer-down; faders track the pointer at 60 fps with no React re-render; rocker returns to centre; two-finger chords register; cancellation never leaves a stuck control; full keyboard operation.
- **P3 v2.6 simulation.** *Accept:* every row of the control map is reproducible in the app and covered by a test; LED behaviour matches the documented meanings including per-track polyrhythmic wrap pulses; 16 songs / 4 banks navigable; grid and heads simulated. No audio.
- **P4 Stem integration.** *Accept:* four labelled aligned stems occupy the four tracks, share one simulated transport, and every v2.6 manipulation applies to them without breaking the simultaneity invariant.
- **P5 Mapping experiments.** *Accept:* stock inventory layered in; ≥3 layer candidates testable; conflict warnings live; presets saved; a recommended mapping exported as JSON.
- **P6/P7 Audio.** Only after the interaction model is approved.

## 15. Risks and blocking questions

**Risks:** timing thresholds are guesses until measured; the Function button is dangerously overloaded (power, grid ×4, grid clear, track select, latch, chords) and is the most likely source of misfires; Safari filter performance and `transform-box`; iOS may still steal a two-finger gesture; the optimistic-tap-revision pattern is only valid where effects are revisable; a browser-tuned gesture set may not survive on hardware with different switch feel.

**Blocking questions for you:** (1) Do you have a physical SP-1 available for the measurement checklist, and roughly when? (2) Does v2.6 keep stock latching, or replace it? (3) Should the four stock effects be reimplemented at all, given v2.6's filter and chop already cover part of that ground? (4) Is losing stock fast-forward/rewind acceptable in exchange for v2.6 varispeed? I have assumed yes.

## 16. Recommended first scope after approval

**P1 + P2 in one cycle:** the corrected SVG as a typed React component with hit zones, full pointer/keyboard input, working faders and rocker, visible press feedback, live LED rendering, and the diagnostics panel showing raw pointers and recognized gestures. That is the smallest build that proves the device is genuinely playable in a browser — everything after it is state-machine work that is far cheaper once the surface is trustworthy.

Design note: restrained instrument aesthetic matching the layout reference; labelled **"Unofficial Stem Tape Prototype"**; no TE logos, wordmarks or trade dress.
