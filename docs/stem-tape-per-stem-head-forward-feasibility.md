# Stem Tape — per-stem tape-head forward-feasibility report

Bounded, non-implementing analysis, requested at the close of Phase 2's
continuous-streaming arc (Slices 3A/3B/3B.1/3C). Purpose: quantify what a
later "independent per-stem tape-head" mechanism — each of the four stems
(vocal/drums/bass/inst) playable from its own, independently movable
position, rather than all four locked to one shared master position —
would cost in RAM and eMMC read bandwidth against the architecture Phase 2
just built, and flag anywhere that architecture would silently foreclose
it. **No heads are implemented here. No production code changes.**

## 1. What Phase 2 actually built (the numbers this report reasons from)

All constants below are the real, compiled values in
`st_v11_format.h`/`st_sector_v11.h`, not estimates:

| Quantity | Value | Source |
|---|---|---|
| Sector size | 8,192 B | `ST11_SECTOR_BYTES` |
| Sector header | 32 B | `ST11_SECTOR_HEADER_BYTES` |
| Sector payload | 8,160 B | `ST11_SECTOR_PAYLOAD_BYTES` |
| Frames per sector | 340 | `ST11_FRAMES_PER_SECTOR` |
| Bytes per frame | 24 B | `ST11_BYTES_PER_FRAME` (4 stems × 2 ch × 3 B) |
| Sample rate | 48,000 Hz | fixed format assumption |
| Stems | 4 | `ST11_STEM_COUNT` |

**The frame format interleaves all four stems together.** Every 24-byte
frame is `[vocalL, vocalR, drumsL, drumsR, bassL, bassR, instL, instR]`,
each a 24-bit sample, laid out stem-major inside one shared frame — see
`st_sector_v11.h`'s own layout comment. A sector is therefore not "one
stem's data" — it is **one shared time-position's data for all four
stems at once**. This is the load-bearing fact for everything below.

**Required sustained read rate** (one sector's payload must arrive before
its 340 frames play out): `8192 B / (340 / 48000 s) ≈ 1,156,518 B/s ≈
1.10 MiB/s`. This is the same figure the Slice 3B.1 directive referred to
as "the 1.152 MB/s requirement" — consistent within rounding. It has
**not** been measured on real SP-1 eMMC hardware (per the 3B.1 directive,
that claim stays open until physically measured); the number here is the
format's own arithmetic requirement, independent of hardware.

**Current buffering:** exactly two 8,192 B sector buffers
(`g_stem_sector_buf`, `g_stem_sector_buf_b` — `main.c:1172-1174`), one
`st_stream_t` (one shared master position), one `st_stem_bufmbox_t` (one
producer↔consumer handoff). Total buffer RAM: **16,384 B**. Measured RAM
at the Slice 3B.1/3C baseline: 221,068 B of 262,144 B (nRF52840 SRAM) —
**41,076 B free**, measured, not estimated.

## 2. What generalizes to N independent heads cleanly

`st_stream_t` (`st_stem_stream.h`) and `st_stem_mbox_t`
(`st_stem_bufmbox.h`) are both **pure, allocation-free, single-owner
structs with no hidden global-singleton coupling** — nothing in either
module's own code assumes there is exactly one instance. `st_stream_t`
already tracks one independent playback position (`song_frame`,
`ready_sector`, `state`, `underrun_count`) per instance; `st_stem_mbox_t`
already implements one independent SPSC handoff per instance. Four of
each — one `st_stream_t` + one `st_stem_mbox_t` per stem head — is a
mechanical instantiation, not a redesign of either module. **This part of
Phase 2's design does not lock the later feature out.**

## 3. What does NOT generalize: the storage format

Because one sector's 24-byte frame holds all four stems' samples
together, an independent per-stem head cannot fetch "just its own stem"
without fetching the **entire sector** at whatever position it currently
sits at — the other three stems' bytes come along whether needed or not.
When all four heads are at the same position (today's case), one sector
fetch serves all four "for free." The moment heads diverge to different
positions, each divergent head needs its **own full 8,192 B sector read**
to recover its 6 bytes/frame (2 ch × 3 B) of actually-useful payload —
**25% payload utilization per read** in the fully-diverged case.

### 3a. Read-bandwidth implication (the significant number)

| Scenario | Concurrent divergent positions | Required sustained read rate |
|---|---|---|
| Today (Phase 2, shipped) | 1 (all 4 stems locked together) | ≈1.10 MiB/s |
| 4 independent heads, worst case (all 4 at different positions) | 4 | ≈4.41 MiB/s (4×) |
| 4 independent heads, best case (heads happen to coincide) | 1–4, position-dependent | 1.10–4.41 MiB/s |

A later per-stem-head feature, built on **today's storage format
unchanged**, has a real eMMC-read-bandwidth ceiling of **up to 4× the
current requirement**, purely from format-forced over-fetch — not
something buffering or threading can absorb, since the bytes genuinely
have to come off the device. Whether the SP-1's eMMC can sustain ~4.4
MiB/s is unmeasured and out of this report's scope; the point is that the
number to eventually measure against is 4×, not 1×.

The alternative — the only way to avoid the 4× penalty — is a **future
storage-format revision** that de-interleaves stems into independently
addressable sector streams (e.g. one sector stream per stem, each sized
for that stem's own 2-channel/24-bit payload instead of 4-stem-bundled
frames). That is a new on-disk format version, not a firmware-only
change: it changes what the companion writes, so it is a protocol-version
decision, not something to retrofit silently later. Flagging it now is
the entire point of this report — Phase 2 does not need to decide this,
but whoever scopes the head feature does.

### 3b. RAM implication

Buffering does scale linearly and is comparatively cheap:

| Design | Buffers | RAM | Delta vs. today |
|---|---|---|---|
| Today: 1 shared position, double-buffered | 2 × 8,192 B | 16,384 B | — |
| 4 independent heads, single-buffered each (higher underrun risk, no prefetch headroom) | 4 × 8,192 B | 32,768 B | +16,384 B |
| 4 independent heads, double-buffered each (matches today's per-head safety margin) | 8 × 8,192 B | 65,536 B | +49,152 B |

Against the measured 41,076 B currently free: the single-buffered-per-head
option (+16,384 B) fits with headroom to spare (24,692 B left); the
double-buffered-per-head option (+49,152 B) **does not fit** — it
overruns the current free budget by 8,076 B, before accounting for the
4 independent `st_stream_t`/`st_stem_mbox_t` structs themselves (small,
well under 1 KB combined) or any per-head diagnostic/control state a real
UI would need. A later head feature built double-buffered-per-stem, on
this SoC, needs either a RAM reclamation pass elsewhere first or a
narrower buffering strategy (e.g. shared-pool buffers sized to the
*currently divergent* head count rather than a fixed 4× allocation).

### 3c. Threading/scheduling implication (qualitative, not quantified here)

The SP-1 has one physical eMMC read path. Four independent producer
demands do not turn into four parallel physical reads — they still
serialize through one device. Whether that is modeled as one producer
thread round-robining four mailboxes, or four threads contending for one
underlying read call, the *aggregate* bandwidth ceiling from §3a applies
either way, and per-head prefetch latency grows with however many heads
are actually diverged at a given moment (a head waiting behind three
others' reads needs deeper lookahead than today's ~14 ms/2-sector margin
to avoid underrun). This needs real scheduling design when the feature is
actually scoped; it is not a number this report can respons­ibly quantify
without a chosen scheduling policy, so it is flagged, not sized.

## 4. Bottom line

- **Does not lock out:** the state-machine/mailbox architecture
  (`st_stream_t`, `st_stem_bufmbox_t`) — both already generalize to N
  independent instances with no rework.
- **Does constrain:** the on-disk sector **storage format** — interleaved
  4-stem frames force up to a 4× eMMC read-bandwidth requirement
  (≈1.10 → ≈4.41 MiB/s) the moment stem positions diverge, and there is
  no firmware-only fix for that; it requires a future storage-format
  revision if 4× read bandwidth turns out not to be available on real
  hardware.
- **RAM is tight but survivable** at single-buffering-per-head (+16,384 B,
  fits in the measured 41,076 B free); double-buffering-per-head
  (+49,152 B) currently does **not** fit and would need either RAM
  reclaimed elsewhere or a non-fixed buffering strategy.
- **No decision is required now.** Phase 2 ships with a shared-position
  design that is correct and proven for today's scope (four synchronized
  stems, one position). This report exists so that whoever scopes the
  per-stem-head feature later starts from real numbers instead of
  discovering the 4× bandwidth requirement — or the RAM shortfall — after
  committing to a storage format that can't support it.
