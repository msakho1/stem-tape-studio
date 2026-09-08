# A reversed stem that reaches the front of the song (st64)

**The invariant.** A head in `ST_STREAM_START_OF_SONG` is terminal for source
consumption. It consumes no source frames, contributes silence, bounds no source
run, and does not trip the whole-mix underrun guard merely because its
`song_frame` happens to equal the transport's. MASTER and the other three
forward stems continue normally.

---

## 1. What happened on hardware

Reverse a stem, let it travel all the way back to frame 0, and the device
entered a badly degraded state: heavy crackling, a dramatic BPM slowdown,
controls largely unresponsive — no pause, no reverse release — and the only
apparent escape was a power cycle. Slow Playback, notably, *still* responded.

## 2. The mechanism, in one arithmetic step

The per-head source-bound loop excluded a head only on `!resident[sk]`. It never
looked at state. A reversed head parked at frame 0 is still sitting on a
validated sector 0, so it was treated as an active reader:

```
fis = 0  ->  rk = fis + 1 = 1      (the `rk > pos_k + 1` clamp is 1 > 1,
                                     so it does not catch this)
         ->  run = min(everything, 1) = 1
```

`st_rs_out_frames()` floors at 1, so one source frame is one output frame, and a
256-frame block took **256 full passes of the run loop** — four pin lookups,
four mailbox acquires, four request publishes, four divisions, the whole bounds
computation, the seam checks and a `stem_render_run()` call, *each pass* — where
ordinary playback takes one or two.

Everything else follows from that.

| symptom | mechanism |
|---|---|
| crackling | two contributors. (a) The parked head rendered `fis = 0` on all 256 passes: **frame 0 emitted 48,000 times a second**, a constant, not audio. (b) Real I2S and sector underruns from the CPU cost below. |
| BPM / transport slowdown | the read path is CPU-bound, and the audio thread is priority 0 (`ST_PRIO_AUDIO < ST_PRIO_STREAMER`, `< ST_PRIO_MAIN`). Its only yield is `k_mem_slab_alloc(&tx_slab, …, K_FOREVER)`; once behind realtime the DMA has drained the 10 slabs, so the alloc **stops blocking** and the thread never yields. The streamer starves, the forward heads miss residency, and the co-location guard then freezes *every* head for whole blocks. `g_stem_song_frame_pub` is the transport head's position and `st_beat_phase` derives from it, so the song clock and the beat audibly stop advancing. |
| no pause | `play_tap` is produced on `play_edge_up` (`st_ctl.c`), an edge computed from state sampled once per outer control pass. With MAIN starved, a press and release both falling between two passes produce **no edge at all**. |
| no reverse release | reverse is a **double-tap**, `ST_CTL_REVERSE_DBLTAP_MS = 450`. Two press/release pairs inside 450 ms cannot be reconstructed by a loop whose passes are hundreds of milliseconds apart. |
| Slow Playback still worked | it needs neither. Entry to the FUNCTION branch is a **level** test (`pwr_pressed()`), not an edge — it only has to be true on *some* pass. Once inside, that branch owns the button and runs its **own** `k_msleep(25)` sampling loop, measuring the hold with `k_uptime_get()` deltas rather than pass-to-pass edges. And its effect is consumed before the run loop, by `st_pitch_slow_glide()` once per block. |

The device was never hard-frozen. It was a priority-0 thread that had stopped
yielding often enough, and the two controls the player needed were the two that
depend on fine temporal resolution.

## 3. What was NOT happening

Worth stating, because the symptoms suggested it and it was checked:

* `st_stream_advance_frames()` returns `TICK_NOT_PLAYING` for `START_OF_SONG`
  before touching anything — no re-seek, no repeated entry/exit, no repeated
  resampler drop, no underrun accounting.
* The reversed prefetch is clamped (`ahead = needed >= 3 ? needed − 3 : 0`), so
  it never requests a sector before the song.
* **No eMMC read storm.** The streamer reads only when
  `st_stem_mbox_producer_next_run()` finds slots needing fill; with sectors
  0,1,2 already resident it returns false and issues nothing.
* Residency was never invalidated while parked.

The head was already boring. The caller was not.

## 4. The fix

Two conditions.

**A — a parked head is not reading.** One line, at the single place residency is
computed, because *three* consumers depend on it and all three must exclude it:

```c
resident[sk] = (g_stem_stream[sk].ready_sector == needed[sk]) &&
               (g_stem_stream[sk].state != ST_STREAM_START_OF_SONG);
```

* the **source-bound loop** — otherwise `run` is pinned to 1;
* the **buffer choice** — `!resident ? silent_group : real` is the *only* thing
  that makes a head silent, so a head excluded from the bound but left resident
  would render its real sector-0 bytes at the *transport's* offset, which is
  worse than the defect;
* the **underrun accounting** — a head that is not reading has not starved.

**B — and therefore the co-location guard must exclude it too.** This is a
consequence of A, not an independent idea: before A a parked head was still
resident, so `!resident[sk]` was false and the guard could never fire for it. A
makes it non-resident, which newly exposes it to the guard's *position* test.

```c
if (!resident[sk] &&
    g_stem_stream[sk].state != ST_STREAM_START_OF_SONG &&
    g_stem_stream[sk].song_frame == tr->song_frame) {
```

Explicit state, not positional coincidence: the position test cannot tell "the
same frame because we are synchronised" from "the same frame because I ran out
of tape where you happen to be."

The guard is **narrowed, not disarmed** — a genuinely starved *forward* head
co-located with MASTER still stalls the mix, which is the case it exists for.

## 5. Provenance

`git log -S` on both anchors returns a single commit: **`53bda3e` — "st52 /
reverse step 4 part 2b: four playheads, and PASS C around them."** The defect
has been present since per-track reverse landed, ten builds before st61. st62
touched the advance loop and the EOF apply; st63 touched only the
reverse-*consume* block. Neither goes near the bounds loop or the guard.

It was simply never reached: it needs a reversed stem to travel all the way back
to frame 0.

## 6. The sibling defect, NOT fixed here

`END_OF_SONG` has the mirror shape and the same root — terminal-state heads are
not excluded from the bounds computation. A *resident forward* head parked at
`song_frame == frames` computes `left_in_song = frames − frames = 0`, so
`rk = 0` and `run = 0`, and the run loop `break`s: the block is abandoned as
silence, every block. Different pathology (whole-mix silence rather than a CPU
storm), same cause.

It is deliberately left alone in st64 so this checkpoint's hardware validation
stays clean, and it is reachable through the still-open song-end path in
`docs/stem-tape-song-end-contract.md` §7. Fix them together when either is next
touched.

## 7. What proves it

| | |
|---|---|
| `tests/test_reverse_start_gate.c` | 10 cases, 513 checks. Real `st_stem_stream.c`; a model of `stem_audio_block()`. Parks every stem in turn and watches eight whole blocks: run not pinned, ≤2 run passes, full-block silence with zero real frames, MASTER and the other three advancing a full block each, the song clock keeping up, no guard firings. Plus reverse-at-the-very-start, the constructed position coincidence, the guard still working for a starved forward lane, release and switch from parked, and the loop regression wall. |
| `stemtape_player_reverse_start_mutations.py` | 5 mutations, all red. S-1/S-2/S-3 break the three consumers *independently*; S-4 reverts the single production line; S-5 mutates the production backward clamp. |
| wiring check **H-1/H-2** | reads production `main.c` directly. Both mutation-proven. |

**What none of it proves.** The gate's transport is a model — `main.c` cannot be
linked on the host — so it can agree with a `main.c` that has drifted from it.
H-1/H-2 mitigate that but are textual. And nothing here measures actual audio
thread cost on the device; the CPU argument is read from source, not profiled.

### Hardware acceptance

> PLAY → reverse one stem until it reaches frame 0 → the other stems continue
> normally, BPM stays stable, no crackling, controls stay responsive → release
> reverse → the parked stem rejoins MASTER cleanly. Then the same starting very
> near the beginning of the song.
