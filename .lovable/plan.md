# Correct true scratch ownership and hand-velocity control

## Diagnosis to preserve

- **Master audio ownership:** node and worklet paths feed the same per-stem `stemGate`. Migration crossfades the worklet in while fading only the newest tracked node source over 12 ms. It does not explicitly prove or enforce that every old node source is silent after takeover, so stale seam/loop sources can remain connected and create doubled audio.
- **Rocker input:** the current SVG uses two adjacent half-rectangles. Pointer capture is requested on the pressed half and should survive crossing, but this is still represented as two controls rather than one rocker owner. The current velocity formula is absolute grab displacement, `clamp((grabY-currentY)/70) * 3.5`, so a stationary off-center finger leaves velocity latched. That is shuttle behavior, not hand-on-record scratch.
- **Isolated stem behavior:** FUNCTION + fader still routes to `laneFaderScrub`, which schedules short `AudioBufferSourceNode` grains from repeated positional previews. It is granular scrub, not an independent signed read head. This pass will prevent confusing that legacy path with completed scratch; implementing isolated signed-head scratch remains separate from the narrow master correction.

## Implementation

1. **Make master takeover exclusive**
   - At the shared worklet takeover frame, fade and stop every live node source for each migrated stem, not only the newest source.
   - Shorten the takeover seam to the minimum anti-click interval needed for a correlated same-position handoff.
   - Add engine diagnostics/tests proving that after takeover each loaded lane has zero live node sources, worklet gain is unity, and the worklet is the sole transport source.

2. **Replace displacement velocity with hand velocity**
   - Track timestamped SVG-space pointer samples.
   - Compute `tapeVelocity = clamp(-Δy/Δt × sensitivity, -Vmax, +Vmax)` with centralized sensitivity, deadband, stop timeout, max velocity, and short ramp tunables.
   - Command zero immediately on grab, decay to zero after a short no-motion timeout, reverse through zero from pointer motion, and clear all timers on pointer/function release, cancellation, blur, or visibility loss.
   - Keep the visual rocker continuous and bounded while making sound velocity depend on motion rather than absolute position.

3. **Use one rocker drag owner**
   - Preserve ordinary upper/lower rocker taps, but make FUNCTION + rocker acquire one pointer-captured drag session for the whole physical rocker region.
   - Verify capture and continuous movement across the center using actual rendered SVG coordinates at the current mobile viewport.

4. **Regression proof**
   - Add pure mapping tests for slow/fast/reversed motion, deadband, clamping, and hand-stop zero.
   - Add engine ownership tests for all live node voices being retired at master takeover.
   - Run focused tests, full Vitest, TypeScript, production build, and a browser pointer-coordinate audit.

## Explicit non-goals

- No loop, FX, LED, or unrelated UI changes.
- No return to granular scrub for master scratch.
- No claim that isolated stem scratch is complete; its current `laneFaderScrub` grain path will be documented as legacy until a true per-lane signed head replaces it.
