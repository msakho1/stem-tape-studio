/*
 * st_latency.h — THE storage-latency model, in one place, as numbers.
 *
 * Every buffer depth in this firmware is a bet about how long the eMMC can
 * take to hand over a sector. Until now those bets were made independently,
 * in prose, in three different files, from three different numbers -- and at
 * least one of them was wrong. This header is the single source: the measured
 * figures, the guarantee derived from them, and the depths that guarantee
 * implies. Nothing may size a pool from a figure that is not here.
 *
 * ======================================================================
 * THE MEASUREMENTS
 * ======================================================================
 * ST_LAT_READ_TYP_US   5073   Uncontended sector read, slice T0's benchmark
 *                             on real hardware. This is a TYPICAL read. It is
 *                             not a bound on anything.
 *
 * ST_LAT_READ_WORST_US 16100  The WORST sector read measured on real
 *                             hardware, recorded in st_stem_bufmbox.h since
 *                             the ring was sized. This is the figure every
 *                             depth must be derived from.
 *
 * ST_LAT_DRIVER_MAX_US 80000  Not a measurement. It is how long sp1_emmc.c's
 *                             start-bit hunt is willing to wait before giving
 *                             up. It is the driver's patience, i.e. the point
 *                             past which a read is declared failed -- not an
 *                             observed duration.
 *
 * ======================================================================
 * THE CORRECTION THAT PRODUCED THIS FILE
 * ======================================================================
 * The loop-pin depth shipped in st17 was sized against "5073 + 5073 =
 * 10.15 ms", built by doubling the TYPICAL read. That is the wrong quantity:
 * a depth exists precisely for the atypical case. Against the measured worst
 * read the same worst case is 16100 + 16100 = 32.2 ms, and st17's three
 * pinned sectors (14.19 ms of runway) do not cover it. The gate injected
 * 10.15 ms, so it never tested the real bound.
 *
 * st_stem_bufmbox.h carried a second, opposite error: it claimed eleven
 * sectors of read-ahead (77.9 ms) "covers the driver's own worst-case
 * allowance [80 ms] with margin". 77.9 < 80. It never covered it.
 *
 * ======================================================================
 * THE GUARANTEE, STATED ONCE
 * ======================================================================
 * This firmware guarantees uninterrupted audio across
 *
 *      ONE worst-case measured read (16.1 ms) arriving while another read
 *      is already in flight (a further typical read, 5.073 ms),
 *
 * that is ST_LAT_GUARANTEE_US = 21.2 ms of the producer delivering nothing.
 *
 * WHY THAT AND NOT MORE. The alternatives were considered rather than
 * assumed:
 *
 *   * "two consecutive WORST reads" (32.2 ms) requires the conjunction of two
 *     independent rare events. It is affordable only by making every pool
 *     ~50% deeper, and on a 256 KB part that is the difference between
 *     shipping the remaining roadmap and not.
 *
 *   * "the driver's 80 ms allowance" is not achievable at any depth this
 *     device can afford: it would need 12 sectors of runway at EVERY seek
 *     target, and the current ring does not achieve it either. A read that
 *     actually takes 80 ms is a card fault, and the honest response to a card
 *     fault is an underrun, not 200 KB of buffers.
 *
 * WHAT THIS FIRMWARE THEREFORE DOES NOT PROMISE, stated so no comment
 * anywhere claims otherwise: it does not promise to survive the driver's full
 * 80 ms start-bit allowance, and it never did.
 *
 * ======================================================================
 * THE MARGIN, AND WHY IT IS IN THE MODEL RATHER THAN IN THE DEPTHS
 * ======================================================================
 * ceil(GUARANTEE / SECTOR_US) alone produces depths whose margin is whatever
 * the rounding happens to leave. At this guarantee that is 76 us out of
 * 21173 -- 0.36%, less than the quantisation of a single 5.333 ms output
 * block, and the loop gate demonstrated it: a depth sized that way survived
 * the arithmetic and failed the injected stall.
 *
 * So the margin is applied ONCE, here, as a stated fraction of the
 * guarantee, and every depth is derived from the margined budget. Sectors are
 * never added on top of a derived depth to "feel safer" -- that is how the
 * pools reached a size this device cannot afford.
 *
 * ======================================================================
 * DEPTHS THE BUDGET IMPLIES
 * ======================================================================
 * A sector is ST_LAT_SECTOR_US of audio. Two geometries need covering, and
 * they differ by exactly one sector -- a geometric fact, not a safety pad:
 *
 *   READ-AHEAD (steady playback). D sectors buffered ahead of the playhead
 *   cover D * SECTOR_US of producer silence, because the consumer starts at a
 *   sector boundary. D = ceil(BUDGET / SECTOR_US).
 *
 *   RESIDENCY AT A SEEK TARGET (loop entry, loop wrap, loop exit). The target
 *   frame can sit on the LAST frame of its sector, so n pinned sectors cover
 *   only (n-1) * SECTOR_US. n = D + 1.
 *
 * Both are computed here so no call site can round them differently, and the
 * ring's slot count follows from D as well: one slot the consumer holds, D
 * ahead of it, and one the producer may never target because the consumer
 * holds it (see st_stem_bufmbox.h).
 */

#ifndef ST_LATENCY_H_
#define ST_LATENCY_H_

#include "st_v11_format.h"

/*
 * Microseconds of audio in one 8192-byte sector -- DERIVED, because the frame
 * count inside a sector is a property of the stored sample width.
 *
 * This was the literal 7083, which is 340 frames at 48 kHz: correct for
 * v1.1/v1.2's 24-bit samples and silently wrong for v1.3's 16-bit ones,
 * where the same 8192 bytes hold 510 frames and therefore 10,625 us. Every
 * depth in this header is expressed in sectors and converted through this
 * constant, so a stale value understates the read-ahead cover by a third --
 * and understates it in the SAFE direction, which is exactly why nothing
 * would have failed loudly. tests/test_loop_playback_gate.c caught it only
 * because it asserts the derived depth is MINIMAL, and a depth that is
 * secretly 1.5x deeper than believed stops being minimal.
 */
#define ST_LAT_SECTOR_US \
	((ST11_FRAMES_PER_SECTOR * 1000000u) / ST11_SAMPLE_RATE_HZ)

/* Measured on real hardware. See the header comment for provenance. */
#define ST_LAT_READ_TYP_US    5073u
#define ST_LAT_READ_WORST_US 16100u

/* sp1_emmc.c's start-bit patience. NOT a measured read duration. */
#define ST_LAT_DRIVER_MAX_US 80000u

/* THE GUARANTEE: one worst-case read behind one typical in-flight read. */
#define ST_LAT_GUARANTEE_US (ST_LAT_READ_WORST_US + ST_LAT_READ_TYP_US)

/* ceil(a / b) for unsigned compile-time constants. */
#define ST_LAT_CEIL_DIV(a, b) (((a) + (b) - 1u) / (b))

/* The margin, applied once. 15% of the guarantee -- comfortably more than one
 * output block (5.333 ms), so a depth can never pass the arithmetic and fail
 * the injected stall. */
#define ST_LAT_MARGIN_PCT 15u
#define ST_LAT_BUDGET_US \
	(ST_LAT_GUARANTEE_US + (ST_LAT_GUARANTEE_US * ST_LAT_MARGIN_PCT) / 100u)

/* Sectors of read-ahead that cover the budget from a sector boundary. */
#define ST_LAT_READAHEAD_SECTORS \
	ST_LAT_CEIL_DIV(ST_LAT_BUDGET_US, ST_LAT_SECTOR_US)

/* Sectors that must be resident AT a seek target, whose frame may sit on the
 * last frame of its sector. One more than the read-ahead depth. */
#define ST_LAT_RESIDENCY_SECTORS (ST_LAT_READAHEAD_SECTORS + 1u)

/* Ring slots: the one the consumer holds, D ahead of it, and one the producer
 * may never target because the consumer holds it. */
#define ST_LAT_RING_SLOTS_MIN (ST_LAT_READAHEAD_SECTORS + 2u)

/*
 * THE REFILL BATCH, and why the ring is rounded UP to a multiple of it.
 *
 * A batch is one emmc_read_blocks() only while its destination slots are
 * contiguous, and slot is sector % G, so a run that crosses the end of the
 * ring becomes two reads. When R divides G, runs aligned to a multiple of R
 * never cross it -- that rule is why G=7/R=3 is worse than it looks, its
 * batches cycling 3,3,1.
 *
 * R lives here rather than in st_planar.h because the ring size depends on
 * it, and the ring size is depth policy, which is this header's subject. R=3
 * is a hardware measurement: tools/sp1-readcost-sweep.py fitted
 * us = 650 + 159*blocks on real hardware, so half of a single-group read is
 * fixed command overhead and batching pays. See st_planar.h for the full
 * derivation and the worst-case CPU table that picked 3 over 2 and 6.
 *
 * WHY THE ROUND-UP EXISTS AT ALL. ST_LAT_RING_SLOTS_MIN is derived from the
 * stall budget divided by a sector's DURATION, and v1.3 made a sector 510
 * frames instead of 340 -- so the same real-time cover now needs 5 slots
 * where it needed 6. Five is correct as a minimum and unusable as a ring:
 * 3 does not divide 5, so every batch would straddle the wrap and cost two
 * reads instead of one. Rounding up to 6 restores the divisibility, costs
 * exactly the RAM the ring already had, and spends the surplus on read-ahead
 * rather than throwing it away.
 */
#define ST_LAT_REFILL_GROUPS 3u
#define ST_LAT_RING_SLOTS \
	(ST_LAT_CEIL_DIV(ST_LAT_RING_SLOTS_MIN, ST_LAT_REFILL_GROUPS) * \
	 ST_LAT_REFILL_GROUPS)

#if !defined(__cplusplus)
_Static_assert(ST_LAT_RING_SLOTS >= ST_LAT_RING_SLOTS_MIN,
	       "the ring may be rounded up for batch alignment, never down");
_Static_assert((ST_LAT_RING_SLOTS % ST_LAT_REFILL_GROUPS) == 0u,
	       "R must divide G or every refill batch straddles the ring wrap");
#endif

/* What a given residency depth actually covers, in microseconds, for tests
 * and reports: (n-1) whole sectors plus the single frame the target sits on. */
#define ST_LAT_RESIDENCY_COVER_US(n) (((n) - 1u) * ST_LAT_SECTOR_US)

#endif /* ST_LATENCY_H_ */
