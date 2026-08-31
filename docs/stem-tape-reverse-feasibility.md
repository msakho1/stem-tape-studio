# Per-track reverse playback — feasibility, and what it costs

Status: **blocked on one hardware measurement.** The arithmetic is settled and
host-tested (`tests/test_readcost.c`); the number that decides the outcome
must be read off the real card with the `'M'` command before any sector
layout is touched.

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

Against `st_latency.h`'s measured figures:

| | |
|---|---|
| audio per sector | `ST_LAT_SECTOR_US` 7083 µs |
| typical sector read | `ST_LAT_READ_TYP_US` 5073 µs |
| forward playback duty | 5073 / 7083 = **71.6%** |
| headroom | 28.4% — about 0.4 of a stream |
| one reversed track (v1.1 layout) | +71.6% → **143%** |

143% does not fit in 100%. On this device a sustained read deficit does not
present as dropouts; it presents as the song playing **slow and crushed**
(`docs/stem-tape-playback-physical-test.md`).

**The Tape Looper has no reverse to reuse.** There is no reverse DSP anywhere
in the firmware; `firmware/src/main.c` has never had one.

## The one thing that could make it affordable

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

## Cost, if A holds

| tracks reversed | reads per sector | duty |
|---|---|---|
| 0 | 1 | 71.6% — unchanged from today |
| 1 | 2 | 73.7% |
| 2 | 3 | 75.8% |
| 3 or 4 | 4 | 77.9% |

Ordinary playback is **not** taxed: with nothing diverging the four planes are
contiguous and are still one read. The price is one read's fixed cost per
diverging track, and it is only paid while tracks actually diverge.

## Running the measurement

Enter transfer mode and send `'M'` (no payload). The device replies `'m'`,
then prints one line per read size:

```
STEMRC sweep begin (read-only, 24 reps per size)
STEMRC blocks=1  n=24 avg_us=… worst_us=… hunt_us=… dma_us=… crc_us=…
STEMRC blocks=2  …
STEMRC blocks=4  …          <- the stem-plane size; this is the one that matters
STEMRC blocks=8  …
STEMRC blocks=16 …          <- should land near 5073
STEMRC sweep end
```

The sweep is **read-only** — it reads blocks `'R'` already permits, never
writes, erases or flushes, and CI enforces that
(`.github/scripts/stemtape_player_readcost_sweep_readonly_gate.py`). It runs
only from transfer mode, where playback is stopped, so it cannot steal
bandwidth from a live stream and skew its own result.

**Reading the answer:** watch `hunt_us` across the sizes. If it falls roughly
in proportion to `blocks=`, hypothesis A holds and per-track reverse is
affordable. If it stays near 1763 µs regardless of size, B holds and no layout
change rescues the feature. Feed the `blocks` / `avg_us` pairs through
`st_readcost_fit()` for the fitted split and
`st_readcost_planar_duty_ppm()` for the duty at each level of divergence.

## What would then have to be built

Confirming A does not implement the feature. It unblocks a sizeable project:

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
