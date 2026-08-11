# Heads mode: engine-authoritative entry, live head readout

## Verified current behaviour

- `src/machine/surface.ts:470-478` flips `headsMode` optimistically and *then* emits `heads.enter`. Nothing ever reconciles it.
- `applyHeadsFeedback` exists at `src/machine/surface.ts:933` and is called from no live path — `useDeviceSurface` never sees engine acks.
- `HeadLanes.enter()` (`src/audio/headLanes.ts:113-150`) rejects with `"audio not unlocked"` when there is no context and `"heads rejected — no decoded lane to read"` when no buffer is decoded. Both surface verbatim through `engine.ack`.
- On success it parks all four lanes at `songPosition()` with `moving:false`, regardless of what was audible.
- `useAudioEngine` already exposes `acks` and already owns the async unlock-and-retry pattern (`commandTail`).
- `engine.status().heads` already carries the per-head snapshot (position, playing, latched, held, muted, reverse, loop).

## What changes

### 1. Entry becomes provisional until the engine acks

`heads.enter` is emitted with the reducer's heads state left **unchanged**. `useDeviceSurface` subscribes to acks and calls `applyHeadsFeedback`:

```text
FN + PLAY x3 -> heads.enter (provisional)
              -> engine.enterHeadsMode()
   accepted   -> applyHeadsFeedback({active:true})  -> headsMode = true
   rejected   -> headsMode stays false + reason shown
```

Same for `heads.exit`. A periodic reconcile compares `status.heads.active` with `state.headsMode` and forces the reducer to the engine's value, so the split-brain state cannot exist even transiently after an unexpected engine stop.

### 2. Automatic unlock on entry

`heads.enter` joins the existing `commandTail` unlock path: if the context is missing or suspended, resume it inside the same gesture stack, then retry `heads.enter` once. Rejection only after the retry.

### 3. Entry preserves the musical state

`HeadLanes.enter()` takes an entry descriptor from the engine: for each decoded lane, `{ audible, position }` captured **before** any lane state is touched.

- Capture `at = currentTransportPosition` first; set `lane.posS = at` for all four lanes before any anchor/moving/reverse write. No `stopLane`/reconcile recompute from stale anchors.
- Transport playing + lane audible: head starts **moving** from `at` (anchor set to `ctx.currentTime`), so the composition does not drop out.
- Transport playing + lane muted/inactive: head parked at `at`, muted.
- Transport paused: all heads parked at `at`, nothing sounds, UI still goes active.

Never reset to 0 s.

### 4. Minimal heads status layer

A compact indicator directly above the device, at every width (no `lg:` gating), no legend, no instructions:

```text
HEADS   1 ▸ 12.4   2 ■ 12.4   3 ▸ 12.4 ↺   4 ✕ 12.4
```

Per head: position (s), moving/parked, latched, reverse, muted. Values are read from `engine.status().heads`, never from reducer state. It disappears on exit.

### 5. Entry acknowledgement

On accepted entry: FUNCTION LED breathes (existing heads LED frame), a brief `HEADS` confirmation flashes, and the four indicators appear immediately — including when every head is parked.

### 6. Product-facing rejection copy

Engine detail strings are mapped at the UI boundary; raw text stays in logs.

| engine detail | shown |
| --- | --- |
| no decoded lane to read | HEADS unavailable · load a song first |
| audio not unlocked / retry failed | HEADS unavailable · audio engine locked |
| no output bus | HEADS unavailable · audio engine locked |

### 7. Track semantics stay as implemented

Tap = mute, hold = solo audition, ×3 = latch, FN + fader = scrub, FN + Track ×2 = reverse, FN + Track + VOL ± = resize. These already dispatch into `headLanes` while `heads.active`; the pass only verifies no path falls back to transport once heads are live. Multi-tap gesture arbitration is not touched.

### 8. Diagnostics

`HeadLanes.log` gains the named events `heads.enter.requested`, `heads.enter.audioUnlocked`, `heads.enter.accepted`, `heads.enter.rejected`, `heads.exit`, and per-head `position/moving/latched/reverse` samples, all readable through the existing `window.__stemTape` heads bridge.

## Acceptance run

Chromium at 420 px and desktop, scenarios A–J from the brief: no song (reject + copy, `headsMode` false), suspended context (auto-unlock, single gesture), all four stems audible (no dropout, four moving heads at the song position), stems 1+3 audible (2 and 4 parked at the same position), paused transport (parked, UI active, silent), hold Track 1, triple-tap latch, FN + fader 1 isolation, FN + Track 1 ×2 reverse with continuous position, and exit reconcile. Each asserted against `engine.status().heads`, plus an invariant check that `headsMode === status.heads.active` at every step.

## Files

`src/machine/surface.ts` (provisional entry), `src/device/useDeviceSurface.ts` (ack subscription + reconcile + rejection copy), `src/audio/useAudioEngine.ts` (unlock-and-retry for `heads.enter`), `src/audio/engine.ts` (entry descriptor), `src/audio/headLanes.ts` (position-first init, audible-lane continuation, named log events), `src/routes/index.tsx` + a small heads status component.
