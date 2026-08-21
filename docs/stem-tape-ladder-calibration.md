# Stem Tape — Track/PLAY ladder calibration (build `st16-cal`)

## Why this exists

st15's chord bands were **not measured**. They were produced by fitting a
resistor model to the five documented single-button centres and validating it
against exactly one measured chord — Track 1 + Track 4, the bootloader DFU
failsafe band at raw 1280–1390. The host tests then fed those *predicted*
centres back into a decoder built from the *same* prediction, which proves the
code is self-consistent and says nothing about a physical device.

On real hardware the chords did not fire. This build replaces the model with
measurements from your SP-1.

## What the build does and does not do

- It does **not** change audio, mixing, streaming, transfer, or the LED owner.
- It adds one ADC read and one `printk` per control pass, **only while the
  transport is stopped** (`g_playing == 0`). The instant playback starts the
  capture returns immediately and the image behaves like production.
- Logging is rate-limited: a line only when the raw value moves more than
  ±3 counts, at least 40 ms apart, plus a 1 s heartbeat so a steady hold still
  confirms itself. Nothing logs at audio rate.

The "only while stopped" rule is load-bearing, not caution: `ladder_read()`
blocks the control thread, and the control thread preempts the eMMC streamer.
Over-sampling this rail is what starved the card once already.

## Line format

```
CAL t=12345 raw= 213 stable= 213 n=  7 fn=0 cls=BAND          band=1000 settled=1000
```

| Field | Meaning |
|---|---|
| `t` | milliseconds since boot |
| `raw` | the instantaneous ADC reading — **this is where you read the jitter** |
| `stable` | the settled value (3 consecutive reads within ±3) — **record this as the centre** |
| `n` | how many consecutive reads have agreed; a big `n` means a clean hold |
| `fn` | FUNCTION (a separate GPIO) 1 = held, 0 = not |
| `cls` | `IDLE`, `BAND` (st15's model claims it), `UNKNOWN` (model has no band), `PLAY-OR-ABOVE` |
| `band` | which mask the *current model* thinks it is, or `----` |
| `settled` | what the production settling decoder currently outputs |

`cls=UNKNOWN` on a combination you are definitely holding is the expected
result for most chords — that is the model being wrong, which is the point.

## Setup

1. Flash `stemtape_player.bin` from the `st16-cal` build.
2. Open the USB serial console (CDC ACM) and **assert DTR** — the SP-1 prints
   nothing until a monitor attaches. Any of:
   - `screen /dev/tty.usbmodem* 115200`
   - `minicom -D /dev/ttyACM0`
   - `picocom /dev/ttyACM0`
3. Confirm the banner reads `STEMTAPE BUILD st16-cal`. If it says anything
   else you are running the wrong image — stop.
4. Note the `V11 lib: TEMPO ...` line and **paste it back to me**. It reports
   `bpm_q8`, `sample_rate`, `downbeat_frame` and the derived
   `frames_per_beat`. If it says `TEMPO INVALID`, the loop cannot start on
   this song and that alone explains part of what you saw.
5. **Leave the transport STOPPED.** Do not press PLAY to start the song. If
   the song is playing, capture stops.

## Captures

Hold each item **steady for about 2 seconds** so `n` climbs and `stable`
settles, then release fully to idle before the next one. Releasing to idle
between captures matters — it gives every reading a clean starting point.

Capture in this order:

| # | Hold | Note |
|---|---|---|
| 1 | nothing (idle) | baseline |
| 2 | PLAY | |
| 3 | Track 1 | |
| 4 | Track 2 | |
| 5 | Track 3 | |
| 6 | Track 4 | |
| 7 | T1 + T2 | |
| 8 | T1 + T3 | |
| 9 | T1 + T4 | the one band that was already measured — a cross-check |
| 10 | T2 + T3 | |
| 11 | T2 + T4 | |
| 12 | T3 + T4 | expected to collide with PLAY |
| 13 | T1 + T2 + T3 | |
| 14 | T1 + T2 + T4 | |
| 15 | T1 + T3 + T4 | expected to collide with PLAY |
| 16 | T2 + T3 + T4 | expected to collide with PLAY |
| 17 | all four | collision evidence only |

Rows 12, 15, 16 and 17 are expected to be unusable. Capture them anyway —
"we measured it and it really does collide" is a much stronger statement than
"the model said it would".

Then two extra passes that matter for the ± tolerance:

18. **Press-and-release T1 ten times** in a row, at a normal performance
    speed. This shows how much the settled value moves between presses, which
    is what sets the band width.
19. Repeat 18 for one chord — **T1 + T2** is a good choice.

## What to send back

The raw console text is ideal — just paste the whole log. I will extract the
minimum/maximum/jitter per combination myself. If you would rather summarise,
per row I need: **the `stable` value, and the lowest and highest `raw` you saw
while holding it.**

If a combination is physically awkward to hold, say so and skip it — I will
mark that mask unsupported rather than guess a band for it.

## What happens next

From your numbers I will derive non-overlapping production bands with real
tolerance, mark any mask that is electrically indistinguishable from PLAY or
from another control, commit the measured data as a machine-readable table so
the thresholds are auditable, and remove this capture from the production
build. The final image gets the next production tag — never `st16-cal`.
