# Per-track reverse playback — feasibility, and what it costs

Status: **STORAGE YES. CPU: FOUR TRACKS NO — measured, controlled, on
hardware. One and two tracks are still open.**

## The result (firmware st36, the controlled gate)

```
STEMPGATE RESULT rev=4 FAIL
STEMPGATE  baseline und=0    sectors=2824 keepup=100% worst_fetch_us=21593
STEMPGATE  test     und=1212 sectors=2445 keepup=86%  worst_fetch_us=20684
```

with `busy=83%` steady through the baseline window and `busy=99%` through the
test window.

**This one is trustworthy, and the reason is the first line.** The baseline
window ran the shipped read pattern for the same 20 seconds on the same song
and came out at zero underruns and 100% keep-up. The control was clean, so the
1212 underruns in the test window belong to the read pattern rather than to
the conditions — which is exactly the attribution the first gate could not
make.

### What the numbers say beyond pass/fail

**The failure is sustained cost, not a tail.** The worst single sector fetch
was *lower* in the test window (20684 us) than in the baseline (21593 us). The
worst case is set by preemption, not by how the sector is fetched; what kills
level 4 is paying an extra ~2 ms on *every* sector, not an occasional long one.
This is the opposite of the mechanism claimed in the retracted st33 analysis,
which built a tail-exposure story on a misread `rd_max_us`.

**The baseline's own worst fetch is 21.6 ms** — a sector holding 7.08 ms of
audio, fetched in three times that, with zero underruns. The read-ahead ring is
already absorbing multi-sector stalls in ordinary playback. That is reassuring
about the ring and sobering about the headroom.

**Where the budget goes.** Ordinary playback already sits at 83% busy while
the streamer's reads only account for ~45% of a sector period. The remaining
17 points are the entire budget for anything new.

### Projection to the untested levels — and its limits

The second sweep refit at **665 us fixed + 156.8 us per block** (the first
sweep: 625 + 159.3 — the card is reproducible). Each diverging track costs one
extra read, so one extra fixed cost:

| tracks reversed | reads | fetch us | storage duty | extra CPU points | projected busy |
|---|---|---|---|---|---|
| 0 | 1 | 3173 | 44.8% | — | 83% |
| 1 | 2 | 3838 | 54.2% | +9.4 | **92%** |
| 2 | 3 | 4503 | 63.6% | +18.8 | **102%** |
| 3 | 4 | 5168 | 73.0% | +28.2 | 111% |
| 4 | 4 | 5168 | 73.0% | +28.2 | 111% (measured: 99%, capped, keepup 86%) |

**Do not read that table as a result for levels 1 and 2.** The only measured
point is level 4, and it is *saturated* — 99% is a ceiling, so it establishes
that level 4 needs more than 100% without establishing how much more. The
slope is therefore modelled, not measured, and the projection inherits every
assumption in the model (that the extra cost is exactly the extra fixed
per-read price, and that it converts one-for-one into CPU busy). Levels 1 and 2
are one 45-second run each. Run them.

Note also that storage duty and CPU busy are different quantities: the storage
can serve level 4 at 73% duty, and the CPU still cannot afford it. The storage
sweep was never going to answer this question, which is why there are two.

## The retracted st33 run, and why it did not settle the question

```
STEMPLANAR sim=ON und=0  rd_max_us=11205 reads=13 busy=51% spare=49%
STEMPLANAR sim=ON und=32 rd_max_us=32239 reads=73 busy=99% spare=1%     <- run 1
STEMPLANAR sim=ON und=41 rd_max_us=13552 reads=30 busy=77% spare=23%    <- run 2
```

**What it does support:** under `sim=ON` the CPU saturated (99%) and the
streamer fell behind — 60 sectors fetched in a window needing ~71. That is
consistent with the planar pattern being too expensive.

**Why that is not a finding yet:**

1. **No control.** `sim=OFF` was never measured under the same conditions
   (same song, same duration, same USB console attached). Without a baseline
   the underruns cannot be attributed to the read pattern.
2. **The resume transient is inside the numbers.** `X` sets
   `g_slot_switch_req`, so leaving transfer mode RELOADS the song and the
   read-ahead ring restarts empty. Counters are cleared at arm time, before
   the `X`. The first window shows 13 sectors against ~71 needed — that is
   priming, not failure, and it is counted.
3. **Counters are cumulative, never per-window.** There is no way to tell
   whether the 32 underruns happened in the settling second or throughout.
4. **The two runs disagree.** 73 sectors/32 underruns/99% busy versus 30
   sectors/41 underruns/77% busy — more underruns from fewer sectors. That
   inconsistency is itself evidence the experiment is uncontrolled.
5. **Sustained playback was never confirmed.** The companion tool reported
   `PLAYING FOR 0:00` for the second run, meaning it never observed a PLAY
   state.

**A correction to the first analysis of this data.** `rd_max_us` times the
whole `stem_read_sector()` call — all four reads PLUS any preemption between
them — not one read. It was compared against `ST_LAT_READ_WORST_US`, a
single-read figure, and a "24x, 2x the documented worst case, tail exposure"
mechanism was built on that comparison. The comparison was wrong. Against the
right baseline (four reads = 5388 us uncontended) the worst observed fetch is
a ~6x stretch, and the average stretch across the bad window is ~1.55x.

## What the gate needs before it can answer

- a **settle period** so the post-resume prime is excluded
- **per-window deltas** rather than cumulative counters
- a **real baseline** — level 0 measured identically, for comparison
- a **keep-up ratio** (sectors fetched vs sectors needed), the direct question
- **gating on actually playing**, so a stopped transport cannot pollute it
- **per-read timing** kept distinct from per-sector-fetch timing

## Cost model (unchanged, from the storage sweep)

| tracks reversed | read work / sector | vs today |
|---|---|---|
| 0 | 3139 µs (44.3%) | — |
| 1 | 3890 µs (54.9%) | +10.6 CPU points |
| 2 | 4640 µs (65.5%) | +21.2 CPU points |
| 3 or 4 | 5388 µs (76.1%) | +31.8 CPU points |

One reversed track costs a third of what four do. That is why the gate is now
parameterised rather than the feature abandoned.

Superseded status line (kept for the reasoning trail): **MEASURED — affordable**
referred to STORAGE only. The `'M'` sweep was run on real hardware
(firmware st32). The start-bit hunt scales with read size, so a stem-planar
layout makes per-track reverse fit. One gate remains, and it is not storage —
see "The second gate" below.

## The measurement

24 reads per size, uncontended (transfer mode, playback stopped):

| blocks | bytes | average | worst | hunt |
|---|---|---|---|---|
| 1 | 512 | 675 µs | 2585 µs | 11 µs |
| 2 | 1024 | 1040 µs | 1402 µs | 17 µs |
| **4** | **2048** | **1340 µs** | **1952 µs** | **31 µs** |
| 8 | 4096 | 1945 µs | 2458 µs | 56 µs |
| 16 | 8192 | 3152 µs | 3703 µs | 109 µs |

Fitted: **649 µs fixed per read + 158.4 µs per block.**

**The deciding number:** the hunt went 31 µs → 109 µs across a 4× size step,
a ratio of **3.52**. Per-block predicts ~4.0; per-read predicts ~1.0. It is
**hypothesis A**, and the feature is affordable.

| tracks reversed | reads | µs | duty |
|---|---|---|---|
| 0 | 1 × 16 blk | 3152 | **44.5%** |
| 1 | 1 × 12 blk + 1 × 4 blk | 3889 | 54.9% |
| 2 | 1 × 8 blk + 2 × 4 blk | 4625 | 65.3% |
| 3 or 4 | 4 × 4 blk | 5360 | **75.7%** |

## Two things the measurement corrected

**The reads are much faster than `ST_LAT_READ_TYP_US`.** A full sector is
3152 µs uncontended, not 5073. That older figure came from a boot capture
*with* contention and is a different quantity — it is **not** wrong and has
**not** been changed here. The read-ahead depth is sized from contended
worst cases and must stay that way.

**The fixed cost is 649 µs (21%), not the predicted 150 µs (3%).** The phase
breakdown suggested only the CMD18/CMD12 handshake was per-read; there is
about 500 µs more per-read overhead than that accounted for. The conclusion
survives, but for a different reason than predicted: not because the fixed
cost is negligible, but because the whole read is fast enough to pay it four
times over.

## The second gate — how to run it

Flash **st33**, then:

1. Enter transfer mode and send `'N'`. The device replies `'n'` (armed) and
   prints `STEMPLANAR sim=ON`. Counters are cleared.
2. Send `'X'` to leave transfer mode. Playback resumes.
3. **Play a song for several minutes.** Longer is better — an underrun that
   only appears after a minute is exactly the failure being looked for.
4. Watch the diagnostic line:

```
STEMPLANAR sim=ON und=0 rd_max_us=… reads=… busy=…% spare=…%
```

**`und=` is the answer.** Zero across sustained playback means the scheduler
can afford four diverging tracks and v1.2 is clear to build. Anything above
zero means it cannot, and no storage layout fixes that.

Send `'N'` again (or reboot) to disarm. The flag is never persisted.

### Why a read pattern and not a percentage

`CPU str=` reports what the streamer *uses*, not what it *could get* — today
it only asks for ~44%, so the number cannot answer whether it could have
75.7%. Instead the streamer is made to do the real thing: **four 4-block
reads per sector instead of one of sixteen**, which is exactly v1.2's worst
case with every track reversed. It reads the same sector, so the bytes and
the decoded audio are bit-identical; only the cost of getting them changes.

The four quarters are read **back to front**, so the card's sequential
read-ahead cannot make the simulation cheaper than the real thing — in v1.2
the four planes sit at unrelated song positions and get no such help.

## The second gate

Every duty figure above says what fraction of wall clock **the streamer
needs**. These reads were measured with playback stopped, so they are
uncontended. During playback the streamer competes with the audio thread,
the MIDI thread and the control loop, and this project has already been
bitten by exactly that: reads stretched from 5073 µs to ~12500 µs when the
streamer's share fell to 42%, and the song played slow and crushed.

Four reversed tracks need **75.7% of wall clock for the streamer alone**,
leaving 24% for everything else. Forward-only needs 44.5%. Whether the
scheduler can give the streamer three quarters of the CPU is **unmeasured**,
and it should be measured before the format change is committed to — it is
now the only thing between here and the feature.

## The request

Double-click a TRACK button to reverse that stem only, from its current
audible position, with the other three continuing forward. Each track
independent, compatible with varispeed, slow mode and loops.

## Why it is not free

`st_sector_v11.h` stores all four stems **interleaved in one 24-byte frame**
(`stem_l[4]`, `stem_r[4]`). There is no way to fetch only Track 2's bytes, so
a track at its own position needs a **whole extra sector stream**.

A reversed stem diverges from the forward transport at twice the play rate, so
it leaves the resident sector in about 3.5 ms. Retaining history in RAM only
delays the problem: sustained reverse needs sustained reads of older sectors.
With 34.5 KB free and 24 B per frame, a full-fidelity history of all four
stems buys **~30 ms** of backward travel, which is not a musical feature.

Estimated before the sweep, from `st_latency.h`'s contended figures
(**superseded** by the measurement above — the real uncontended read is 3152 µs,
so today's duty is 44.5%, not 71.6%; the conclusion below is unchanged because
it is about needing a *second whole stream*, not about its exact size):

| | |
|---|---|
| audio per sector | `ST_LAT_SECTOR_US` 7083 µs |
| sector read, contended | `ST_LAT_READ_TYP_US` 5073 µs |
| forward playback duty | 5073 / 7083 = 71.6% |
| one reversed track (v1.1 layout) | +71.6% → **143%** |

143% does not fit in 100%. On this device a sustained read deficit does not
present as dropouts; it presents as the song playing **slow and crushed**
(`docs/stem-tape-playback-physical-test.md`).

**The Tape Looper has no reverse to reuse.** There is no reverse DSP anywhere
in the firmware; `firmware/src/main.c` has never had one.

## Background: why it was in doubt at all

If each stem's samples were **contiguous in their own plane** within the
sector, a reversed stem could fetch just its own quarter — 4 blocks instead
of 16, aligned to the 512-byte block size the eMMC already works in.

That only helps if a quarter-size read costs about a quarter. From the boot
capture in `docs/stem-tape-playback-physical-test.md`, the 5073 µs read is:

| phase | time | scales with size? |
|---|---|---|
| SPIM3 DMA | 2056 µs | yes — 514 B × 16 blocks at 32 MHz |
| start-bit hunt | 1763 µs | **the question** |
| CRC + copy-out | 1104 µs | yes — per byte |
| CMD18/CMD12 | 150 µs | no — one handshake per read |

The start-bit hunt is 35% of the read, and everything turns on it:

| | fixed cost | 4-block read | four tracks reversed |
|---|---|---|---|
| **A** — hunt per **block** | 150 µs (3%) | 1381 µs | **77.9%** — affordable |
| **B** — hunt per **read** | 1913 µs (38%) | 2703 µs | **152.6%** — impossible |

Under B even a *single* reversed track lands at 98.6% — it "fits" only in the
sense that 98.6 < 100, with 1.4 points of headroom against a streamer that
does not get the whole wall clock. That is a refusal, not a pass.

`sp1_emmc.c`'s `emmc_read_blocks()` puts the hunt, the DMA and the CRC inside
its `for (i = 0; i < count; i++)` loop, and its own comments call the 80 ms
access hunt "every per-block bound" — which says **A**. That is a strong
argument from source and it is **not a measurement**. A layout change
re-encodes every stored song, so it is gated on the real card.

## Cost — measured

See the duty table at the top. Ordinary playback is **not** taxed: with nothing
diverging the four planes are contiguous and are still one read, so zero
reversed tracks costs exactly what playback costs today. The price is one
read's fixed cost per diverging track, paid only while tracks actually diverge.

## Running the measurement

Enter transfer mode and send `'M'` (no payload). The device replies `'m'`,
then prints one line per read size:

```
STEMRC sweep begin (read-only, 24 reps per size)
STEMRC blocks=1  n=24 avg_us=… worst_us=… hunt_us=… dma_us=… crc_us=…
STEMRC blocks=2  …
STEMRC blocks=4  …          <- the stem-plane size; this is the one that matters
STEMRC blocks=8  …
STEMRC blocks=16 …          <- a full sector; measured 3152 us
STEMRC sweep end
```

The sweep is **read-only** — it reads blocks `'R'` already permits, never
writes, erases or flushes, and CI enforces that
(`.github/scripts/stemtape_player_readcost_sweep_readonly_gate.py`). It runs
only from transfer mode, where playback is stopped, so it cannot steal
bandwidth from a live stream and skew its own result.

**Reading the answer:** watch `hunt_us` across the sizes. If it falls roughly
in proportion to `blocks=`, hypothesis A holds and per-track reverse is
affordable — which is what the run above found (31 µs at 4 blocks, 109 µs at
16). If it had stayed flat regardless of size, B would hold and no layout
change would rescue the feature. Feed the `blocks` / `avg_us` pairs through
`st_readcost_fit()` for the fitted split and
`st_readcost_planar_duty_ppm()` for the duty at each level of divergence.

## What would then have to be built

Confirming A does not implement the feature. It unblocks a sizeable project,
and the CPU-share gate above should be settled first — it can still stop it:

0. **Measure the streamer's wall-clock share during playback.** Four reversed
   tracks need 75.7% of it. If the scheduler cannot give that, the format
   change would be wasted work.
1. **Sector format v1.2** — stem-planar layout, new encoder/decoder, new
   fixtures. The frozen handoff fixtures are byte-exact contracts.
2. **Companion re-encode** — every stored song must be re-uploaded.
3. **Streaming rework** — up to four independent plane readers with their own
   residency, replacing today's single sliding window.
4. **The reverse reader** — negative rate through the existing resampler,
   which already carries a signed cursor concept, plus boundary wrapping in
   the reverse direction.
5. **The gesture** — per-track double-click with consumption. Note the
   companion places lane reverse on **FUNCTION + double-tap Track**
   (`src/machine/surface.ts:741`), and records that a *bare* Track double-tap
   is loop capture; the requested bare double-click would collide with it.

## Files

- `src/st_readcost.h` / `.c` — the pure model
- `tests/test_readcost.c` — arithmetic, and proof the two hypotheses diverge
- `src/main.c` — the `'M'` sweep
- `.github/scripts/stemtape_player_readcost_sweep_readonly_gate.py` — read-only gate
