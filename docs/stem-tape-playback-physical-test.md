# Stem Tape stored-song playback — physical acceptance test

This document exists because host tests are not acceptance evidence for
physical playback. Everything in CI proves the algorithm; only the SP-1
proves the product. What follows is the procedure that decides it, the
numbers the firmware now reports so the decision is made on measurement
rather than impression, and an explicit statement of what is still unknown
until the test is run.

## 1. What changed, and why it should matter

The reported symptom was audio that sounded crushed and played back
dramatically slower than the source, with the BPM well off, from a
correctly uploaded 48 kHz song whose metadata and duration were right.

A slow song is the important clue, and it points away from the codec and
towards throughput. `st_stream_advance_frame()` freezes the playhead on an
underrun instead of skipping frames. That is the correct choice for a
performance instrument — dropping frames would tear the groove — but it
means a sustained throughput deficit does not present as dropouts. It
presents as **time-stretch**: the song takes longer than it should, and the
tempo reads low, in direct proportion to how starved the reader is.

So the question was never "is the decoder wrong". It was "why is the
streamer not being fed fast enough", and the answer was that it was not
being *scheduled* fast enough.

### The measured model

From a real boot capture, an uncontended sector read (8192 bytes, 16 eMMC
blocks) takes **5073 µs**:

| phase | time | note |
|---|---|---|
| SPIM3 DMA | 2056 µs | 514 B × 16 at 32 MHz — a hard floor on this bus |
| start-bit hunt | ~1763 µs | bit-banged; partly the card making us wait |
| CRC + copy-out | ~1104 µs | pure CPU |
| CMD18/CMD12 | ~150 µs | handshake |

That is **1,614,823 B/s** with the CPU otherwise idle — 140% of what
playback needs. The hardware was never too slow.

Under load the same read took ~12,500 µs and the rate collapsed to
**643,898 B/s**, about 56% of requirement, with one underrun episode per
sector read. The capture also showed `aud=51% str=40%`.

That yields a model worth stating plainly, because it is what makes the
fix predictable rather than hopeful:

```
read time = uncontended read time ÷ streamer's share of wall clock
```

Checked against the capture: 5073 ÷ 0.42 = 12,079 µs predicted, versus
11,762–13,399 µs observed — within about 3%. The model reproduces the
measurement, so it can be used to say what has to change.

### What the model demands

| target | max read time per sector | streamer share of wall clock required |
|---|---|---|
| 1,152,000 B/s (break-even) | 7.111 ms | **71.3%** |
| 1,400,000 B/s | 5.851 ms | **86.7%** |

The streamer's share is whatever the audio thread, MIDI thread and main
loop leave behind. The audio thread was taking 51% of wall clock, and
almost all of that was inherited Tape Looper work that cannot affect a
stem-rendered block: recorder failsafe scans, take/arm/punch machinery
gated on track states this firmware cannot reach, classic resampling, and
two full mixing passes over buffers holding no content.

The fix is therefore to stop executing that work at all — not to buffer
around it. Read-ahead buys *time*; it never buys *bytes per second*.

## 2. What the firmware now does

The Stem Tape dispatch is the **first** statement in `looper_audio_block()`
and returns before any classic engine code runs. It is not a late branch
after the expensive passes.

```
audio_thread()                        I2S block loop, DWT-timed
└─ looper_audio_block(blk)            dispatch is its FIRST statement
   ├─ st_stream_play()                idempotent transport sync
   ├─ master_vol_ramp()
   ├─ stem_audio_block()              noinline/noclone
   │  ├─ st_stream_required_sector()
   │  ├─ st_stem_mbox_try_acquire()   / st_stem_mbox_release()
   │  ├─ st_stream_sector_ready()
   │  ├─ st_stem_mbox_set_requested_sector()
   │  ├─ st_stem_mix_prepare()        once per block, not per frame
   │  ├─ stem_render_run()            -O2, whole-run inner loop
   │  │  ├─ st11_sector_decode_frame()
   │  │  └─ st_stem_mix_frame_prepared()
   │  └─ st_stream_advance_frames()   run form, not per-frame
   ├─ audio_block_epilogue()
   └─ return                          ← before the first classic statement
└─ i2s_write()
```

Two CI gates hold this in place, and both fail closed:

- **Source level** — the wiring check parses `main.c` and asserts the
  dispatch returns before the first classic-engine statement, by line
  number, not by proximity.
- **Link level** — the symbol gate requires `stem_audio_block` and
  `stem_render_run` by exact name in the ELF. These are reachable only
  through the bypassing dispatch, so their presence is evidence of the
  fast path itself rather than of a wrapper both engines share.

Supporting changes: the mixer moved from int64 to int32 (2,000,000
randomised frames bit-identical); eMMC CRC verification was folded inside
the DMA window and the copy-out eliminated; rendering became run-based
rather than per-frame (whole-song equivalence host-tested, hash
`0xe9650dda`); the read-ahead ring was primed and validated in full at boot
rather than sector 0 only; and 147,456 bytes of unreachable classic buffers
were reclaimed.

## 3. Reading the diagnostics

Connect to the USB serial console. Every diagnostic line carries `b=<tag>`
and the boot banner prints `STEMTAPE BUILD <tag>`. **If that tag does not
match the build you flashed, the flash did not take and nothing in the
capture says anything about this firmware.** That check exists because a
capture was once analysed at length as evidence about a fix while the
device was still running firmware from before it.

The line that decides the test:

```
STEMRT aus=<us> budget=<%> need=1152000Bps have=<Bps> margin=<%> ahead=11sec/77916us und=<n>
```

| field | meaning | pass condition |
|---|---|---|
| `aus` | worst `looper_audio_block()` execution time this session | — |
| `budget` | `aus` as a percentage of the 5333 µs a 256-frame block at 48 kHz is allowed | must be well under 100% |
| `need` | 48000 × 24 B — an identity, not a target | — |
| `have` | measured sustained read rate | — |
| `margin` | `have ÷ need` | **must exceed 100%** |
| `ahead` | read-ahead the ring holds when full | context only |
| `und` | steady-state underrun episodes | **must be 0** |

`margin` is the term that has to clear 100%. `ahead` only says how long a
transient may last before it becomes audible; a deeper ring is not proof of
throughput and must never be read as such.

Supporting lines: `STEMIO` (bytes read, per-read timing, underruns,
corrupt sectors), `STEMRD` (where a read's time went — hunt, DMA, CRC),
and `CPU` (per-thread wall-clock share, which is the input the model above
turns into a predicted read time).

## 4. The test

1. Flash the candidate BIN.
2. Confirm the boot banner's build tag matches what was flashed.
3. Select the already-uploaded full song. Press Play.
4. Let it play to the end without touching anything. Time it.
5. Play it again and, during playback, exercise all four faders, tap-to-mute
   and hold-to-solo on each stem, and master volume. Watch the track LEDs.
6. Capture the serial output for the whole run.

### Pass conditions

- Audio is continuous, at the original pitch, BPM and duration. A
  225-second song takes approximately 225 seconds.
- No recurring crushed or gated artefacts, in particular none recurring at
  sector boundaries (one sector is 7.083 ms of audio).
- All four faders, tap-to-mute, hold-to-solo, master volume and the track
  LEDs remain usable and responsive throughout.
- `STEMRT` reports `und=0` in steady state and `margin` above 100%.

**Zero steady-state underrun episodes is the requirement.** A non-zero
`und=` invalidates the run regardless of every other number on the line.

### If it fails

Read the numbers before changing anything.

- `und` > 0, `margin` < 100% → still starved. `CPU` says which thread is
  taking the wall clock and `STEMRD` says which read phase is costing it.
  The model in §1 converts those two directly into a predicted read time,
  so the next step is arithmetic rather than guesswork.
- `budget` at or above 100% → the audio thread alone cannot meet the block
  deadline and no amount of read-ahead will help.
- `und` = 0, `margin` > 100%, but the audio still sounds wrong → this is
  the clipping case, not the starvation case. Four stems summed at unity
  saturate into int16, and the fix is measurement-led. **Do not reduce gain
  blindly**; that is a separate step, deliberately not taken before the
  underrun question has been answered on real hardware.

## 5. What is still unknown

Honest limits of what has been established:

- **No audible verification has been performed.** There is no eMMC, I2S or
  codec hardware in CI, and no human has listened to this build. Every CI
  step that touches stored playback carries that distinction explicitly.
- **`aus` and the achieved `margin` have not been measured on this
  firmware.** The dead work removed from the audio path is known exactly;
  what that converts to in wall-clock share is what the physical run
  measures. The prediction is that the audio thread's share drops far
  enough for the streamer to clear the 71.3% it needs, but a prediction is
  not a measurement and is not presented as one.
- **Whether the summed four-stem mix clips** on this particular uploaded
  song is unknown and deliberately untested until underruns are ruled out,
  so the two failure modes are never confused.

Recording, overdub and UAC2 remain absent. Zero occurrences of any UAC2
symbol, `g_record_arm` or overdub state exist in compiled code, and the
symbol gate proves their definitions are absent from the linked ELF rather
than merely unreachable.
