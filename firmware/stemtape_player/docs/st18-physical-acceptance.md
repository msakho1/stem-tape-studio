# st18 — physical acceptance checklist

Build tag `st18`. This is the list the BIN has to pass on a real SP-1 before
anything in this repository may call the loop seamless. **CI green is not on
this list.** CI proves the waveform is continuous in a host model and that the
LED decision assigns particular levels; it cannot hear anything.

Confirm the device is running this build first — the streamer prints
`STEMTAPE BUILD st18` as its first line, and the `LOOPER b=st18 …` diagnostic
line carries it too.

## The eight

| # | What to do | What must happen |
|---|---|---|
| 1 | Hold PLAY past the loop threshold while a song runs | Loop starts with **no audible blip or outage** at the entry |
| 2 | Let it run for at least a minute | **Every** wrap is inaudible — no click, no "seek" sound, no gap |
| 3 | Release PLAY (unlatched) | **No audible outage** at the release |
| 4 | Listen to what follows the release | The looped passage is **not** heard again |
| 5 | Keep listening | Playback continues from `loop_end`, forward, in the song |
| 6 | Throughout | No slowdown, no crushed or distorted audio, no dropouts |
| 7 | While the loop runs | **T1 → T2 → T3 → T4 chase**, bright, one LED at a time, in tempo, T1 on the downbeat |
| 8 | Exercise the rest | Track chords, FUNCTION latch, PLAY+VOL length change, upload, and ordinary playback all still work |

Any single failure means the build is not accepted. Report which number failed
and what it sounded like — "wrap still clicks" and "wrap now sounds like a
short duck" are different findings and lead to different fixes.

## What changed in st18, so you know what you are listening for

Every loop transition now ducks the output gain to zero over 128 frames
(2.67 ms), performs the seek at zero gain, and ramps back over another 128
frames. Total 5.33 ms — exactly one output block.

So the honest expectation is **not** "identical to no transition at all". It
is that the seam sounds like a very short dip rather than a click. If you hear
a click, the duck is not covering it. If you hear an obvious *fade*, the duck
is too long and should be argued down from `ST_SEAM_FRAMES`.

This is the base SP-1's own BOUNDARY FADE technique (`firmware/src/main.c`
line 1962), at the length that file uses specifically for a loop seam.

## Item 7 in detail

The chase derives from the real loop playback frame and the song's `bpm_q8`
and `downbeat_frame` — there is no free-running LED timer. Consequences worth
knowing before calling it broken:

* A song uploaded **without a tempo** gets no chase at all. That is deliberate:
  a fabricated tempo is worse than none.
* A loop whose length is not a whole number of beats will make the chase jump
  at the wrap. That is the loop, faithfully displayed, not a bug.
* A **latched** loop keeps S1 solid as well as running the chase.
* With **no** loop running, the Track row behaves exactly as it did before —
  beat pulse with the bar accent. If that changed, it is a regression.

## Read the diagnostic line too

With a serial terminal attached (DTR asserted), two things are worth
capturing during the session:

* **`STACK unused aud=… str=… midi=… main=…`** — new in this build. Bytes of
  each thread stack never touched. Capture it after a session that has looped,
  changed loop length, held chords, transferred a song and played one to the
  end. The smallest figure across all of that is the only honest headroom, and
  no stack size may be changed until it exists. A number near the full size
  means the thread has barely run, not that it is safe.
* **`stv=[…]`, `gl=`, `iwf=`, `aus=`** on the `EMMC48` line — starvation
  counts, stored glitches, I2S failures and worst audio-block execution time.
  All should stay at zero (`aus` well under 5333 µs) through the loop test. A
  seam problem and a starvation problem sound similar and these tell them
  apart: the seam is a discontinuity with **zero** underruns.

## What is NOT in this build

The RAM rearchitecture. Free RAM is 67,618 bytes of 262,144; the 96 KiB target
needs the unified sector cache, which is not written yet. Nothing in st18
changed a pool size, so any change in playback reliability from st17 would be
a surprise and worth reporting.
