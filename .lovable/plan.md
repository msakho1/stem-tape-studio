# Stem Tape — Performance Expansion (Multi-Fader, Tape Inertia, 12 FX)

## A. Audited current state (verified reads)

Single-fader cause — `src/device/useDeviceSurface.ts`:
- L84 `const dragRef = useRef<{ index; pointerId; channel } | null>(null)` — one singleton drag session.
- L86 `pendingCyRef` holds a single `{index, cy, value}`; L245-265 the shared rAF flushes only that one entry.
- L309 `if (!drag || drag.pointerId !== e.pointerId) return;` — a second pointer's moves are dropped.
- L284 a second pointer-down overwrites `dragRef`, stealing ownership from the first finger.
- L324/L348 `endDrag`/`cancelDrag` take no pointer id, so any up/cancel ends whatever drag is current.
- L239-243 `resolveChannel()` reads a mutable `layerRef`; correct per-gesture, but there is no rebase when a modifier changes mid-drag.
- `src/audio/controlBus.ts` is already per-`channel:index` keyed — no change needed to be multi-pointer safe.
- `src/styles.css` L179/L194 already scope `touch-action: none` to the surface.

Transport — `src/audio/engine.ts` L1705-1743: `transport.play` calls `startAll(offset)` (shared `startAt = currentTime + LOOKAHEAD_S`, `timeline.anchor`), `transport.stop` anchors and hard `stopSources()`. No ramp on either. Rate ramps already exist and are correct: `rate.set` L1805-1842 uses `glideCurve` + `timeline.glideTo` with the exact exponential integral (`src/audio/glide.ts` `integratedDistance`), and forwards `setRate {rampFrames}` to the worklet.

FX — `src/machine/stemPerformance.ts`: `FX_FAMILIES = [filter, echo, reverb, beatRepeat]`, `FxSlotState { momentary, latched, variation: 1|2|3|4, rejected, arming }`, `STEM_TAPE_SCHEMA_VERSION = 3`. `src/audio/fx/rack.ts` has a permanent rack input and 4 fixed variation tables. `src/machine/chordArbiter.ts` L249-268 currently maps volume ± inside the overlay to `fx.variation ±1` while an FX track is held (claims the track at L174 so no master-gain leak) — this is the system being superseded.

## B. Gesture / arbitration conflict matrix (new rows)

| Context | Volume − / + | Track 1-4 | FUNCTION + Track | Faders |
|---|---|---|---|---|
| Base | master gain | v2.6 rows | v2.6 rows | stem level |
| FX overlay open, no bank selected | short chord toggles overlay; single tap = bank-less no-op (claimed, no master gain) | tap selects bank | — | stem level |
| FX overlay, bank selected | short tap cycles algorithm (+ fwd, − back); hold = macro ±; claimed before dispatch | tap re-selects; hold = momentary | latch/unlatch | stem level |
| Heads + overlay | as above | as above | latch | head level |
| Heads + FUNCTION | — | — | — | head scrub |

Arbiter precedence insert: new "FX bank volume" claim sits above base master-volume, below the long Volume−+Volume+ system chord (duration arbitration unchanged) and below overlay toggle. Claims happen on pointer-down via `claim()`, so no command is emitted then rolled back.

## C. Workstream 1 — true multi-fader

`src/device/useDeviceSurface.ts` replaces the singleton with:
```ts
interface FaderDragSession { pointerId: number; faderIndex: 0|1|2|3; startUserY: number;
  startValue: number; lastPreviewValue: number; channel: ContinuousChannel; grabOffset: number }
const pointerToDrag = new Map<number, FaderDragSession>();
const faderToPointer = new Map<number, number>();
const pendingPreviews = new Map<number, { cy: number; value: number }>();
```
- Down: reject if `faderToPointer.has(index)` (first pointer untouched); else create session, send `phase:"start"`.
- Move: look up by `e.pointerId` only; write into `pendingPreviews`.
- One shared rAF drains the whole map: all cap `cy` writes, then all `controlBus.send` previews tagged with a single `batchFrame` id so the worklet can apply them on one shared future audio frame (`workletProtocol` gains an optional `applyAtContextFrame` batch stamp for fader/level/scrub messages).
- Up: commit only that session (`dispatch faderCommit` with the exact `lastPreviewValue`). Cancel/lostpointercapture/blur: reconcile only that fader; `releaseAll` on blur iterates sessions individually.
- Modifier change mid-drag: sessions keep their claimed channel; if a session must rebase (mode switch), store `grabOffset = currentTargetValue - pointerValue` and use pickup semantics so nothing jumps.
- Faders stay out of tap/hold/chord (`isContinuousControl` unchanged); no React state touched during drag.

Desktop: `Shift+click` on a cap toggles group membership (`faderGroup: Set<index>` in a ref, mirrored to a non-drag React state for the highlight ring only). Dragging a member moves all members by the same delta, clamped per-fader without re-scaling others. Keyboard (current map uses Q/A/Space/F/-/=/1-4): add `R/F`… conflicts with F=function, so use **Y/H, U/J, I/K, O/L** (raise/lower faders 1-4), all chordable via a held-keys set driven by one rAF ramp. Diagnostics label the source as `touch-multipointer` vs `mouse-group` vs `keyboard`.

## D. Workstream 2 — tape inertia

New `src/audio/inertia.ts`: presets `tight (180/300 ms)`, `classic (300/450 ms)`, `slow (600/900 ms)`; curve = finite sampled exponential ending exactly on target (reuse `sampleCurve`/`glideCurve` style), plus `integratedDistanceInertia()` so `TapeTimeline` advances on the same curve the audio uses.
- `TapeTimeline` gains an `inertiaSegment` variant of the existing glide segment (finite duration, exact endpoint) so `positionAt` and `timeAtPosition` stay correct; seam recalculation already keys off `seamGeneration` — bump it on every inertia start/stop.
- `transport.play`: schedule sources at rate ≈0 at the shared `startAt`, then apply the wind-up curve to `playbackRate` (node engine) or send `setRate {rampFrames, curve:"inertia"}` (worklet). All linked stems get the identical `startAt` and identical curve array.
- `transport.stop`: apply wind-down curve, then a 8 ms click-free fade and `stopSources()` at the curve end; ack `completed` only when the transition is accepted/scheduled.
- Reversal: Play during wind-down and Stop during wind-up read the instantaneous rate from the timeline and start a new curve from it — no source respawn, so rapid alternation cannot duplicate sources or strand state.
- Exclusions (no inertia, instant rate): grid punch, recording onset, loop seams, relink, beat-aligned starts, internal source handoff, worklet migration — gated by an explicit `{ inertia: false }` flag on the internal start/stop path, defaulted off for those callers.
- LED arbiter gains transport phases `winding-up | playing | winding-down | stopped`.
- Worklet is the reference implementation; node fallback uses `setValueCurveAtTime` on `playbackRate` and is audited against the same thresholds — if it cannot hold them it is reported in diagnostics as degraded, not silently wrong.
- Optional motor/hiss layer: designed, default **off**, locally generated.

## E. Workstream 3 — twelve FX

Banks (track button 1-4): TONE = Filter / Isolator / Dirt-Crusher; MOTION = Tempo Echo / Pitch Echo / Granular Scatter; SPACE = Reverb / Shimmer / Spectral Freeze; RHYTHM = Beat Repeat / Rhythmic Gate / Pump. No Tape Stop. Tape Brake only as an optional swap for Pump if requested.

State (`src/machine/stemPerformance.ts`, schema → 4):
```ts
interface FxBankState { selectedAlgorithm: 0|1|2; macroAmount: number; momentary: boolean; latched: boolean;
  rejected: string | null; arming: boolean }
interface StemFxState { banks: [FxBankState,FxBankState,FxBankState,FxBankState]; selectedBank: 0|1|2|3|null }
```
Migration from v3: `filter/echo/reverb/beatRepeat` → bank N algorithm 0, `latched` preserved, old `variation 1..4` mapped to that algorithm's macro default (variation semantics retire; the value is kept in `legacyVariation` for one version so nothing is silently lost). `momentary`, `fxOverlay`, `selectedBank` never persist. New algorithms get safe default macros.

Commands: `fx.bank.select`, `fx.algorithm.cycle {dir}`, `fx.macro {dir|value}`, existing `fx.momentary.start/end`, `fx.latch` retargeted to bank. `fx.variation` is removed from the arbiter and map after migration — no two competing selection systems.

Routing: one rack per stem, unchanged permanent input. Banks are four serial stages between rack input and `faderGain`; each stage is a stable dry/wet crossfade node pair, algorithms lazily constructed on first use and swapped through a `FILTER_FADE_S` complementary-gain crossfade (correlated) so switching is click-free. Heads mixed output enters the same rack input through the existing handoff envelope, so all twelve process Heads audio; Beat Repeat / Scatter / Freeze read from their own bounded ring buffers and never touch head pointers. Freeze captures at the activation frame. Tails persist across overlay close and Heads exit.

Heavy processors (Freeze, Scatter, Shimmer) load individually; a rejection sets `bank.rejected` and leaves every other bank running.

## F. LED / UI
Inside the existing arbiter only. Track LEDs = selected bank + momentary/latched; side LEDs = bank activity, temporarily overridden for ~800 ms during cycling to show 1/2/3 lit. Web overlay label shows bank, algorithm, `n/3`, macro. Recording/error/safety keep priority.

## G. Diagnostics
Pointer→fader table, live pointer count, pending vs committed values, batch frame id, cancel/reconcile log, desktop group membership, inertia phase + instantaneous rate + preset + integrated position error, per-stem selected bank/algorithm/macro for all 16 banks, active instances + lazy state, crossfade events, processor memory, Heads+FX routing state, rejected effects, suppressed base commands, mapping JSON export with all twelve algorithms. Tutorial metadata authored for every new command; no guide built.

## H. Sequence and acceptance
1. **Multi-fader input** — synthetic 2/3/4-pointer drags, opposite directions, crossing paths, random release order, ownership rejection, one cancel with three alive, blur recovery, zero React rerenders during drag, committed == last preview.
2. **Multi-fader in every layer** — stem levels, head levels, simultaneous head scrub, overlay-open bare faders, pickup on modifier change.
3. **Desktop group + keyboard** — offsets preserved, clamping isolated, chorded keys, diagnostics distinguish source.
4. **Tape inertia (worklet)** — shared start frame, drift within existing tolerance, integrated position matches curve, play-during-stop, stop-during-start, rapid alternation, rates above/below 1.0, no discontinuity above threshold, no inertia on grid/record/seams.
5. **Node-fallback inertia audit** — pass or documented degradation.
6. **FX state + mapping migration** — cycle order 1→2→3→1 and 1→3→2→1, no master-volume leak, latch/momentary, v3→v4 project migration, persistence across save/reload and song switch.
7. **Twelve DSP algorithms** — measurable wet/dry delta each, click-free swaps, 4 banks at once per stem, Heads-output processing, pointer immutability for Repeat/Scatter/Freeze, individual heavy-effect rejection, Node↔Worklet parity.
8. **Regression** — Phase 5/6 suites green, 37/37 v2.6 map satisfied, TypeScript clean, zero console errors, zero user-audio network requests.

Real-device checklist: iPhone + iPad four-finger fader drags, Heads scrub with two fingers, inertia audibility at 44.1/48 kHz, Freeze/Scatter CPU under load, memory headroom with 4 stems + 4 active banks; emulator runs labelled as emulation.

## I. Risks
Worklet message volume from 4 simultaneous faders (mitigated by one batched frame); FFT Freeze memory on iOS (bounded, rejectable); serial 4-bank chain latency and gain staging (per-bank compensation); inertia interacting with existing seam scheduling (seamGeneration bump on every curve); migration losing variation nuance (legacy field retained).

## J. Blocking questions
1. Inertia default preset — Classic (300/450 ms) unless you say otherwise.
2. Macro-hold on Volume ±: should macro changes be per-algorithm or per-bank-shared?
3. Should the desktop keyboard fader keys (Y/H, U/J, I/K, O/L) be confirmed, or do you want a different cluster?
4. Tape Brake substitution for Pump — keep Pump as specified?
