/*
 * st_readcost.c -- see st_readcost.h for the two hypotheses this arithmetic
 * exists to tell apart, and why a sector layout change is gated on it.
 */

#include "st_readcost.h"

bool st_readcost_fit(const uint32_t *blocks, const uint32_t *us, uint32_t n,
		      st_readcost_t *out)
{
	int64_t sx = 0, sy = 0, sxy = 0, sxx = 0;
	int64_t den, slope_q8, icpt_q8;
	uint32_t i, distinct = 0;

	out->fixed_us_q8     = 0u;
	out->per_block_us_q8 = 0u;
	out->valid           = false;

	if (blocks == 0 || us == 0 || n < 2u) {
		return false;
	}

	for (i = 0; i < n; i++) {
		sx  += (int64_t)blocks[i];
		sy  += (int64_t)us[i];
		sxy += (int64_t)blocks[i] * (int64_t)us[i];
		sxx += (int64_t)blocks[i] * (int64_t)blocks[i];
		if (i == 0u || blocks[i] != blocks[0]) {
			distinct++;
		}
	}

	/* Two samples at the SAME size determine a slope no better than one
	 * sample does; the denominator below is exactly zero there, and
	 * dividing by it would invent an answer. */
	den = (int64_t)n * sxx - sx * sx;
	if (den == 0 || distinct < 2u) {
		return false;
	}

	slope_q8 = (((int64_t)n * sxy - sx * sy) << 8) / den;
	if (slope_q8 <= 0) {
		/* A bigger read costing less is a broken measurement, not a
		 * small fixed cost. Refuse rather than report a fit that would
		 * argue for the format change on nonsense. */
		return false;
	}

	icpt_q8 = ((sy << 8) - slope_q8 * sx) / (int64_t)n;
	if (icpt_q8 < 0) {
		/* A negative fixed cost is unphysical but IS a normal outcome
		 * of noise when the true fixed cost is near zero. Clamp to
		 * zero rather than refuse: it still says "essentially all of
		 * the cost scales", which is the answer being sought. */
		icpt_q8 = 0;
	}

	out->per_block_us_q8 = (uint32_t)slope_q8;
	out->fixed_us_q8     = (uint32_t)icpt_q8;
	out->valid           = true;
	return true;
}

uint32_t st_readcost_predict_us(const st_readcost_t *rc, uint32_t blocks)
{
	uint64_t q8;

	if (!rc->valid) {
		return 0u;
	}
	q8 = (uint64_t)rc->fixed_us_q8 +
	     (uint64_t)rc->per_block_us_q8 * (uint64_t)blocks;
	return (uint32_t)((q8 + 128u) >> 8);      /* round, do not truncate */
}

uint32_t st_readcost_planar_duty_ppm(const st_readcost_t *rc,
				      uint32_t n_reversed)
{
	uint32_t forward_planes, total_us = 0u;

	if (!rc->valid) {
		return 0u;
	}
	if (n_reversed > ST_RC_STEMS) {
		n_reversed = ST_RC_STEMS;
	}
	forward_planes = ST_RC_STEMS - n_reversed;

	/* The forward planes are contiguous in the forward sector, so however
	 * many there are they cost ONE read. This is the whole reason zero
	 * reversed tracks is free: the layout change does not make ordinary
	 * playback slower, it only adds a read per diverging track. */
	if (forward_planes > 0u) {
		total_us += st_readcost_predict_us(
			rc, forward_planes * ST_RC_PLANE_BLOCKS);
	}
	/* Each reversed stem is at its own position, so each is its own read
	 * of its own plane. */
	total_us += n_reversed * st_readcost_predict_us(rc, ST_RC_PLANE_BLOCKS);

	return (uint32_t)(((uint64_t)total_us * 1000000u) / ST_RC_SECTOR_US);
}

uint32_t st_readcost_plan_planar(st_rc_read_t out[ST_RC_PLAN_MAX])
{
	uint32_t i;

	/* Back to front -- see the header for why the order matters. */
	for (i = 0; i < ST_RC_STEMS; i++) {
		const uint32_t q = ST_RC_STEMS - 1u - i;

		out[i].block_off = q * ST_RC_PLANE_BLOCKS;
		out[i].buf_off   = q * ST_RC_PLANE_BLOCKS * 512u;
		out[i].blocks    = ST_RC_PLANE_BLOCKS;
	}
	return ST_RC_STEMS;
}
