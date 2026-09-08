# The resampler cursors, and the pitch crackle they are being moved for

**Commit 1 of two. This one changes nothing.** It moves the four per-stem
resampler cursors out of `stem_render_run()` and into `src/st_rs_cursor.h` so
that a host gate can link the *real* implementation. No arithmetic, ordering,
clamp, fraction, decode dispatch or writeback rule was altered.

---

## 1. Why the diagnosis changed

The hardware symptom is: **1×, +0.5, +1.0 and +1.5 semitones are clean; from
about +2.0 upward it crackles.** A serial capture taken at 1× reported 785
underrun episodes and 3.13 s of emitted silence, but the diagnostic block is
DTR-gated (`controls_diag()`, `main.c:8574`) — a device with no console attached
runs none of it — and the file's own comment at `main.c:8557` warns that "a
diagnostic that manufactures its own underruns poisons the evidence." That
capture is therefore **not** evidence about normal playback and is not used
here.

What the ear data alone settles:

* **The "cost step at the first click off unity" hypothesis is falsified.**
  `unity` requires `rate_q16 == ST_RS_ONE` (`main.c:3021`), and +0.5 semitone is
  `rate_q16 = 67456`. The full interpolating path is already engaged at the
  first click, which is clean. The fixed cost of leaving unity is affordable.
* **Whatever binds must scale with rate, and be near its limit.** Of the three
  candidates in the source, two are excluded: read duty goes from 64% at 1× to
  71.5% at +2 st (251 reads per 2 s window × `ST_LAT_READ_TYP_US = 5073`, one
  read being `ST_LAT_REFILL_GROUPS × 2048 = 6144` B), which is not saturation;
  and run-loop passes per block barely move, because `out_n` is capped by
  `BLK_FRAMES − f` either way.
* **A sharp knee inside the top 1.5 clicks of a 6-click range is a threshold,
  not a utilisation curve.** The remaining candidate is the audio block's own
  5333 µs deadline, and overrunning it triggers the feedback loop already
  documented in `stem-tape-reverse-start-of-song.md` §2: the priority-0 audio
  thread's only yield is `k_mem_slab_alloc(&tx_slab, …, K_FOREVER)`, which stops
  blocking once the DMA has drained the slabs, so the streamer starves.

**This is inferred from source plus the ear, not measured.** No clean `aus`
reading exists at any pitch. Commit 2 is what makes it falsifiable: if removing
~40% of the interpolating loop's scalar work does not move the crackle
threshold at all, this model is wrong and the cause is storage-side.

## 2. What Commit 2 will do, and why the boundary had to come first

The plan is to collapse the four identical cursor walks into one when the heads
are genuinely together. That is ~46% of the per-output-frame scalar operations
in the interpolating body — index/clamp, the index-equality test, the validity
test, the fraction accumulate and the walk's scalar bookkeeping. It removes
**none** of the blend, the `prev` copies or the decodes.

Its entire safety claim is *the output does not change*. Every other audio gate
in this repository states in its own report that it checks a **model** of
`stem_audio_block()` and can therefore agree with a `main.c` that has drifted
from it. A model cannot prove bit-identity of code it is not. So the cursors
move into a header first, in a commit proven to change nothing, and Commit 2 is
diffed against that.

**The predicate Commit 2 will need is stronger than the existing `together`.**
`together` checks only that no lane is reversed and that all four
`frame_in_group` agree (`main.c:3011`). It does not check the carried fraction,
the run bound, or interpolator validity — and the st63 reverse-release path
(`main.c:3839`, `3852`) clears **one** lane's `s_stem_rate_frac[j]` and
`s_rs_prev_valid[j]` while seeking it to MASTER, so one block later all four are
co-located and forward, `together` is true, and one lane carries fraction 0
while three carry a fraction. Mutations X-2, X-5, X-10 and X-11 pin that as RED
now, before the code that could get it wrong exists.

## 3. The extraction

Five `static inline __attribute__((always_inline))` functions, called in a fixed
order once per output frame:

| | |
|---|---|
| `st_rs_cursor_index()` | clamp each cursor, form each read index |
| `st_rs_cursor_fetch()` | decode the frame at the cursors (shared or four-index) |
| `st_rs_cursor_prime()` | first frame after a drop: `prev := nxt`, per lane |
| *(the blend stays in `main.c`)* | it reads `prev`, `nxt` and `frac[]` |
| `st_rs_cursor_advance()` | walk each cursor by one output frame's source |
| `st_rs_cursor_finish()` | publish `frac_io[]` and `used_out[]`, once per run |

The blend is **not** extracted: it is mix work, every stem's samples genuinely
differ, and there is nothing shareable about it in any future variant either.

`s_rs_prev` and `s_rs_prev_valid` stay exactly where they were, in `main.c`, and
are reached through parameters. No storage moved, no new statics.

`always_inline` is load-bearing, not decoration. `stem_render_run()` carries
`__attribute__((optimize("O2"), noinline, noclone))` and runs 48,000 times a
second on the priority-0 thread; a non-inlined helper would be a call per output
frame, which is the exact cost st45 and st57 removed from this loop. CI asserts
the absence of any out-of-line `st_rs_cursor_*` symbol from the linked ELF.

## 4. What proves it

| | |
|---|---|
| `tests/test_rs_cursor_gate.c` | Two arms over identical inputs: a **frozen verbatim transcription** of the pre-extraction `main.c` text, and `st_rs_cursor.h` itself, linked. 200,000 randomized runs plus the st63 reverse-release state at five rates, a reversed lane at both group edges, and the rate extremes. Compares every blended sample, every read index, every carried fraction, every consumed source count and the whole interpolator state. 6 cases, 47 checks, 0 failures. |
| pinned non-unity hash | **`0xbd69ac9c`** — a scripted six-rate playback through the cursor and blend path, produced identically by both arms. This is the number Commit 2 must reproduce. |
| `stemtape_player_rs_cursor_mutations.py` | 11 mutations, all red, **all against the production header**. |
| wiring check **I-1/I-2** | reads production `main.c`: all five helpers called, in contract order, and no second copy of the walk left behind. Both mutation-proven. |
| `test_stem_playback_gate.c` | unity playback hash `0x2a737e00`, unchanged. |

**What none of it proves.** The gate covers the cursor and blend path, not the
whole streaming chain — `0x2a737e00` is what covers that, at unity. It says
nothing about whether the crackle model in §1 is right. And CI green is not
hardware.

### A finding, recorded rather than dropped

`used_out[sp] = (cur[sp] > src_avail[sp]) ? src_avail[sp] : cur[sp]` is **dead
defensive code**. `cur` starts at 0, is only ever incremented by one, and
`st_rs_cursor_advance()` clamps it to exactly `src_avail[sp]` the moment it
reaches it, so `cur > src_avail` is structurally unreachable. The first draft of
mutation X-10 removed that bound and survived, correctly. The line stays — this
is firmware with no MMU and it costs one compare per run — but nothing claims to
test it, and X-10 now mutates something observable.

## 5. Status

Commit 1 is CI-proven and **hardware-unproven**, which for a no-op extraction
means: the audio is proven identical on the host, and nothing has been flashed.
**st64 `c0bac9681efdabbf10e296fde36e9e602269a545` remains the hardware-confirmed
rollback baseline.** Commit 2 is not started and requires explicit approval.
