# Per-track reverse — the specification

Frozen from the product owner's own words. Where this document and an
implementation disagree, this document is right.

## The one rule everything else follows from

> **Reverse changes the direction of that track's head. It never changes its
> position.**

Not a temporary effect that rejoins the song. A real reversal of one track's
tape head.

## Behaviour

**Gesture: double-tap a TRACK button.** Not FUNCTION + PLAY — `st_loop.h`
already records that an earlier revision invented that binding and it was never
authorised. Extends the existing click-vs-double-click arbitration; no second
gesture detector.

**Drift is the point.** All four tracks at 1:00, reverse track 2 for ten
seconds:

```
tracks 1, 3, 4   ->  1:10
track 2          ->  0:50
```

Turning reverse off leaves track 2 playing **forward from 0:50**. It is never
jumped back into sync.

**One track at a time.**

- Double-tap the same reversed track → reverse OFF, continue **forward from its
  exact current position**.
- Track 2 reversed, double-tap track 4 → track 2 resumes forward from wherever
  it is, track 4 begins reversing from wherever *it* is. Neither position moves.

**Start of song.** A reversed track that reaches the absolute beginning
**stops/clamps there**. It does not wrap to the end. The other tracks keep
playing. Turning reverse off then lets it move forward from the beginning again.

**Inside a loop.** A reversed track stays inside the loop window: travelling
backward, on reaching `loop_start` it **wraps to `loop_end`** and continues
backward.

**Speed.** Varispeed and slow mode set the MAGNITUDE; reverse sets the SIGN.
The existing varispeed system is used, not duplicated.

**Never, on engage or disengage:** restart, seek, pause, silence, or playhead
reset.

## The direction change

Reuse `st_scrub`'s existing signed-rate behaviour, which already decelerates
through exactly zero before crossing direction:

```
forward  ->  quickly decelerate through zero  ->  backward
```

and the same in reverse. A digital sign-flip discontinuity is wrong, and so is a
long tape-stop: **very quick and responsive.**

## What the existing code already provides

Checked, rather than assumed — the brief said to inspect before writing DSP:

| need | already exists |
|---|---|
| signed rate, negative = reverse, decelerating through zero | `st_scrub.h` |
| move a playhead forward or backward with no stop and no restart | `st_stream_seek()` |
| the gesture binding | named in `st_loop.h`; `main.c:8552` already anticipates a double-tap |
| storage that makes a diverging track affordable | `st_planar` (built, proven) |

## Order of work

1. Wire the v1.2 planar read path.
2. Companion uploads v1.2 planar songs.
3. **Verify ordinary four-stem playback is still perfect.**
4. Build one-track-at-a-time reverse.
5. Flash; physical test.
6. Instrument and measure the REAL implementation.

## Acceptance

**The real feature running on the SP-1 without audible degradation.**

Measurement serves diagnosis; it is not the gate. The synthetic load-profile
harness is explicitly NOT a precondition for building this — enough
investigation has been done to justify building the real thing, and step 6
measures what was actually built rather than a simulation of it.
