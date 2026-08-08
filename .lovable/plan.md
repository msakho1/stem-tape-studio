# Phase 5C — Stem Tape v1 Mapping + Four-Effect Performance Layer

Adds the stem-performance half of the instrument on top of the existing Phase 5A/5B tape engine, without a second gesture engine, second transport, or an audio "mode" switch. The FX overlay only re-labels the four track buttons; tape audio never stops.

Incorporates the 11 binding corrections.

## What gets built

**1. Versioned mapping registry — `src/machine/stemTapeV1Map.ts`**
Declarative rows (layer, required control order, controls, tap/hold threshold, semantic command, suppressed base commands, rollback, LED response, provenance: stock | v2.6 | reinterpreted | extension). The existing 37 v2.6 rows are re-exported unchanged and referenced, not rewritten. JSON export from the Mapping Lab.

**2. Ordered chord arbitration before dispatch (correction 1)**
Authoritative flow: raw pointer/key input → ordered chord arbitration → **one** semantic command → reducer + audio. Base Play/Volume/Track commands are never emitted-then-undone; arbitration holds the base command until the chord window resolves. `TxnSnapshot` rollback stays only as the safety fallback for genuinely optimistic multi-tap sequences and for lost pointers.
Precedence: cancel/safety → long system chords → Play-first → Function-first → FX-track-first → bare FX-overlay track → bare v2.6.
Chords: `Play+Vol−/+` select stem (wrap Vocals→Drums→Bass→Instruments); `Play+Track` short = solo, >700 ms = link/unlink, **duration measured from the overlap of the two controls, not from the initial Play press**; `Vol−+Vol+` (second within 120 ms, both released <600 ms) toggles the FX overlay, ~2 s holds the existing pairing gesture, the 600–2000 ms band is a diagnostics-only no-op; `FXtrack+Vol−/+` variation; `FXtrack+Function` short latch/unlatch; all four FX tracks + Function clears latches on the active stem.

**3. Performance state — `src/machine/stemPerformance.ts`**
`StemPerformanceState` as specified (`activeStem`, `fxOverlay`, per-track `soloed`/`linked`/`fx: Record<FxFamily, FxSlotState>`). Persisted per song: link, solo, variations, latches — never `momentary`, never `fxOverlay`.

**4. Semantic commands**
`stem.select`, `stem.solo`, `stem.link`, `fx.overlay`, `fx.momentary.start`, `fx.momentary.end`, `fx.variation`, `fx.latch`, `fx.clearLatches` added to `src/audio/commands.ts`, on the existing ordered command + ack path. Rejected activations ack `rejected` and leave audio and LEDs untouched.

**5. Signal graph — permanent FX bus and split gains (corrections 2 and 3)**
```text
Node tape out ── handoff envelope ─┐
                                   ├→ FxRackInput (permanent, never disconnected)
Worklet out ──── handoff envelope ─┘

FxRackInput → filter → beatRepeat ─┬→ directGate ──────────────┐
                                   ├→ echoInput → echoReturn ──┤
                                   └→ reverbInput → revReturn ─┤
                                                               ↓
                                                          faderGain (post-FX, controls tails)
                                                               ↓
                                                          soloGain (separate, smoothed)
                                                               ↓
                                                            master
```
Engine migration crossfades the two tape sources into the stable input — no re-parenting, no bypass window, tails/Beat-Repeat/filter state survive.
The existing combined `trackGain` splits into `directGate`, `echoInput`, `reverbInput`, `faderGain`, `soloGain`. Mute closes `directGate`/`echoInput`/`reverbInput` only and leaves feedback returns decaying. FX release closes only that FX's input, never the track gate. Solo never mutates saved mute state:
```ts
audibleBySolo = anySolo ? track.soloed : true;
inputOpen = audibleBySolo && (!track.muted || track.soloed);
```

**6. FX families — `src/audio/fx/` (lazy, truly bypassed when inactive)**
- **Filter (correction 4)**: one per-stem processor, three-layer state model — base Tape filter (`FN+Fader 4`), momentary FX preset override, latched FX state. Momentary press snapshots the underlying tape filter; unlatched release restores that snapshot (not forced dry); latching commits. Single automation owner for cutoff/type/Q. Variations: Warm LP, Resonant LP, Clean HP, Resonant HP. Centre remains the existing sample-identical true-dry bypass with complementary fades.
- **Echo (correction 5)**: 1/4, 1/8, dotted 1/8, 1/8 triplet. `effectiveBpm[track] = baseBpm * Math.abs(trackRate[track])` — per stem, so unlinked stems follow their own rate; BPM source hierarchy grid → manual → provisional 120. Division/rate changes use **two delay taps with a short crossfade**, not `delayTime` smoothing (no Doppler glide). Named, clamped feedback constants.
- **Reverb**: locally generated algorithmic FDN, no IR fetch. Tight Room, Plate, Hall, Atmospheric Wash with decay ceiling and output limiter.
- **Beat Repeat (correction 6)**: new `public/beat-repeat-processor.js` worklet, bounded rolling per-stem buffer sized from 1/2 note at the minimum supported effective BPM, counted in project diagnostics and the operation-level memory approval. Activation captures the immediately preceding **completed** division ending at the activation frame and begins repeating within one render quantum via a seam crossfade; with insufficient buffered audio it enters a visible **arming** state until one full division exists — never repeats stale memory. Underlying playhead untouched; release and division changes crossfade. An extreme tempo/rate that exceeds the buffer allowance rejects Beat Repeat only, with a clear ack. Restored latches re-arm and refill; ring-buffer contents are never persisted.

**7. Link/unlink audio semantics (correction 7)**
Unlinking is phase-continuous and never restarts the stem. Global Play/Stop still gates every stem. Tape operations target the linked group when the active stem is linked, and only that stem when it is unlinked; other unlinked stems keep their independent state. Relink happens at the next shared seam through the existing handoff envelopes, drift ≤ 2 frames.

**8. Storage schema bump and lifecycle (correction 8)**
Saved-project schema version bump with migration defaults for older projects: all stems linked, no solos, no latches, variation 1 per family. On song switch, stem replacement or deletion: clear momentary state, fade out DSP history, clear Beat Repeat buffers, keep stored FX configuration in recoverable trash where appropriate. Undo restores configuration and explicitly does not claim to restore tails or ring-buffer audio.

**9. LED priority table (corrections 9 and 10 of the LED spec)**
Explicit deterministic priority in `src/machine/surface.ts` (no LED writes from handlers): error > recording (Phase 6 reserved) > momentary FX > latched FX > soloed > unlinked > active > muted > base. Overlay on: both Function LEDs alternate-pulse; track LEDs show stem state (active brighter, soloed solid, non-solo faint, unlinked double pulse). Side LEDs 1–4 = Filter/Echo/Reverb/BeatRepeat for the active stem: dark / breathing (momentary) / solid (latched); during variation selection they show the variation number (5 = all four) then time out. Diagnostics render both the full underlying state and the winning pattern with the reason it won.

**10. Diagnostics + Mapping Lab** (`src/device/DiagnosticPanel.tsx`)
Layer, active stem, solo/link masks, tape target, all 16 slot states, per-stem effective BPM + source, echo division/time, reverb preset, repeat division + buffer frames + arming state, recognized ordered chord, suppressed commands, rejected activations, engine source, FX state before/after migration, worklet load, tunable timings/preset params, JSON export, and a fallback to clear all project latches.

## Verification and numeric acceptance (correction 11)

Objective thresholds asserted in tests, not just captured WAVs:
- peak sample-to-sample discontinuity at any transition ≤ 0.02 full-scale
- short-window (5 ms) RMS energy jump ≤ 3 dB across seams, mute, FX release, relink
- relink drift ≤ 2 frames
- Beat Repeat slice length exact to the frame for every division at the tested BPM/rate
- Echo tap timing within 1 frame of `60/effectiveBpm × divisionRatio`
- Filter centre vs true dry: null difference ≤ −120 dBFS (sample-identical path)
- all-16-slot test asserts audible DSP output per slot (measured wet/dry delta), not enabled flags

Also: v2.6 regression 37/37 with the overlay closed and non-conflicting tape gestures still firing with it open; chord ordering/suppression, solo-vs-link overlap timing, overlay short/long/ambiguous volume chord, pointer-cancel cleanup, filter tape→momentary→release and latched→fader-sweep tests, playhead invariance under Beat Repeat, save/reload restore, Node↔Worklet migration parity, `tsgo` clean, zero console errors, and a network-log assertion that no user audio leaves the device.

**Browser verification honesty (correction 10):** Playwright at 375 px with an iPhone UA is reported as **mobile emulation only**. I will ship a real-device checklist for you to run in iPhone Safari — multitouch chord reliability, four simultaneous Beat Repeats, four reverbs and four echoes, all 16 slots, background/interruption recovery, Node↔Worklet migration, the large-project memory case, audible glitches and latency — and will not claim iPhone verification until you report results.

## Sequence

1. Mapping registry + performance state + commands (no audio yet).
2. Ordered chord arbitration + suppression-before-dispatch + LED priority; re-run 37/37.
3. Signal-graph split (permanent FX bus, split gains) with filter ownership model.
4. Echo, reverb, Beat Repeat worklet.
5. Storage schema bump, lifecycle, migration parity, diagnostics.
6. Full verification pass, numeric measurements, completion report + iPhone checklist.
