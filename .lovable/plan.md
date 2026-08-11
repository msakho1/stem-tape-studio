# Heads mode: why "nothing happened", and the fix

## What I verified in the running app

I drove the real control surface in a headless browser at your viewport (420 px, touch): held FUNCTION, tapped PLAY three times, released.

Reducer state before: `headsMode: false`
Reducer state after: `headsMode: true`
Command tail emitted:

```text
rollback
rate.set        play.snap     (the ×2 step, rolled back)
rollback
heads.enter     play.heads
```

So the gesture, the deferred multi-tap arbitration and the reducer are all working. The failure is downstream of the reducer, in three places:

1. **The engine can silently refuse.** `HeadLaneEngine.enter()` returns `heads rejected — no decoded lane to read` when no stem is decoded, and `audio not unlocked` when the AudioContext is still suspended. Nothing reverts the surface after a refusal: `applyHeadsFeedback` exists in `src/machine/surface.ts` but is never called from `useDeviceSurface`. Result: the surface believes heads are on, the audio engine knows they are off.
2. **There is no visible heads state on mobile.** The only heads-aware UI element in `src/routes/index.tsx` is `KeyboardPanel`, wrapped in `hidden lg:block`. Below the `lg` breakpoint nothing on screen changes at all — no banner, no head strip, no rejection notice.
3. **Even a successful entry is silent by design.** On entry the four heads are parked, not playing: "nothing playing until a Track is held or latched". So a correct entry with stems loaded also produces no sound until you hold or triple-tap a Track button.

## The fix

### 1. Make the engine's verdict authoritative

- Subscribe to engine acks in `useDeviceSurface` and route `heads.enter` / `heads.exit` results into `applyHeadsFeedback`. A rejected entry flips `headsMode` back to `false` and surfaces the reason.
- Show the rejection text ("no decoded lane to read", "audio not unlocked") in the existing "what just happened" narration line so a refusal is never silent.

### 2. Auto-satisfy the preconditions instead of refusing

- If the AudioContext is suspended when `heads.enter` arrives, unlock it inside the same gesture call stack (the pattern already used for global scrub) and retry once before rejecting.
- Keep the "no decoded lane" refusal, but state it plainly: *heads need at least one loaded stem — load a song or the demo kit first.*

### 3. Give heads mode a visible presence at every width

- A heads banner at the top of the device column, visible on mobile and desktop: `HEADS · four independent lane heads` plus the one-line control legend (tap = mute, hold = solo audition, ×3 = latch, FN + fader = scrub, FN + ×2 = reverse, FN + Track + VOL ± = resize).
- A four-head strip showing, per head: playing / latched / muted / reversed, loop bars, and position in seconds — read from the head-lane engine so it reflects audio truth, not reducer intent.
- Exiting heads mode removes both, restoring the current layout exactly.

### 4. Confirm the entry gesture landed

- Flash the heads banner and pulse the FUNCTION LED (`breathe`, already specified in the LED frame) on entry so the triple-tap is acknowledged even when no stem is loaded yet.

## Technical notes

Files touched: `src/device/useDeviceSurface.ts` (ack → `applyHeadsFeedback` wiring, unlock-and-retry), `src/audio/engine.ts` / `src/audio/headLanes.ts` (retry after unlock, unchanged rejection semantics otherwise), `src/routes/index.tsx` (heads banner + head strip, no breakpoint gating), and a small new heads status component. No changes to the gesture engine or the reducer's heads table — both are proven correct by the run above.

Verification: replay the same headless run at 420 px and at desktop width, with and without stems loaded, and assert (a) rejection reverts `headsMode` and shows the reason, (b) successful entry shows the banner and strip, (c) holding Track 1 makes head 1 audible while the transport stays paused.
