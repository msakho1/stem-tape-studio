# Stem Tape T0 — throughput budget and physical benchmark

Status: **RETIRED as of Slice C4.** The T0 "STOP and wait for physical
results" plan below was superseded mid-flight by a controlling directive
that moved straight to building the real production bulk-upload path
(`'U'`, `xfer_bulk_write_sector()` — see `docs/stem-tape-bulk-upload-v1.md`)
without waiting for a physical T0 measurement first. That real path now
exists, is wired into the production firmware, and supersedes this
benchmark entirely: the `'Y'` command and its whole write path
(`xfer_bench_run()`, `bench_inactive_song_region()`, and everything else
this document specifies) have been **removed from `main.c`** — not merely
disabled — because production firmware has no further use for an
unbounded, arbitrary-pattern eMMC writer once the real verified bulk path
exists. This document is kept, unmodified below this notice, purely as a
historical record of the real engineering it did before removal: the two
real physical failure block numbers (4611, 4745) it documents, the RAM/
safety analysis, and the wire-protocol design that shaped the real bulk-
upload contract's own decisions (e.g. reusing `s_v11_verify_scratch`
rather than a second 8 KiB buffer, and using `st_ab_session_check_write()`
as the sole authoritative per-block gate). No further physical measurement
against this specific benchmark will happen; the real bulk path's own
physical verification is what matters now.

---

*Everything below this line is the ORIGINAL Slice T0 document, unmodified,
kept for historical/design-provenance reasons only — see the retirement
notice above.*

---

Status: **CI-verified, physical measurement PENDING.** This is Slice T0 of
the upload-reliability phase. Per the phase directive: *"Build the safe
benchmark, get CI green, provide the firmware artifact and stop for my
physical measurement... STOP after T0 and wait for my physical results. Do
not guess the physical throughput from CI."* Nothing below claims a
physical throughput number — CI can prove the code is safe and the wire
protocol works; it cannot prove what a real SP-1's USB/CDC stack or eMMC
card actually sustain.

## 1. Why this exists

The phase target: a prepared four-stem song (225.35 s/stem, 48 kHz/stereo/
24-bit, 10,816,646 frames, 31,814 logical 8 KiB sectors, 509,024 physical
512-byte blocks, ≈248.5 MiB) must go from "user presses Upload" to "ready
to press PLAY" in ≤180 s — at least 1.38 MiB/s effective payload
throughput, with a design target of ≥1.5 MiB/s to leave headroom for
flushes, protocol responses, and verification.

Two real physical uploads of the same song failed at block 4611 and block
4745 respectively (three retries each, connection stayed open, no
validity magic written, generation 1 correctly stayed active). The
current write path issues one command and one `CMD24` (single-block) eMMC
operation per 512-byte block, and returns the same generic `E` for every
failure — so today there's no way to tell whether those two failures were
CDC loss, framing, a stale ACK, a session-gate rejection, or an eMMC-level
failure.

T0 does **not** fix that failure and does **not** build the fast-transfer
protocol. It only measures, on real hardware, the numbers T1-T5 need to be
designed against instead of guessed.

## 2. What was inspected before writing this

- `sp1_emmc.c`'s `emmc_write_blocks()`/`emmc_read_blocks()` **already**
  support real multi-block bursts: `count == 1` issues `CMD24`
  (single-block), `count > 1` issues `CMD25` (multi-block) with proper
  busy-wait, `CMD12` stop, and a burst deadline. The current bottleneck for
  a future fast path is therefore at the **protocol layer** (today's wire
  protocol only ever calls with `count = 1`), not the eMMC driver.
- The existing eMMC diagnostic counters (`emmc_dbg_cmd_retries`,
  `emmc_dbg_busy_timeouts`, `emmc_crc_wr_errs`, `emmc_crc_rd_errs`) are
  real and already maintained by the production write/read path — T0
  reuses them rather than inventing parallel counters.
- The CDC RX path had a real, previously invisible gap: `cdc_rx_isr()`
  discarded `ring_buf_put()`'s return value outright, so if the 1024-byte
  `g_cdc_rx` ring was ever full when the ISR tried to add bytes (e.g. the
  consumer stalled inside a slow eMMC write), the excess bytes were
  silently dropped with **no counter anywhere ever recording it**. This is
  a genuine candidate cause for the block 4611/4745 failures. T0 makes
  this loss *visible* (`g_cdc_rx_dropped_bytes`, reported in every
  benchmark result) without changing the ring size or drain rate — the
  actual fix, if this turns out to be the real cause, is T1's job.
- Transport is already paused for the whole duration of any transfer
  session: entering `g_xfer_mode` already sets `g_playing = 0`, so no new
  pause logic was needed for the benchmark.

## 3. Safety argument

Every write this benchmark issues goes through the same two mechanisms the
production write path (`xfer_v11_write()`) is bounded by — no parallel,
unverified bounds-check exists:

- `xfer_v11_refresh_session()` — the same function `'Q'` already calls, to
  open/refresh `g_v11_session` fresh from the real on-disk index.
- `st_ab_session_check_write()` — the same per-block gate
  `xfer_v11_write()` itself uses.

The benchmark never writes to an index region — it only ever computes a
target inside the *current inactive song region's* own address range —
and never constructs, let alone writes, an `ST11_INDEX_MAGIC` record, so
`st_ab_session_check_write()`'s own magic-detection path is never reached.
There is no code path in the benchmark that can ever commit a new
generation. Interrupting a benchmark run (power loss, host disconnect,
reset) leaves at most harmless pattern bytes inside the currently inactive
song region — which a real upload would overwrite in full anyway before
computing its own checksums against it. The active song, active index, and
both records' magic bytes are never touched.

This is proven structurally, not just asserted: the strict persistence
safety gate (`.github/scripts/stemtape_player_safety_gate.py`, pass D)
verifies — from the real compiled source, not by trust — that every
`emmc_write_blocks()` call site in the firmware is enclosed by one of
exactly two functions (`xfer_v11_write()`, `xfer_bench_run()`), and that
each one's own body genuinely contains its required safety-mechanism
calls with an early `return -1;` guard before ever reaching the write.

## 4. RAM

Zero new large static buffers. Every mode reuses `s_v11_verify_scratch`
(`ST11_SECTOR_BYTES` = 8192 B), already allocated for the real commit
path's own verify-before-commit step, and otherwise idle for the entire
duration of a benchmark run (a benchmark never reaches a magic-committing
write, so the real verify step can never be in flight at the same time).

This is also why T0 does **not** test a single eMMC burst larger than one
8 KiB sector (16 blocks): `CONFIG_MAIN_STACK_SIZE` is 4096 B (`prj.conf`)
and the measured free-RAM baseline at the start of this slice was ~41 KB
(see `docs/stem-tape-per-stem-head-forward-feasibility.md`'s own measured
figure, adjusted for this session's own small Phase 3 additions) — against
the 32 KiB floor standing constraint, that leaves on the order of 8 KB of
safe new-static-allocation headroom, not comfortably enough to add a
second full scratch buffer and still call the remaining margin a real
floor rather than a number chosen to just barely clear it.

"Any larger transfer unit that fits safely" is instead exercised as
multiple chained 16-block (8 KiB) `CMD25` bursts within one benchmark call
(`BENCH_MODE_EMMC_WRITE`/`READ`/`END_TO_END`'s own `total_blocks` can far
exceed `per_op`) — a real, honestly-labeled measurement of whether a
longer *session* of same-size bursts sustains higher throughput than
issuing them one at a time from the host, but **not** a claim about one
single burst larger than 8 KiB. Once this commit's real CI build reports
the exact free-RAM figure, a true larger-single-burst test can be scoped
against real numbers rather than guessed here.

## 5. Wire protocol

New command byte `'Y'` — additive only; `P`/`Q`/`R`/`W`/`F`/`X` are
byte-for-byte unchanged.

```
host   -> device: 'Y' <mode:u8> <mode-specific params, little-endian>
device -> host:   <status:u8> ['y' only: 32-byte result record, LE]
```

`status`: `'y'` = ok (result record follows); `'e'` = rejected (invalid
params, no v1.1 layout, no inactive region, or the operation itself
failed) — no result record follows `'e'`.

### Result record (8 × u32 LE = 32 bytes)

| Offset | Field | Meaning |
|---|---|---|
| 0 | `elapsed_ms` | `k_uptime_get()` delta for this one call |
| 1 | `bytes_or_units` | mode-specific (see table below) |
| 2 | `cdc_rx_dropped_bytes` | `g_cdc_rx_dropped_bytes` — **cumulative since boot**, not a per-call delta (diff two results yourself) |
| 3 | `emmc_cmd_retries` | `emmc_dbg_cmd_retries`, cumulative |
| 4 | `emmc_busy_timeouts` | `emmc_dbg_busy_timeouts`, cumulative |
| 5 | `emmc_crc_wr_errs` | cumulative |
| 6 | `emmc_crc_rd_errs` | cumulative |
| 7 | mode-specific | 0 for every mode except `CDC_ONLY`, which reports the line's currently negotiated baud rate here (real hardware line-coding state via `uart_line_ctrl_get()`; 0 if the backend doesn't support the query) |

Every cumulative counter is a real, already-existing diagnostic this
codebase already maintains for the real data path, except
`cdc_rx_dropped_bytes`, which is new instrumentation for a real,
previously-invisible gap (§2).

### Modes

| # | Name | Params | Measures |
|---|---|---|---|
| 0 | `CDC_ONLY` | `total_bytes:u32` | Drains `total_bytes` from the CDC ring, touches no eMMC at all — isolates host↔device USB throughput from flash entirely. Capped at 4 MiB/call. `bytes_or_units` = bytes actually received. |
| 1 | `EMMC_WRITE` | `per_op:u16` (1 or 16 only), `total_blocks:u32` (multiple of `per_op`, ≤4096) | Writes a fixed, deterministic, never-zero, never-magic-shaped pattern (generated locally, never received over the wire) to the current inactive song region in bursts of `per_op` blocks, via the same `emmc_write_blocks()` the real path uses (`count=1`→`CMD24`, `count=16`→`CMD25`). Pure eMMC-write speed, no CDC/CRC cost. `bytes_or_units` = `total_blocks * 512`. |
| 2 | `EMMC_READ` | same | Reads back `total_blocks` from the most recent `WRITE` call's own base (or a fresh session base if none this boot). Pure eMMC-read speed. |
| 3 | `END_TO_END` | same | The real path, timed as one number: CDC receive of `per_op*512` real bytes, `st_crc32_compute()` over them (timed — the real future validation cost, not skipped), `st_ab_session_check_write()` per block, `emmc_write_blocks()`. True host→validate→flash throughput. |
| 4 | `FLUSH` | none | Times `emmc_cache_flush()` alone. |
| 5 | `READBACK` | `num_samples:u8` (2..16) | Times a deterministic spread readback over the most recent `WRITE` call's own range: sample 0 = first block, sample n-1 = last block, the rest evenly spaced — the exact sampling shape (first + last + even spread) T3's production readback rule will need real timing data for. |

Every `'Y'` call is a single, individually bounded operation (never more
than 4096 blocks = 2 MiB of eMMC traffic, or 4 MiB of pure CDC traffic,
per call), so no single call can run long enough to risk a
`k_uptime_get()` wraparound or starve the watchdog `main()` feeds
elsewhere. Issue repeated `'Y'` calls back to back to build up a larger
sample — exactly like a real upload issues repeated `'W'` calls today.

## 6. Physical test instructions

Prerequisites: the SP-1 is running this commit's firmware build; a
terminal/script capable of raw serial I/O at whatever baud the OS reports
for the device's CDC-ACM port (see §6.4 on why the configured baud may not
matter); the device has **any** valid v1.1 library on it (needed only so
`g_v11_layout_ready` is true and an inactive region exists — the benchmark
never touches the active song).

**Do not run this while a real upload is in progress on the same device.**
The benchmark and a real upload both target the same "current inactive
song region" — running them concurrently would make both results
meaningless, though neither can corrupt the active song or index.

### 6.1 Connect and confirm

1. Open the SP-1's CDC serial port.
2. Send `Q` (single byte) — confirm you get the existing STCP capability
   response you already use today. This proves the device is alive and
   `g_v11_layout_ready` before running any benchmark mode.

### 6.2 CDC-only throughput (mode 0)

Send `Y 0x00 <total_bytes:u32 LE>` (e.g. `total_bytes = 2097152` for 2
MiB), then immediately start writing `total_bytes` arbitrary bytes to the
serial port. Read the 1-byte status, then (if `'y'`) the 32-byte result
record. `elapsed_ms` and `bytes_or_units` give you host→device USB/CDC
throughput with zero eMMC involvement. `result[7]` gives you the real
negotiated baud rate — compare against whatever baud your host-side
Web Serial (or terminal) configuration requested, to answer whether the
configured value actually limits this CDC implementation.

Repeat at a few sizes (64 KiB, 512 KiB, 2 MiB, 4 MiB — the cap) to see
whether throughput is size-dependent (framing/setup overhead amortizing).

### 6.3 eMMC write/read throughput (modes 1, 2)

Send `Y 0x01 <per_op:u16 LE=1> <total_blocks:u32 LE>` for 512-byte-at-a-
time (today's real per-transaction unit), then `Y 0x01 <per_op:u16
LE=16> <total_blocks:u32 LE>` for one-sector-per-transaction (16 blocks =
8 KiB). Use the same `total_blocks` for both so the comparison is
apples-to-apples — e.g. 4096 blocks = 2 MiB, the per-call cap. Read status
+ result record each time.

Then send `Y 0x02` with the same params to read the same range back.

Run each combination a few times — eMMC write speed can vary with prior
card state (garbage collection, etc).

### 6.4 End-to-end throughput (mode 3)

Send `Y 0x03 <per_op:u16 LE> <total_blocks:u32 LE>`, then stream
`total_blocks * 512` bytes of arbitrary payload to the device (any
content — it's never compared against anything, just CRC'd and written).
This is the real host→CRC→eMMC→ACK path, timed as one number, at both
`per_op=1` and `per_op=16`.

### 6.5 Flush and readback timing (modes 4, 5)

After a write call, send `Y 0x04` (no params) to time a durability flush
alone. Then send `Y 0x05 <num_samples:u8>` (try 2 and 16) to time a
deterministic spread readback over the just-written range.

### 6.6 Recording results

Log every result record verbatim (all 8 fields) plus which mode/params
produced it. See §8 for the exact fields to fill into the throughput
table. If any call returns `'e'` instead of `'y'`, record that too —
it means the benchmark itself rejected the call (bad params, no v1.1
layout, or no inactive region), not a throughput data point.

## 7. Machine-readable output

Each `'Y'` call's response is exactly:

```
1 byte:  status ('y' or 'e')
32 bytes (only if status == 'y'): 8 x uint32, little-endian, per §5's table
```

A simple capture script should log, per call: `mode`, `params`, `status`,
and (if `'y'`) the 8 decoded u32 fields, as one row (CSV or JSON) per
call — this is the exact shape needed to fill in §8's table.

## 8. Throughput-vs-180s-budget table (structure — fill in with real numbers)

Target: 248.5 MiB (509,024 blocks) in ≤180 s ⇒ ≥1.38 MiB/s effective
payload throughput; design target ≥1.5 MiB/s to leave headroom for
flushes, protocol responses, and verification.

| Tier | Mode | `per_op` | Measured throughput (MiB/s) | Time for full 248.5 MiB (projected) | Meets 1.38 MiB/s? | Meets 1.5 MiB/s? |
|---|---|---|---|---|---|---|
| CDC only | 0 | n/a | *pending* | *pending* | *pending* | *pending* |
| eMMC write, 512 B/txn | 1 | 1 | *pending* | *pending* | *pending* | *pending* |
| eMMC write, 8 KiB/txn | 1 | 16 | *pending* | *pending* | *pending* | *pending* |
| eMMC read, 512 B/txn | 2 | 1 | *pending* | *pending* | *pending* | *pending* |
| eMMC read, 8 KiB/txn | 2 | 16 | *pending* | *pending* | *pending* | *pending* |
| End-to-end, 512 B/txn | 3 | 1 | *pending* | *pending* | *pending* | *pending* |
| End-to-end, 8 KiB/txn | 3 | 16 | *pending* | *pending* | *pending* | *pending* |
| Flush duration (single call) | 4 | n/a | *pending (ms, not MiB/s)* | — | — | — |
| Readback duration (2 / 16 samples) | 5 | n/a | *pending (ms, not MiB/s)* | — | — | — |
| CDC dropped bytes observed | — | — | *pending (count, cumulative)* | — | — | — |
| eMMC cmd retries / busy timeouts / CRC errs observed | — | — | *pending (counts, cumulative)* | — | — | — |

Methodology once real numbers arrive: `time_for_248.5MiB_s = (248.5 * 1024
* 1024) / (measured_MiB_s * 1024 * 1024)`, i.e. simply
`248.5 / measured_MiB_s` seconds, since both sides are in MiB. This
projection ignores flush/readback/protocol overhead for the *raw*
transfer tiers — the end-to-end tier already includes CRC validation, and
flush/readback durations from modes 4/5 must be added on top (multiplied
by however many flush/readback checkpoints T3/T4's actual design uses)
to get a true projected wall-clock total. **No number in this table may be
filled in from CI** — CI has no physical CDC line or eMMC card; every cell
here is filled in only from a real SP-1 run per §6.

## 9. What T0 explicitly does not do

- Does not change the production write path (`xfer_v11_write()`,
  `'W'`/`'F'`/`'X'`/`'Q'`) at all.
- Does not fix the block 4611/4745 failures — that's T1, gated on this
  slice's physical results.
- Does not build the fast multi-block production transfer path — that's
  T2/T3, gated on this slice's physical results per the phase directive's
  own delivery rule ("Do not build the final transfer architecture until
  the physical throughput benchmark has been run").
- Does not test a single eMMC burst larger than 8 KiB (§4).
- Does not touch the golden Tape Looper firmware (`firmware/src/main.c`).

## 10. Stop point

Per the phase directive, this phase stops here until physical measurement
results are available. T1 (precise failure reporting, and fixing the
actual proven cause of the block 4611/4745 failures) does not begin until
those results are in hand.
