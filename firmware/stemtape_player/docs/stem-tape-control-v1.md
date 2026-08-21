# Stem Tape control path (build `st17`)

## The gestures

| Gesture | Result |
|---|---|
| Hold one Track | that stem alone, while held |
| Hold two or three Tracks | exactly those stems, while held |
| Release all Tracks | all four stems back, within ~16 ms |
| Tap PLAY | play / stop |
| Hold PLAY ≥ 450 ms | loop the bar starting **where PLAY went down** |
| PLAY held + VOL − / + | loop division: 1 bar → ½ → ¼ → ⅛ bar (clamps) |
| FUNCTION, during a momentary loop | latch it; release both, it keeps running |
| FUNCTION held + VOL − / + | division, once latched |
| PLAY, while latched | exit |

Track presses never toggle or latch mute. FUNCTION never unlatches — PLAY
does, and both that press and its release are consumed so neither also stops
the transport.

**Playback is forward only.** There is no reverse gesture in this phase.
Reverse belongs to a later one and its gesture will be a Track-button
double-tap.

## Where the loop starts and where it ends

The window is **half-open**: `[start, end)`. That convention is used by the
control thread, the audio path, the streamer and the tests, with no exceptions.

- **Start** is the frame the audio thread was playing when PLAY went **down**,
  captured on that edge and held as a candidate. At the threshold the firmware
  **seeks back** to it, so the first repetition is audible at the threshold —
  not a whole window later.
- **End** is `start + one division`, clamped to the song.
- **Exit** — unlatched release or latched PLAY press — resumes at `end`: the
  first frame after the section that was looping. Nothing already heard is
  replayed. If the window was clamped to the song end there is
  no later frame, so that one case resumes at 0, exactly as ordinary playback
  does when it reaches the end.

Cold boot is always **one bar**, derived from the selected song's real
`bpm_q8`, sample rate and 4 beats/bar. A division you choose sticks for later
loops in the same session; a power cycle puts it back to one bar.

## Timing

Control loop cadence ~8 ms.

| | Budget |
|---|---|
| Track / chord response | 3 agreeing reads, ~24 ms |
| Track release | 2 confirmed idle reads, ~16 ms |
| PLAY tap | ~24 ms after release |
| Loop entry | 450 ms threshold + PLAY debounce, **~474 ms worst case** |
| Entry / exit seek visible to audio | next block boundary, ≤ 5.33 ms |

There is exactly one PLAY-hold owner. The inherited Tape Looper's 400 ms
hold-to-restart cannot run while a Stem Tape song is selected.

## Why the sectors are pinned

A one-bar window at 93.71 BPM is 122,932 frames — 361 sectors. The read-ahead
ring holds 12, and sector *s* and sector *s+12* share a slot, so a window wider
than the ring cannot keep both of its ends resident. Two ends have to be
reachable without warning:

- **entry** — the seek back to `start`, and every wrap;
- **exit** — `end`, reachable on the pass right after entry.

Both are fetched into pinned buffers outside the ring, requested the instant
the gesture **arms** — a full hold before the loop can start. Six sector reads
take ~30 ms against a 450 ms hold.

**Depth is 3 sectors each, and three is the minimum.** Both regions are based
exactly on their seek target's sector, so the worst case is the target frame
sitting on the *last* frame of its sector, leaving `(n-1)×340 + 1` frames of
pinned audio:

| depth | pinned runway | verdict |
|---|---|---|
| 1 | 1 frame = 0.02 ms | misses |
| 2 | 341 frames = 7.10 ms | misses |
| 3 | 681 frames = 14.19 ms | safe, 4.04 ms margin |

against a **10.15 ms** worst-case wait: the streamer may be mid-sector-read
when the seek lands (5.073 ms to finish) plus 5.073 ms to read ours. Past that
first handover the producer outruns the consumer (5.073 ms per sector read vs
7.083 ms of audio per sector), so nothing beyond three buys anything.
6 × 8192 = **49,152 bytes**.

`tests/test_loop_playback_gate.c` measures all of this rather than asserting
it: it injects the full worst-case producer stall at the moment of the exit
and shows depth 3 emits **zero** silent frames, while the same case at depth 2
emits 427.

## The ladder

One ADC sample per pass, one classifier (`st_ladder.c`), one published Track
mask that the mixer and the LEDs both consume. `decode_tracks()` is handed
`TRK_NONE` while a stem song is selected, so it cannot fire a gesture
underneath the dispatcher.

Bands come from `docs/ladder-measured.json` — a capture from the physical
device. All fifteen masks resolve; tightest clearance is 58 counts against
6 counts of measured jitter. A reading between bands **holds** the settled
state rather than proposing a new one, so the failure mode is a missing chord,
never a wrong one.

There is no slew guard. st15 had one — `if (|raw - last_raw| > 40) return
settled` — and it made no progress, so playback-time coupling into the shared
button rail latched the mask at zero. On a switched resistor ladder the node
steps in microseconds, so it had no sweep to reject in the first place.

## LEDs

Unchanged from the physically-verified build. The repaired Track mask feeds the
existing display; no new LED state was added.
