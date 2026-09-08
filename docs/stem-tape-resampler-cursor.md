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

## 5. What it cost

| | st64 `c0bac96` | this extraction | delta |
|---|---|---|---|
| FLASH (bin) | 116,864 B | 116,848 B | **−16 B** |
| RAM (linker) | 203,486 B | 203,486 B | **0** |
| RAM free | 58,658 B | 58,658 B | 0 |
| new statics | — | none | 0 |
| `st_rs_cursor_*` out-of-line copies in the image | — | **none** | fully inlined |
| `stem_render_run()` frame | *not measured* | `sub sp, #188` | **unknown** |

The image is **not** byte-identical: 116,848 vs 116,864, and a different SHA-256
(`bde7f62c…210e` vs `abac6ef8…4ad9`). Sixteen bytes is four Thumb-2
instructions. The build tag was deliberately left at st64 so that a byte-identical
image would have been available as proof; it was not available, so the proof of
no behavioural change rests entirely on the differential gate in §4 — which is
where it should rest, since that gate compares audio rather than encoding.

**The stack delta is not measured, and is reported as unknown rather than
zero.** The measuring step did not exist at st64, so there is no like-for-like
prior number; 188 B is the extracted build's frame, and it becomes the baseline
Commit 2 is compared against. The *expectation* is no growth — the helpers take
existing arrays by pointer, add no locals, and are proven fully inlined by the
symbol assertion, so they reuse the caller's frame — but that is reasoning, not
measurement, and it is written down as such.

---

# Commit 2 — the shared lane (st65)

**What it does.** Off unity, ordinary four-forward playback ran four
bit-for-bit identical cursor walks and threw three away. When the four cursors
are *provably* one cursor, the bookkeeping is now done once on lane 0 and
broadcast. Every per-stem audio operation — the blend, `prev`, the decodes —
stays per stem, untouched.

## 7. The predicate, and why `together` is not enough

```c
const bool locked = !unity &&
		     st_rs_cursor_locked(frame_in_group, dirs, src_avail,
					  frac, s_rs_prev_valid);
```

Five conditions, all required: every direction forward, every
`frame_in_group[]` equal, every `src_avail[]` equal, every carried fraction
equal, every `prev_valid[]` equal. Any one failing runs the existing
independent path verbatim — the predicate *is* the fallback, so there is no
second divergence detector to keep in sync.

`together` (`main.c:3011`) checks only the first two. The other three can differ
with `together` true:

| | how it happens | what sharing would do |
|---|---|---|
| `frac[]` | st63's reverse release clears **one** lane's `s_stem_rate_frac[j]` and seeks it to MASTER (`main.c:3839`, `3852`). One block later all four are co-located and forward. | renders that stem at the other three's sub-sample phase — audible only off centre pitch, only just after a reverse release |
| `src_avail[]` | a starved lane borrows the transport's offset and a forward direction (`main.c:4489`) | shares lane 0's run bound, so a short lane is advanced past source it never had resident |
| `prev_valid[]` | cleared per lane at the same sites as `frac[]` | reads lane 0's flag: either leaves the rejoining lane interpolating from a position it no longer occupies, or flattens three lanes' first output frame |

Under all five, every cursor-domain quantity is identical across lanes at every
iteration by induction, so the shared form computes the *same values* — the
output is bit-identical, not merely equivalent.

## 8. The broadcast, and the two readers it exists for

`st_rs_cursor_advance()` publishes `frac[]` and `cur[]` to all four lanes as the
last thing it does. That placement is load-bearing, not tidiness. Three things
downstream read `cur[]` **per lane, after the walk**, in the same frame
iteration:

* `st_fx_process(…, heads[s_fx_target].song_frame + dirs[…] * cur[s_fx_target])` — the echo's time index
* `g_stem_zero_at[sp] = song_frame + cur[sp]` — where the dropout detector says a silence began
* `st_fx_process(…, song_frame + cur[fx_clock_stem])` — the global rack's clock

A shared walk that forgets lanes 1–3 is **inaudible** — the blend reads `prev`
and `nxt`, not `cur` — and still wrong in all three. `test_resample_lock_gate.c`
models the first two directly, and mutation L-6 removes the broadcast to prove
the gate sees it.

## 9. What proves Commit 2

| | |
|---|---|
| `test_rs_cursor_gate.c` | unchanged in shape, now with **half the 200,000-run sweep deliberately locked-shaped** (100,294 locked / 100,318 unlocked). Still compared against the frozen pre-extraction `main.c` text, so this is bit-identity of the *shared lane* against the original. 6 cases, 47 checks, 0 failures. |
| pinned non-unity hash | **`0xbd69ac9c`, unchanged** — and now produced *through* the shared lane, since the scripted playback is four-forward at every rate. |
| unity hash | `0x2a737e00`, unchanged. |
| `test_resample_lock_gate.c` | 8 cases, 165 checks, 0 failures. Soundness (five divergences, each isolated so exactly one condition differs), non-vacuity at +0.5/+1/+1.5/+2/+2.5 and −2.0, the two downstream readers, and 60,000 randomized locked runs. |
| `resample_lock_mutations.py` | **L-1…L-8, all red on the lock gate**, all against the production header, all compiling. |
| wiring **I-3** | production `main.c` computes the predicate, passes it to all five helpers, and does not derive it from `together`. Mutation-proven. |

**A finding worth recording.** L-4 (predicate degenerates to `together`) is red
on the lock gate and **green on the differential**. In the differential's
generator, a reversed or displaced lane always *also* has a different run bound,
so the surviving `src_avail` condition masks it. The lock gate catches it only
because case 2 deliberately equalises the run bound to isolate `dirs`. A
randomized sweep is not a substitute for a constructed one.

## 10. What it cost

| | Commit 1 `c410598` | st65 `dafc7d2` | delta |
|---|---|---|---|
| FLASH (bin) | 116,848 B | 117,344 B | **+496 B** |
| RAM (linker) | 203,486 B | 203,486 B | **0** |
| RAM free | 58,658 B | 58,658 B | 0 |
| new statics | none | **none** | 0 |
| `st_rs_cursor_*` out-of-line copies | none | **none** | fully inlined |
| `stem_render_run()` frame | `sub sp, #188` | `sub sp, #196` | **+8 B** |

sha256 `bde7f62c…210e` → `58931dfb…aecb`. Build tag `st64` → **`st65`**.

**The stack grew by 8 bytes, and that is reported rather than rounded to
zero.** The shared branch introduces two locals — `f0` and `c0`, the lane-0
fraction and cursor — and GCC gave them stack slots rather than keeping them in
registers across the walk. Two words out of the audio thread's 3072-byte stack
(`main.c:939`), which carries a deliberate +1 KiB margin over the historical
2048. It is not free, it is small, and the `STACK` runtime diagnostic reports
the real high-water mark on hardware if it ever matters.

FLASH +496 B is the locked branches themselves: five helpers each carrying a
second code path. That is the price of the fallback being a real path rather
than a reinterpretation of the shared one.

## 11. Status

Commit 1 is CI-proven and **hardware-unproven**, which for a no-op extraction
means: the audio is proven identical on the host, and nothing has been flashed.
**st64 `c0bac9681efdabbf10e296fde36e9e602269a545` remains the hardware-confirmed
rollback baseline.**

Commit 2 (**st65**) is CI-proven and hardware-unproven. Acceptance is by ear,
**console detached**, 1× → +0.5 → +1 → +1.5 → +2 → +2.5, and the reading is
fixed in advance so the result cannot be reinterpreted afterwards:

| result | conclusion |
|---|---|
| the whole range clean | the CPU-deadline model is confirmed and the pitch defect is closed |
| the threshold moves up but still crackles below +2.5 | partially confirmed; report the remaining gap before any further change |
| the threshold does not move at all | **the deadline model is rejected as the primary cause.** Keep the change only because it is bit-identical and cost-neutral, and move the investigation to the storage/read path |

Pitch limits are untouched. Reverse, loop, EOF, FX, power, scratch, heads,
MIDI, master-clock behaviour, storage geometry, buffer sizes and audio quality
are untouched.
