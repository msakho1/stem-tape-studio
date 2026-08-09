# Track Performance Controls, Automatic Grid, Recording Removal

## 1. Verified current state

Everything below was read in this session.

**Track buttons (`src/machine/surface.ts:447-540`, `:612-700`)** — the bare-Track table is recording-first:

- `count === 2` on a loaded/muted track emits `track.delete` (`:485-492`) → recoverable trash.
- `count === 1` on loaded/muted emits `track.mute` / `track.unmute` (`:529-532`).
- `holdStart` level `hold` with `!inputEnabled` emits `rec.requestInput` (`:645+`); with input enabled it emits `rec.arm`.
- Heads mode claims Track gestures first (`:451-464`): double-tap = `heads.reverse`, tap = `heads.mute`; hold = `heads.source` / `heads.print` (`:620-640`).
- FN + track = bank jump / next song (`:466-477`).
- FX overlay Track rows live in `src/machine/stemTapeV1Map.ts:213-260` (momentary FX, variation stepping, FN-latch) and already declare `suppresses: ["track.mute","track.unmute"]`.
- `stem.solo` today is a PLAY + Track chord (`stemTapeV1Map.ts:92-108`, `surface.ts:890-894`) — a *latching* solo, handled by `src/machine/chordArbiter.ts`.

**Gesture engine (`src/input/gestures.ts`)** — taps fire **optimistically** on release with `count = 1`, then re-fire with `count = 2` inside `multiTapGapMs: 300` (`:104-113`, `DEFAULT_TIMINGS:63-71`). `holdMs: 450`. This directly violates the requirement "the first tap of a double-tap must never mute the stem" and "arbitration before dispatch".

**Grid (`src/audio/grid.ts`)** — scalar only: `GridState { bpm, anchorFrame, sampleRate, source: "none"|"tapped"|"rounded"|"manual", intervals }`. BPM is null until 3+ FN taps (`tapGrid`). Persisted as `control.grid = { bpm, source }` (`src/audio/store.ts` `StoredProject`). No beat/bar frame map, no analysis. Consumers: `rocker.speed` (`surface.ts:~570`), `decidePunch`/`decidePunchOut` (recording only), FX tempo (`src/audio/fx/banks.ts:38`).

**Loops** — per-track normalized `window {start,end,shift,reverse}` + `chopDiv`, shared across tracks via link mask; `setLinked` (`engine.ts:1384`), link mask in `engine.ts:3028`. Dual-source equal-power relocation already exists (`engine.ts:820`, `:926`, `:1139`) with per-track `generation` counters (`engine.ts:143`, `:679`) — this is the machinery the bar loop will reuse. Audibility funnels through `applyAudibility` (`engine.ts:1230-1245`).

**Recording** — `src/machine/recordingState.ts`, `src/audio/input/*`, `src/audio/InputPanel.tsx` (mounted from `src/device/SystemPage.tsx:340`), `public/input-capture-processor.js`, `src/workers/recordingWorker.ts`, `src/workers/takePageWorker.ts`. **Critical shared dependency:** `src/audio/export/performanceRecorder.ts:30,43,47` loads `input-capture-processor.js` **and** `recordingWorker.ts`; `src/workers/wavWorker.ts:9` imports `chunkStore`. So the capture worklet, recording worker and chunk store are *retained infrastructure*, not deletable.

## 2. Conflicts found (must be resolved, not disabled)

| # | Conflict | Resolution |
|---|---|---|
| C1 | Optimistic `count=1` tap → mute fires before a double-tap is known | Add Track-button arbitration: defer tap dispatch by `trackTapWindowMs` (200 ms, tunable 180–220) |
| C2 | Double-tap currently = `track.delete` | Delete removed from the surface entirely; double-tap = bar-loop capture |
| C3 | Hold currently = `rec.arm` / `rec.requestInput` | Hold = momentary audition; all `rec.*` emissions deleted from arbitration |
| C4 | PLAY+Track latching solo vs new momentary solo | Chord arbiter keeps priority: with PLAY held, the chord wins and no audition starts. Momentary audition only when PLAY is not pressed |
| C5 | Heads mode claims Track tap/double-tap/hold | Unchanged — Heads/PRINT precedence preserved; new bindings apply only when `!headsMode && !fxOverlay` |
| C6 | FX overlay Track rows | Unchanged, already suppress `track.mute` |
| C7 | FN + Track = bank/song | Unchanged, FN checked before the new layer |
| C8 | Bar loop vs linked-track window targeting | Bar loop is a **separate per-stem layer**, never routed through link mask |
| C9 | Grid persisted as scalar `{bpm, source}` | Schema bumped additively to carry the frame map; old projects re-analyzed on load |

Full combination table (single/double/hold, 2–3–4 holds, muted stem held, persistent solo + audition, pointercancel, double-tap while another held, capture while stopped/cued/playing/scrubbing, tap during scheduled activation, simultaneous releases, rate change during loop, song switch, FX, Heads, node vs worklet, keyboard vs multitouch, legacy projects with takes) will be delivered as a filled precedence/ack table in the implementation's first commit and mirrored as a unit-test matrix.

## 3. State machines

**Track gesture arbiter** (new `src/input/trackGestures.ts`, fed raw down/up/cancel per control):
`idle → pressed → (t ≥ holdMs) auditioning → released` or `pressed → awaitingSecond (≤200 ms) → tapConfirmed | doubleConfirmed`.
Crossing `holdMs` cancels the pending tap window. Cancel/blur/lostpointercapture route to the same `release(reason)` path; a normal release is never `cancel`.

**Momentary audition** (`src/machine/audition.ts`): `inactive → active(set)`; snapshot of `{mutes[], solos[]}` taken on first hold; `held` is a Set; releasing a member updates the group; empty set → restore snapshot exactly and clear. Never persisted.

**Bar loop, per stem**: `inactive → capturing → scheduled → looping → releasing → inactive`.

## 4. Commands

```
track.mute { track }            track.unmute { track }
audition.start { held:number[], snapshotId }
audition.update { held:number[] }
audition.end { snapshotId }
barloop.capture { track, bar, startFrame, endFrame, activateAtFrame }
barloop.release { track, releaseAtFrame }
grid.installed { gridId, source:"analysis" } | grid.replaced { gridId }
grid.correct  { anchorFrame, bpm, source:"manual" }
```
Rejections ack `rejected` with a reason, mutate nothing (audio, LEDs, mute/solo/loop untouched).

## 5. Automatic grid

New `src/audio/gridAnalysis.ts` + `src/workers/gridWorker.ts`. Runs after ingest/restore, before performance-ready.

- Read the **already-decoded** channel data in sequential bounded chunks (no second decode, no second full copy) — transferable Float32 slices, ~4 s hop, peak working set reported into `src/audio/memory.ts`.
- Per chunk: spectral-flux onset envelope (downsampled ~344 Hz), summed across stems with transient-rich stems weighted higher.
- Tempo: autocorrelation / comb-filter over the summed envelope, 60–200 BPM, with explicit half/double-time disambiguation by comparing beat-level onset energy at f, 2f, f/2.
- Phase: cross-correlate the beat pulse train; downbeat by 4-beat energy accumulation (beatsPerBar 4 default).
- Output a **frame map**, not a scalar:

```ts
interface SongGrid {
  gridId: string; sampleRate: number; beatsPerBar: number;
  originFrame: number;
  beats: Int32Array;            // beat frames @ sampleRate
  bars: Int32Array;             // downbeat frames
  segments: { startFrame: number; bpm: number }[];
  normalized: Float64Array;     // portable 0..1 positions for SR restoration
  sourceHashes: string[];       // stem contentHash list
}
```
Restoration converts through `normalized` × new frame length, guaranteeing ≤1 converted frame at 44.1↔48 kHz. Grid recomputes when `sourceHashes` change; restores directly when they match. No confidence score, no "AI" labelling anywhere.

`src/audio/grid.ts` keeps `tapGrid` as the *optional* FN ×4 correction path, writing `source:"manual"` segments over the analyzed grid.

## 6. Bar loop signal flow

Capture (double-tap accepted at frame F): find bar `b` containing F from `bars`, capture `[bars[b], bars[b+1])` in **source frames** for that stem, schedule activation at `bars[b+1]` (shared context frame across simultaneous captures). Reuse `engine.ts` dual-source crossfade (`:820`, `:926`) with a new per-track `barLoop` slot, its own generation counter.

Two pointers are maintained per looping stem: the audible loop read pointer, and a hidden song pointer advanced by the same integrated rate curve as the transport (so varispeed and reverse behave tape-style). Release schedules at the next bar boundary, crossfades from loop grain to a fresh source started at the hidden pointer, then tears down the loop path so exactly one playback path remains. Beyond source end → silence, no wrap or stretch. Fader gain is never automated; FX rack position unchanged. The worklet kernel (`public/tape-processor.js`) gets the same bar-loop window + hidden-pointer fields so node and worklet behave identically.

LEDs: new `barloop` tier in the `LED_PRIORITY` table in `surface.ts` (between `soloed` and `unlinked`), patterns `scheduled` = fast breathe, `looping` = double-blink, `releasing` = chase. Written only by the arbiter.

## 7. Recording removal — audit and migration

**Delete from the visible product (immediately):** `src/audio/InputPanel.tsx` and its mount in `SystemPage.tsx:340`; all `rec.*` emissions in `surface.ts`; `rec.*` rows in `stemTapeV1Map.ts:284-340`; recording LED tiers in `surface.ts` (`REC_LED_TIERS` usage); `armed`/`recording`/`overdubbing`/`printing`-except-PRINT branches of the Track table.

**Must temporarily remain (backward compatibility / read-only recovery):** `src/audio/input/takes.ts`, `src/audio/input/takePages.ts`, `src/workers/takePageWorker.ts`, take manifest fields in the project schema — kept so existing stored takes stay exportable during migration.

**Shared with retained features — never delete:** `public/input-capture-processor.js` and `src/workers/recordingWorker.ts` (used by `performanceRecorder.ts:30,43,47`), `src/audio/input/chunkStore.ts` (used by `wavWorker.ts:9` and `takePageWorker.ts`), `src/audio/wav.ts`, `src/audio/export/*`, `src/audio/print.ts`.

**Removable once compatibility is proven:** `src/machine/recordingState.ts`, `src/audio/input/recorder.ts`, `src/audio/input/latency.ts`, `src/audio/__tests__/phase6.test.ts` recording sections.

Migration is additive and non-destructive: no stem blob, project row or take chunk is deleted; new projects simply never create recording state. A one-time "legacy takes" export affordance in Projects lets a user pull old takes to WAV before cleanup.

## 8. Implementation order

1. Recording removal from arbitration + UI (no `rec.*` reachable, no `getUserMedia` path).
2. Track gesture arbiter + momentary audition + mute/unmute commands.
3. Grid analysis worker, `SongGrid` schema, persistence + restore.
4. Bar loop: engine (node), worklet parity, LED tier, diagnostics.
5. Rewire tempo FX (Echo, Pump, Beat Repeat, chop) to the frame map.
6. Test matrix + browser/device evidence.

## 9. Evidence to be produced

- Unit: gesture arbitration matrix (single/double/hold, all 2/3/4 hold combinations, random release order, cancel/blur), audition snapshot restore equality, grid fixtures (straight 4/4, sparse intro, half/double ambiguity, drift, 44.1/48 kHz, unequal lengths, save/reload, stem replacement) asserted in frames with stated tolerances, bar-loop capture/activation/release-rejoin (≤2 frames), 100 capture/release cycles with zero source leak, node/worklet parity, anti-click peak-discontinuity and RMS seam thresholds.
- Playwright (Chromium + WebKit): zero `getUserMedia` calls, no permission prompt, no recording controls in DOM, no `rec.*` in the command stream, PRINT + master performance recording + WAV export still succeed, no user audio in network requests, zero console errors.
- Real device: iPhone/iPad 2/3/4-finger Track holds, loop capture/release listening test, background-interruption recovery.
- Reported numbers: measured single-tap latency and the final chosen `trackTapWindowMs` / `holdMs`.

## 10. Open decisions

1. **PLAY + Track latching solo** — keep it alongside momentary audition, or retire it now that hold gives momentary solo?
2. **Track double-tap in Heads mode** currently toggles head reverse. Keep Heads precedence (my assumption), or should bar-loop capture win there too?
3. **Legacy takes** — is a one-time export affordance wanted, or should stored takes simply remain untouched and invisible?
