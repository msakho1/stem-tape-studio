# Per-track reverse — the specification

Frozen from the product owner's own words. Where this document and an
implementation disagree, this document is right.

## The one rule everything else follows from

> **Reverse changes the direction of that track's head. It never changes its
> position.**

Not a temporary effect that rejoins the song. A real reversal of one track's
tape head.

## Behaviour

**Gesture: FUNCTION + double-tap a TRACK button. The same gesture exits.**
Not FUNCTION + PLAY — `st_loop.h` already records that an earlier revision
invented that binding and it was never authorised. Extends the existing
click-vs-double-click arbitration; no second gesture detector.

> CORRECTED, on the product owner's instruction, from an earlier revision of
> this document that said "double-tap a TRACK button" with no modifier.
> FUNCTION is held. The modifier matters: an unmodified Track double-tap is
> already spoken for by the track controls, and reverse must not be reachable
> by accident on an instrument where a mis-hit is heard immediately.

Engage and disengage are the SAME gesture on the SAME track. There is no
separate exit: FUNCTION + double-tap track 2 reverses track 2, and FUNCTION +
double-tap track 2 again returns it to forward, from wherever it has drifted to.

**Drift is the point.** All four tracks at 1:00, reverse track 2 for ten
seconds:

```
tracks 1, 3, 4   ->  1:10
track 2          ->  0:50
```

Turning reverse off leaves track 2 playing **forward from 0:50**. It is never
jumped back into sync.

**One track at a time.**

- FUNCTION + double-tap the same reversed track → reverse OFF, continue
  **forward from its exact current position**.
- Track 2 reversed, FUNCTION + double-tap track 4 → track 2 resumes forward from
  wherever it is, track 4 begins reversing from wherever *it* is. Neither
  position moves.

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

### One thing that does NOT already exist, found while reading the render loop

`stem_render_run()` touches stored bytes through exactly three
`st11_sector_decode_frame()` calls, which is why the format swap is contained.
But at a **variable rate** it interpolates between the frame behind the cursor
and the frame at it, and it keeps that previous frame in a single shared
`s_rs_prev` / `s_rs_prev_valid` pair plus one shared `cur` cursor. That is
correct exactly while all four stems advance together.

A reversed stem does not. It has its own cursor, its own direction, and its own
"frame behind" — which, travelling backward, is the frame at a *higher* index.
So **`s_rs_prev`, `s_rs_prev_valid`, `cur` and `frac` all become per-stem** in
step 4, and "the frame behind the cursor" has to be read in the direction that
stem is actually moving.

This is cheap — four small scalars, not buffers — but it is invisible until you
look, and it is the kind of thing that otherwise surfaces as "reverse sounds
subtly wrong only when the pitch rocker is off centre". Recorded here so step 4
starts with it rather than discovering it.

**DONE**, as step 4 part 1, with the decoded audio bit-identical
(`0xe9650dda`) over the whole recorded song.

### The larger half, found by reading the block rather than estimating it

```
main.c:1677   static st_stream_t g_stem_stream;     <- ONE playhead
```

Four heads means four of these. The struct makes that cheap — the geometry
above the line (`song_start_block`, `song_block_count`, `frames`,
`sector_count`, `loop_enabled`) is identical for every stem, and only four
fields below it are mutable (`state`, `song_frame`, `ready_sector`,
`underrun_count`). Four streams is on the order of 128 bytes, not a redesign.

What is *not* cheap is `looper_audio_block()`'s PASS C, which is written
around that one position. Per block iteration it currently:

1. derives the needed sector from `g_stem_stream.song_frame`;
2. acquires that one sector for all four stems, all-or-none;
3. publishes one `requested_sector` to all four mailboxes;
4. declares underrun if `ready_sector != needed`;
5. derives `fis` and `run` from the single position;
6. bounds `out_n`, renders, advances the one stream;
7. runs the loop-wrap backstop against the one position.

Steps 1–5 and 6–7 all become per-stem. Three consequences worth naming before
writing any of it:

- **`run` is per-stem, and `out_n` is bounded by the minimum.** Each head has
  its own frames-remaining inside its own resident group. The block can only
  render as many output frames as the *worst-supplied* stem can feed, or the
  reversed stem starves while the other three run on.
- **Acquire stops being all-or-none across stems.** It has to stay
  all-or-none *per stem*: a stem whose group is not resident is the one that
  underruns, and today's shared check would silence all four for it.
- **The transport clock is not "stem 0".** Loop window, seam, beat phase and
  the published song frame belong to the *transport*, and a reversed head must
  not drag the song's clock backwards with it. The transport is whichever head
  is still going forward — there are always at least three, since only one
  track reverses at a time.

The storage side needs nothing: the mailbox, the active-slot array and the
read-ahead ring are already per-stem, and `st_pl_plan_batch()` already takes
per-stem heads *and* per-stem directions and already costs the same read
whichever way each is going. That was designed in when the format was.

Two further prerequisites, both small:

- `src/st_scrub.c` joins the build — but only in the commit that first calls
  it. It is deliberately unlinked until then; the link-closure gate lists it
  among the files present-but-unlinked, and adding dead weight to the image is
  what the symbol gate's own comments argue against.
- The FUNCTION + double-tap TRACK binding extends the existing click/double-click
  arbitration. No second gesture detector.

## Order of work

1. ~~Wire the v1.2 planar read path.~~ **DONE** — four per-stem group rings,
   RAM-neutral, output hash unchanged at `0xe9650dda`.
2. ~~Companion uploads v1.2 planar songs.~~ **DONE, both sides verified.** The
   firmware's commit check dispatches on the format version the record
   declares; the companion emits planar groups; and the two implementations
   produce a byte-identical 352,256-byte image for the reference song
   (`efd80d52…`), straddle included. They share no code.
3. **Verify ordinary four-stem playback is still perfect. UNBLOCKED — this is
   the next thing to actually do.** Half is already banked: CI proves the
   decoded audio is bit-identical to v1.1 over the whole recorded song. What
   it cannot prove is the part that matters most — that the SP-1 keeps up at
   92% streamer busy on real hardware. Procedure and pass/fail criteria:
   `stem-tape-playback-physical-test.md`, "v1.2 song-planar" section.

   Flash `bin=108928B sha256=114bc1b5…` (commit `0a73716`, build **st40**).
4. Build one-track-at-a-time reverse.
   - **part 1 DONE** — the resampler's carried state is per-stem, audio
     bit-identical at `0xe9650dda`.
   - **part 2a DONE** — `st_stream_t` has a direction. Turning it moves no
     playhead, invalidates no residency, stops and restarts nothing; reaching
     frame 0 clamps and never wraps to the end.
   - **part 2b DONE** — four playheads, and PASS C reworked around them:
     per-head residency, per-head direction-aware prefetch, `run` as the
     minimum across heads, a transport clock that is always a forward head, and
     a per-head per-direction loop wrap. The backward loop wrap and the clamp
     at the start of the song landed with it.
   - part 3 — the FUNCTION + double-tap gesture and `st_scrub`'s signed-rate
     crossing.
5. Flash; physical test.
6. Instrument and measure the REAL implementation. `sil=` (frames actually
   silenced) and `rduswin=` (worst fetch since the last print) are in the
   ordinary `STEMIO` diagnostic for exactly this.

## Acceptance

**The real feature running on the SP-1 without audible degradation.**

Measurement serves diagnosis; it is not the gate. The synthetic load-profile
harness is explicitly NOT a precondition for building this — enough
investigation has been done to justify building the real thing, and step 6
measures what was actually built rather than a simulation of it.
