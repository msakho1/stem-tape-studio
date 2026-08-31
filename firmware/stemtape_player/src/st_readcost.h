/*
 * st_readcost.h -- how a sector read's cost splits into a FIXED part and a
 * part that SCALES with size, and what that implies for per-track reverse.
 *
 * ======================================================================
 * WHY THIS EXISTS
 * ======================================================================
 * Per-track reverse playback needs a reversed stem to read from a different
 * position than the forward transport. In the v1.1 sector layout that is
 * impossible to afford: all four stems are interleaved in one 24-byte frame
 * (st_sector_v11.h), so a per-track position costs a WHOLE EXTRA SECTOR
 * STREAM, and st_latency.h's own measured figures leave nowhere near enough
 * room for a second one:
 *
 *     one sector holds     ST_LAT_SECTOR_US   = 7083 us of audio
 *     a typical read costs ST_LAT_READ_TYP_US = 5073 us
 *     so forward playback alone already uses 5073/7083 = 71.6% of the
 *     read engine, and a second stream would need another 71.6%.
 *
 * 143% does not fit in 100%. The interesting question is therefore whether a
 * SMALLER read costs proportionally less -- because if it does, storing each
 * stem's samples in its own contiguous plane lets a reversed stem fetch only
 * its own quarter of the data, and the feature becomes affordable.
 *
 * ======================================================================
 * THE QUESTION, STATED AS TWO HYPOTHESES
 * ======================================================================
 * The 5073 us read decomposes, from a real boot capture recorded in
 * docs/stem-tape-playback-physical-test.md, as:
 *
 *     SPIM3 DMA         2056 us   514 B x 16 blocks at 32 MHz
 *     start-bit hunt    1763 us   bit-banged
 *     CRC + copy-out    1104 us   per byte
 *     CMD18/CMD12        150 us   one handshake per READ
 *
 * DMA and CRC obviously scale with size. The handshake obviously does not.
 * The start-bit hunt is the one that decides the outcome, and it is worth
 * 1763 us -- 35% of the read:
 *
 *   HYPOTHESIS A, the hunt is PER BLOCK. Fixed cost is just the 150 us
 *   handshake, 3% of a read. A quarter-size read costs ~1381 us, and four
 *   independent per-stem planes cost 5523 us against a 7083 us budget --
 *   78% duty. PER-TRACK REVERSE IS AFFORDABLE.
 *
 *   HYPOTHESIS B, the hunt is PER READ. Fixed cost is 1913 us, 38% of a
 *   read. A quarter-size read costs ~2703 us and four planes cost 10812 us
 *   -- 153% duty. PER-TRACK REVERSE IS IMPOSSIBLE, and no amount of layout
 *   work changes that.
 *
 * MEASURED, firmware st32, 24 reads per size, uncontended: the hunt went
 * 31 us at 4 blocks to 109 us at 16 -- a ratio of 3.52 against the 4.0 that
 * per-block predicts and the 1.0 that per-read predicts. HYPOTHESIS A HOLDS
 * and per-track reverse is affordable at 75.7% duty with all four reversed.
 *
 * Two corrections the measurement forced. A full sector read is 3152 us
 * uncontended, not the 5073 us of ST_LAT_READ_TYP_US -- that figure is from a
 * CONTENDED boot capture, is a different quantity, and is deliberately left
 * alone because the read-ahead depth is sized from it. And the fixed cost is
 * 649 us, not the 150 us predicted here: there is ~500 us more per-read
 * overhead than the phase breakdown accounted for. The verdict survives for a
 * different reason than predicted -- not a negligible fixed cost, but a read
 * fast enough to pay it four times.
 *
 * sp1_emmc.c's emmc_read_blocks() puts the hunt inside its per-block loop and
 * its own comments call the 80 ms access hunt "every per-block bound", which
 * says A. That is a strong argument and it is NOT a measurement. A sector
 * layout change re-encodes every stored song and cannot be walked back
 * cheaply, so the firmware sweep that feeds this module exists to settle it
 * on the actual card rather than on a reading of the driver.
 *
 * ======================================================================
 * WHAT THIS MODULE IS
 * ======================================================================
 * The pure arithmetic only: fit a straight line through measured (blocks, us)
 * pairs, and turn the fit into the duty-cycle answer. No I/O, no clock, so
 * the reasoning that a format change would rest on is host-testable against
 * the recorded capture AND against both hypotheses before anyone flashes
 * anything.
 */

#ifndef STEMTAPE_PLAYER_READCOST_H_
#define STEMTAPE_PLAYER_READCOST_H_

#include <stdbool.h>
#include <stdint.h>

/* Blocks in one 8192-byte sector, and stems sharing it. A per-stem plane is
 * ST_RC_SECTOR_BLOCKS / ST_RC_STEMS blocks. */
#define ST_RC_SECTOR_BLOCKS 16u
#define ST_RC_STEMS          4u
#define ST_RC_PLANE_BLOCKS  (ST_RC_SECTOR_BLOCKS / ST_RC_STEMS)

/* Audio one sector holds, us. Mirrors ST_LAT_SECTOR_US; kept as its own
 * constant so this module stays free of the latency header and can be host
 * built on its own. A static assert in the test pins them equal. */
#define ST_RC_SECTOR_US 7083u

/* The fit, in Q8 microseconds so a fractional per-block slope survives
 * without floating point on the target. */
typedef struct {
	uint32_t fixed_us_q8;     /* cost paid once per read, whatever its size */
	uint32_t per_block_us_q8; /* additional cost of each 512-byte block */
	bool     valid;           /* false when the input could not be fitted */
} st_readcost_t;

/*
 * LEAST-SQUARES FIT of us = fixed + per_block * blocks over `n` samples.
 *
 * Least squares rather than two endpoints: a single anomalous read at one
 * size would swing a two-point estimate completely, and the whole point of
 * sweeping several sizes is that no one of them is trusted alone.
 *
 * Returns false (and marks the result invalid) if fewer than two distinct
 * block counts were supplied, or if the fit comes out with a NEGATIVE slope
 * -- bigger reads costing less is not a noisy measurement, it is a broken
 * one, and silently reporting it as a very small fixed cost would argue for
 * exactly the format change this module exists to gate.
 */
bool st_readcost_fit(const uint32_t *blocks, const uint32_t *us, uint32_t n,
		      st_readcost_t *out);

/* The fitted cost of a read of `blocks` blocks, in whole microseconds. */
uint32_t st_readcost_predict_us(const st_readcost_t *rc, uint32_t blocks);

/*
 * THE ANSWER, in parts per million of the read engine.
 *
 * `n_reversed` stems read their own plane from their own position; the rest
 * are contiguous in the forward sector and are fetched in ONE read, which is
 * why zero reversed tracks costs exactly what playback costs today and the
 * price of the feature is only paid when tracks actually diverge.
 *
 * 1000000 ppm is the whole read engine. Anything at or above that is a
 * sustained deficit, which on this device does not present as dropouts but as
 * the song playing slow and crushed.
 */
uint32_t st_readcost_planar_duty_ppm(const st_readcost_t *rc,
				      uint32_t n_reversed);

/*
 * ======================================================================
 * THE PLANAR READ PLAN -- what the streamer does to SIMULATE v1.2 cost
 * ======================================================================
 * The duty figures above are arithmetic. Whether the CPU can actually give
 * the streamer that share during live playback is a different question, and
 * the honest way to answer it is not to compute a percentage but to make the
 * streamer do the work and see if the audio survives.
 *
 * With all four tracks diverging, v1.2 fetches four 4-block planes from four
 * unrelated positions per sector-time. The simulation reproduces that COST --
 * four read commands, sixteen blocks total -- while reading the SAME sector,
 * so the bytes landing in the buffer are identical and the audio is
 * bit-identical. Only the read pattern changes, which is exactly the variable
 * under test.
 *
 * DESCENDING ORDER, deliberately. Read the quarters back to front so the
 * card's own sequential read-ahead cannot flatter the result: in real v1.2
 * the four planes are at unrelated song positions and get no such help.
 *
 * The plan is a pure function because a gap or an overlap in it would corrupt
 * audio silently -- the buffer would hold one quarter twice and another never
 * -- and that is worth proving on the host rather than discovering by ear.
 */
#define ST_RC_PLAN_MAX 4u

typedef struct {
	uint32_t block_off;  /* blocks from the sector's first block */
	uint32_t buf_off;    /* bytes from the start of the sector buffer */
	uint32_t blocks;     /* blocks this read covers */
} st_rc_read_t;

/* Fills `out` with the plan and returns how many reads it contains. */
uint32_t st_readcost_plan_planar(st_rc_read_t out[ST_RC_PLAN_MAX]);

/* True when `n_reversed` diverging tracks fit inside the read engine with the
 * given headroom left spare, in ppm. */
static inline bool st_readcost_fits(const st_readcost_t *rc,
				     uint32_t n_reversed,
				     uint32_t headroom_ppm)
{
	const uint32_t duty = st_readcost_planar_duty_ppm(rc, n_reversed);

	return rc->valid && duty + headroom_ppm <= 1000000u;
}

#endif /* STEMTAPE_PLAYER_READCOST_H_ */
