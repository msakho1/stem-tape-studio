/*
 * st_inertia.c -- see st_inertia.h for what this is and why it is an
 * envelope rather than a rate.
 */
#include <string.h>

#include "st_inertia.h"

#define CURVE_STEPS 32u   /* 33 entries: index 0..32 */

/*
 * SPIN-UP PROGRESS: smoothstep, 3t^2 - 2t^3, in Q16.
 *
 * Zero slope at BOTH ends, which is the whole point. Leaving zero it starts
 * gently, so the reel does not snap into motion; arriving at nominal it flattens,
 * so the pitch contour has no corner at the top. Steepest through the middle,
 * where "coming up to speed" actually happens.
 *
 * Generated from the closed form and rounded, not hand-drawn -- but it is a
 * TABLE precisely so it can be hand-adjusted afterwards by ear. Reshaping the
 * feel means editing numbers here; nothing else in the module needs to know.
 */
static const uint32_t k_spinup[CURVE_STEPS + 1u] = {
	     0,    188,    736,   1620,   2816,   4300,   6048,   8036,
	 10240,  12636,  15200,  17908,  20736,  23660,  26656,  29700,
	 32768,  35836,  38880,  41876,  44800,  47628,  50336,  52900,
	 55296,  57500,  59488,  61236,  62720,  63916,  64800,  65348,
	 65536,
};

/*
 * SPIN-DOWN PROGRESS: 1 - 2^(-6t), in Q16.
 *
 * Steep immediately -- the transport loses speed the moment the button is
 * released, which is what stops it feeling like a fade -- then flattening into
 * a long crawl. By t = 0.5 the envelope is already down to 0.111 of nominal --
 * just over three octaves -- and the remaining half of the ramp is spent covering the last
 * three octaves slowly. That asymmetry is the classic tape stop: most of the
 * TIME is spent at low speed, which is where the ear hears the drama, because
 * pitch is logarithmic and every halving is another octave.
 */
static const uint32_t k_spindown[CURVE_STEPS + 1u] = {
	     0,   8114,  15239,  21496,  26990,  31814,  36051,  39771,
	 43038,  45907,  48426,  50638,  52580,  54286,  55784,  57099,
	 58254,  59268,  60159,  60941,  61628,  62231,  62761,  63226,
	 63634,  63993,  64307,  64584,  64827,  65040,  65227,  65392,
	 65536,
};

/* Linear interpolation into a 33-entry Q16 progress table. `t_q16` is the
 * ramp's normalized position, 0..65536. */
static uint32_t curve_at(const uint32_t *tbl, uint32_t t_q16)
{
	uint32_t idx, frac, a, b;

	if (t_q16 >= ST_INERTIA_ONE) {
		return ST_INERTIA_ONE;
	}
	/* 65536 / 32 = 2048 per cell. */
	idx  = t_q16 / 2048u;
	frac = t_q16 - idx * 2048u;
	/* t_q16 < ONE above, so idx <= 31 and tbl[idx + 1] is always in range;
	 * the table has CURVE_STEPS + 1 entries for exactly that reason. */
	a = tbl[idx];
	b = tbl[idx + 1u];
	return a + ((b - a) * frac) / 2048u;
}

void st_inertia_reset(st_inertia_t *s)
{
	memset(s, 0, sizeof(*s));
	s->state = ST_INERTIA_STOPPED;
}

/* Frames a full-travel ramp takes, scaled by the distance actually left to
 * cover. Catching a reel already at 0.8 must not take as long as starting
 * from rest -- that would be a transport with the wrong amount of mass. */
static uint32_t ramp_frames(uint32_t ms, uint32_t sample_rate,
			     uint32_t from_q16, uint32_t to_q16)
{
	uint32_t full = (uint32_t)(((uint64_t)ms * sample_rate) / 1000u);
	uint32_t dist = (from_q16 > to_q16) ? (from_q16 - to_q16)
					     : (to_q16 - from_q16);
	uint32_t n = (uint32_t)(((uint64_t)full * dist) / ST_INERTIA_ONE);

	return (n < ST_INERTIA_MIN_RAMP_FRAMES) ? ST_INERTIA_MIN_RAMP_FRAMES : n;
}

void st_inertia_play(st_inertia_t *s, uint32_t sample_rate)
{
	if (s->state == ST_INERTIA_RUNNING || s->state == ST_INERTIA_SPINUP) {
		return;   /* already there or already on the way */
	}
	s->from_q16 = s->env_q16;          /* CATCH the reel where it is */
	s->to_q16   = ST_INERTIA_ONE;
	s->total    = ramp_frames(ST_INERTIA_SPINUP_MS, sample_rate,
				   s->from_q16, s->to_q16);
	s->elapsed  = 0u;
	s->state    = ST_INERTIA_SPINUP;
}

void st_inertia_stop(st_inertia_t *s, uint32_t sample_rate)
{
	if (s->state == ST_INERTIA_STOPPED || s->state == ST_INERTIA_SPINDOWN) {
		return;
	}
	s->from_q16 = s->env_q16;
	s->to_q16   = 0u;
	s->total    = ramp_frames(ST_INERTIA_SPINDOWN_MS, sample_rate,
				   s->from_q16, s->to_q16);
	s->elapsed  = 0u;
	s->state    = ST_INERTIA_SPINDOWN;
}

void st_inertia_advance(st_inertia_t *s, uint32_t frames)
{
	uint32_t t_q16, prog;

	switch (s->state) {
	case ST_INERTIA_STOPPED:
		s->env_q16 = 0u;
		return;
	case ST_INERTIA_RUNNING:
		s->env_q16 = ST_INERTIA_ONE;
		return;
	default:
		break;
	}

	/* RECOMPUTED FROM elapsed, NOT INTEGRATED. The audio thread renders in
	 * runs whose length varies with sector and block boundaries, so an
	 * integrating envelope would take a different path for the same ramp
	 * depending on how the run lengths happened to fall. Deriving it from
	 * the elapsed count makes 256 single-frame advances and one 256-frame
	 * advance produce exactly the same envelope. */
	s->elapsed += frames;
	if (s->elapsed >= s->total) {
		s->elapsed = s->total;
		t_q16 = ST_INERTIA_ONE;
	} else {
		t_q16 = (uint32_t)(((uint64_t)s->elapsed * ST_INERTIA_ONE) /
				    s->total);
	}

	prog = curve_at((s->state == ST_INERTIA_SPINUP) ? k_spinup : k_spindown,
			 t_q16);

	if (s->to_q16 >= s->from_q16) {
		s->env_q16 = s->from_q16 +
			     (uint32_t)(((uint64_t)(s->to_q16 - s->from_q16) *
					  prog) / ST_INERTIA_ONE);
	} else {
		s->env_q16 = s->from_q16 -
			     (uint32_t)(((uint64_t)(s->from_q16 - s->to_q16) *
					  prog) / ST_INERTIA_ONE);
	}

	if (s->state == ST_INERTIA_SPINUP) {
		if (s->elapsed >= s->total) {
			s->env_q16 = ST_INERTIA_ONE;
			s->state   = ST_INERTIA_RUNNING;
		}
	} else {
		/* AT REST is a threshold, not an asymptote. The decay curve
		 * approaches zero without arriving, and waiting for exact zero
		 * would leave the transport crawling inaudibly forever. Below
		 * the rest threshold the reel has stopped. */
		if (s->env_q16 <= ST_INERTIA_REST_Q16 ||
		    s->elapsed >= s->total) {
			s->env_q16 = 0u;
			s->state   = ST_INERTIA_STOPPED;
		}
	}
}
