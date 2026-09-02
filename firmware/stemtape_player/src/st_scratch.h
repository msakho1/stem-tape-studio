/*
 * st_scratch.h -- THE signed-head transport primitive, and the eMMC velocity
 * clamp that bounds it.
 *
 * PURE: no Zephyr, no ADC, no clock, no audio, no allocation. The control
 * thread hands this module a gesture (a rocker edge, or a fader movement) and
 * an elapsed time; the audio thread reads back a signed rate. This module owns
 * the physics and nothing else.
 *
 * ======================================================================
 * ONE TRANSPORT, THREE USES -- AND THAT IS THE WHOLE DESIGN
 * ======================================================================
 * Reverse, scrubbing and scratching are not three features. They are three
 * durations of one gesture on one signed-velocity head:
 *
 *   reverse    sustained negative velocity
 *   scrub      sustained user-controlled signed velocity
 *   scratch    rapid signed-velocity manipulation
 *
 * There is deliberately NO timeout that decides "this was a scratch, that was
 * a scrub". The state machine does not know the difference and must not: the
 * duration of the player's input is what produces the musical result, and a
 * threshold in here would be an arbitrary seam the hand can feel. Short
 * alternating rocker presses integrate into a back-and-forth head motion; a
 * sustained hold integrates into a shuttle. Same integrator, same clamp, same
 * zero crossing.
 *
 * The TARGET is the only thing that varies:
 *
 *   ST_SCRATCH_MASTER   all four heads, phase-locked, moved as one four-track
 *                       tape. There is no hidden forward transport running
 *                       underneath -- where the gesture leaves the head IS the
 *                       position, and releasing FUNCTION resumes from there.
 *   ST_SCRATCH_STEM_n   one head moves; the other three are untouched, keep
 *                       their own positions, and are never resynced.
 *
 * ======================================================================
 * WHY THERE IS A CLAMP, AND WHY IT IS DERIVED RATHER THAN CHOSEN
 * ======================================================================
 * Playback rate is a data rate. At |r| the transport consumes stored audio |r|
 * times faster, so the eMMC must supply it |r| times faster, and past some |r|
 * it simply cannot -- that is starvation by arithmetic, not by bad luck, and
 * no amount of buffering fixes a sustained overdraft.
 *
 * So the clamp is not a taste decision. It falls out of the measured read
 * cost, the refill batch size and how many heads are moving, and it is
 * computed here from those inputs rather than written down as a number
 * somebody liked. tests/test_scratch.c re-derives every figure below from the
 * same measurement and fails if a hand-edited constant drifts from it.
 *
 * THE MODEL, stated so it can be argued with:
 *
 *   One refill batch for ONE stem is ST_LAT_REFILL_GROUPS groups, each
 *   ST_PL_GROUP_BLOCKS blocks, in a single emmc_read_blocks(). Its cost is the
 *   measured fit us = 649 + 158.4*blocks (tools/sp1-readcost-sweep.py, st32
 *   sweep, 24 reads per size). It covers ST_LAT_REFILL_GROUPS sectors of
 *   audio, which at unity is ST_LAT_REFILL_GROUPS * ST_LAT_SECTOR_US.
 *
 *   A head moving at rate r consumes that cover r times faster. So N heads at
 *   rate r occupy the eMMC for
 *
 *       duty(N, r) = N * BATCH_US * r / BATCH_COVER_US
 *
 *   and the clamp is the largest r with duty <= the budget.
 *
 * THE BUDGET IS 85%, NOT 100%. The same 15% the rest of the streaming design
 * reserves (ST_LAT_MARGIN_PCT), for the same reason: 100% duty is the cliff,
 * not a place to operate. At the cliff every read must land perfectly and the
 * first jittery one starves the transport.
 *
 * WHAT THE CLAMP DOES *NOT* HAVE TO COVER, and this is the part that makes
 * scratching affordable at all: a gesture that oscillates INSIDE the resident
 * ring costs nothing. The ring holds ST_LAT_RING_SLOTS groups per stem, so a
 * head may move freely across ST_SCRATCH_FREE_WINDOW_US of audio -- forward,
 * backward, repeatedly -- without a single new read. A real scratch is mostly
 * this. The clamp exists for the part of a gesture that TRAVELS, which is the
 * shuttle, and for a scratch wide enough to leave the ring.
 *
 * tests/test_scratch.c does not take that on faith: it drives a synthetic
 * oscillating gesture through the REAL st_stem_stream state machine and counts
 * the sectors actually demanded, so the free-window claim is measured against
 * the production ring rather than asserted here.
 */

#ifndef STEMTAPE_PLAYER_SCRATCH_H_
#define STEMTAPE_PLAYER_SCRATCH_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_latency.h"
#include "st_planar.h"
#include "st_v11_format.h"

/* ======================================================================
 * THE MEASURED READ COST
 * ======================================================================
 * us = FIXED + PER_BLOCK * blocks, fitted on real hardware. PER_BLOCK is kept
 * in Q8 because 158.4 is not an integer and rounding it to 158 understates
 * every cost below -- understating a cost is the unsafe direction for a
 * starvation clamp, so the fraction is carried rather than dropped.
 */
#define ST_SCRATCH_RC_FIXED_US      649u
#define ST_SCRATCH_RC_PER_BLOCK_Q8  40550u   /* 158.4 * 256, rounded up */

/* One stem's refill batch: R groups, each ST_PL_GROUP_BLOCKS blocks. */
#define ST_SCRATCH_BATCH_BLOCKS \
	(ST_LAT_REFILL_GROUPS * ST_PL_GROUP_BLOCKS)

#define ST_SCRATCH_BATCH_US \
	(ST_SCRATCH_RC_FIXED_US + \
	 ((ST_SCRATCH_BATCH_BLOCKS * ST_SCRATCH_RC_PER_BLOCK_Q8) + 255u) / 256u)

/* Audio that batch covers, at unity. */
#define ST_SCRATCH_BATCH_COVER_US \
	(ST_LAT_REFILL_GROUPS * ST_LAT_SECTOR_US)

/* The duty budget, as a percentage. Deliberately the SAME margin the read-ahead
 * depth uses, so there is one notion of "how close to the cliff we operate". */
#define ST_SCRATCH_BUDGET_PCT (100u - ST_LAT_MARGIN_PCT)

/*
 * THE CLAMP, in Q16, for a gesture in which `moving` heads travel at the
 * clamped rate while the remaining heads run forward at unity.
 *
 *   moving * BATCH_US * r + (STEMS - moving) * BATCH_US * 1
 *       <= BUDGET_PCT/100 * BATCH_COVER_US
 *
 * solved for r. The still heads are charged their unity cost because they are
 * genuinely still reading -- an isolated stem scratch does not pause the other
 * three, which is the entire point of it.
 */
#define ST_SCRATCH_STILL_US(moving) \
	((uint32_t)(ST_PL_STEMS - (moving)) * ST_SCRATCH_BATCH_US)

#define ST_SCRATCH_BUDGET_US \
	((ST_SCRATCH_BUDGET_PCT * ST_SCRATCH_BATCH_COVER_US) / 100u)

#define ST_SCRATCH_MAX_RATE_Q16(moving) \
	((uint32_t)(((uint64_t)(ST_SCRATCH_BUDGET_US - ST_SCRATCH_STILL_US(moving)) << 16) / \
		     ((uint64_t)(moving) * ST_SCRATCH_BATCH_US)))

/* The two the firmware actually uses. MASTER moves all four; a stem scratch
 * moves one and leaves three at unity. */
#define ST_SCRATCH_MAX_RATE_MASTER_Q16 ST_SCRATCH_MAX_RATE_Q16(ST_PL_STEMS)
#define ST_SCRATCH_MAX_RATE_STEM_Q16   ST_SCRATCH_MAX_RATE_Q16(1u)

/*
 * THE FREE WINDOW: audio a head may range over without a new read, because the
 * ring already holds it. One slot is the one the consumer sits in, so the
 * travel available either side of it is (G - 1) sectors.
 */
#define ST_SCRATCH_FREE_WINDOW_US \
	((ST_LAT_RING_SLOTS - 1u) * ST_LAT_SECTOR_US)

#if !defined(__cplusplus)
_Static_assert(ST_SCRATCH_BUDGET_US > ST_SCRATCH_STILL_US(1u),
	       "the three still heads must not already exhaust the duty budget");
_Static_assert(ST_SCRATCH_MAX_RATE_MASTER_Q16 > 65536u,
	       "a master gesture must at least reach unity, or it cannot shuttle at all");
_Static_assert(ST_SCRATCH_MAX_RATE_STEM_Q16 > ST_SCRATCH_MAX_RATE_MASTER_Q16,
	       "moving one head must be cheaper than moving four");
#endif

#endif /* STEMTAPE_PLAYER_SCRATCH_H_ */
