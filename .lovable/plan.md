# Phase 5C — Stem Tape v1 Mapping + Four-Effect Performance Layer

Adds the stem-performance half of the instrument on top of the existing Phase 5A/5B tape engine, without a second gesture engine, second transport, or an audio "mode" switch. The FX overlay only re-labels the four track buttons; tape audio never stops.

## What gets built

**1. Versioned mapping registry — `src/machine/stemTapeV1Map.ts`**
Declarative rows (layer, required control order, controls, tap/hold threshold, semantic command, suppressed base commands, rollback, LED response, provenance: stock | v2.6 | reinterpreted | extension). The existing 37 v2.6 rows are re-exported unchanged and referenced, not rewritten. JSON export from the Mapping Lab.

**2. Ordered chord recognition inside the existing `GestureEngine`**
Extend `src/input/gestures.ts` with ordered-pointer chord emission (first-down control decides meaning) plus new gesture kinds. Precedence: cancel/safety → long system chords → Play-first → Function-first → FX-track-first → bare FX-overlay track → bare v2.6. Suppression happens before the base command reaches the reducer, using the existing `TxnSnapshot` rollback — no post-hoc snapshot diffing.

New chords: `Play+Vol−/+` (select stem, wrap Vocals→Drums→Bass→Instruments), `Play+Track` short (solo) / >700 ms (link-unlink, mutually exclusive with solo), `Vol−+Vol+` short (<120 ms arrival, both released <600 ms) toggles the FX overlay while ~2 s holds the existing pairing gesture and the 600–2000 ms band is a diagnostics-only no-op, `FXtrack+Vol−/+` (variation), `FXtrack+Function` short (latch/unlatch), all four FX tracks + Function (clear latches on active stem).

**3. Performance state — `src/machine/stemPerformance.ts`**
`StemPerformanceState` exactly as specified (`activeStem`, `fxOverlay`, per-track `soloed`/`linked`/`fx: Record<FxFamily, FxSlotState>`). Solo audition preserves and restores the underlying mute state. Persisted per song via `src/audio/session.ts`/`store.ts`: link, solo, variations, latches — never `momentary`, never `fxOverlay`.

**4. Semantic commands**
`stem.select`, `stem.solo`, `stem.link`, `fx.overlay`, `fx.momentary.start`, `fx.momentary.end`, `fx.variation`, `fx.latch`, `fx.clearLatches` added to `src/audio/commands.ts`, routed through the existing ordered command + ack path. A rejected activation acks `rejected` and leaves audio and LEDs untouched.

**5. FX rack — `src/audio/fx/` (per stem, lazy)**
Routing inserted between the existing per-track tape output and the existing track fader, identical for the Node and Worklet engines:
```text
tape out → filter → beat-repeat → gate ─┬→ direct
                                        ├→ echo send/fb/return
                                        └→ reverb send/return
        → track fader (post-FX) → solo/master bus
```
- **Filter** reuses the existing per-track `BiquadFilterNode` + complementary dry/wet fade in `engine.ts` — no second filter. Bipolar, centre is the existing true-dry bypass. Variations: Warm LP, Resonant LP, Clean HP, Resonant HP; `FN+Fader 4` keeps sweeping the same nodes.
- **Echo**: tempo-synced 1/4, 1/8, dotted 1/8, 1/8 triplet; `effectiveBpm = baseBpm * currentTapeRate` with the grid → manual → provisional-120 hierarchy; named, clamped feedback constants; smoothed delay-time changes.
- **Reverb**: locally generated algorithmic FDN (no IR fetch). Tight Room, Plate, Hall, Atmospheric Wash with a decay ceiling and output limiter.
- **Beat Repeat**: new `public/beat-repeat-processor.js` AudioWorklet with a bounded rolling per-stem buffer; 1/2…1/32; sample-accurate retrigger, click-free seam, underlying playhead untouched, crossfade back to live on release and across division changes. No timers.
Mute closes the gate and the sends but lets echo/reverb tails decay through the fader. Engine migration re-parents the tape source into the same rack — latches, variations, active stem, solo/link preserved, no bypass window.

**6. LED arbitration** (`src/machine/surface.ts` only — no LED writes from handlers)
Overlay on: both Function LEDs alternate-pulse; track LEDs = stem states (active brighter, soloed solid, non-solo faint, unlinked double pulse). Side LEDs 1–4 = Filter/Echo/Reverb/BeatRepeat for the active stem: dark / breathing (momentary) / solid (latched). During variation selection the side LEDs show the variation number (5 = all four) then time out back to FX state.

**7. Diagnostics + Mapping Lab** (`src/device/DiagnosticPanel.tsx`)
Layer, active stem, solo/link masks, tape target, all 16 slot states, effective BPM + source, echo division/time, reverb preset, repeat division + buffer frames, recognized ordered chord, suppressed commands, rejected activations, engine source, FX state before/after migration, worklet load, plus tunable timings/preset params and a JSON export. Fallback button to clear all project latches.

## Verification I will run and report

- v2.6 regression 37/37 with the overlay closed, and non-conflicting tape gestures still firing with it open.
- Unit tests: chord ordering and suppression, solo-vs-link threshold, overlay short/long/ambiguous volume chord, pointer-cancel cleanup, filter-centre sample-identity to dry, echo division frame lengths vs effective BPM, bounded feedback, repeat division frame lengths, playhead invariance, relink drift ≤ 2 frames (tolerance-based).
- Playwright at 375 px: the full section-14 acceptance performance, multitouch, save/reload restore, Node↔Worklet migration, all 16 slots active, console + `tsgo` clean, and a network-log assertion that no user audio leaves the device.
- Captured-output WAV fixtures for discontinuity measurement at each transition.

## Sequence

1. Mapping registry + performance state + commands (no audio yet).
2. Ordered chord recognition + suppression + LED arbitration; re-run 37/37.
3. FX rack scaffold with filter reuse; then echo, reverb, beat-repeat worklet.
4. Persistence, migration parity, diagnostics.
5. Full verification pass and completion report.
