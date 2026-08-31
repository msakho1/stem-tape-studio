/*
 * st_ladder.c — see st_ladder.h for the measured provenance, the band rule,
 * and why the st15 slew guard is gone. PURE: no Zephyr, no ADC, no clock.
 */

#include "st_ladder.h"

/*
 * THE BAND TABLE. Ascending by voltage -- which on this hardware is also
 * ascending by mask value, so row index == mask for every Track row.
 *
 * Every lo/hi below is centre +/- min(25, 40% of the nearest neighbour gap),
 * from the physical capture in docs/ladder-measured.json. The centres are in
 * the comment column so a bench reading can be compared directly;
 * tests/test_ladder.c re-derives the whole table from those centres and fails
 * if a row here has been hand-edited away from the measurement.
 *
 * PLAY's row is open-ended upward: nothing on this ladder reads higher than
 * PLAY, so a reading above its centre is PLAY, not an unclaimed voltage.
 */
const st_ladder_band_t st_ladder_bands[ST_LADDER_ROWS] = {
	/*   lo    hi   mask play        centre  combination        */
	{  180,  230,  0x1u, 0u },   /*   205   T1                  */
	{  375,  425,  0x2u, 0u },   /*   400   T2                  */
	{  545,  595,  0x3u, 0u },   /*   570   T1+T2               */
	{  702,  752,  0x4u, 0u },   /*   727   T3                  */
	{  843,  893,  0x5u, 0u },   /*   868   T1+T3               */
	{  968, 1018,  0x6u, 0u },   /*   993   T2+T3               */
	{ 1079, 1129,  0x7u, 0u },   /*  1104   T1+T2+T3            */
	{ 1188, 1238,  0x8u, 0u },   /*  1213   T4                  */
	{ 1284, 1334,  0x9u, 0u },   /*  1309   T1+T4               */
	{ 1371, 1421,  0xAu, 0u },   /*  1396   T2+T4               */
	{ 1455, 1505,  0xBu, 0u },   /*  1480   T1+T2+T4            */
	{ 1534, 1584,  0xCu, 0u },   /*  1559   T3+T4               */
	{ 1603, 1653,  0xDu, 0u },   /*  1628   T1+T3+T4            */
	{ 1671, 1719,  0xEu, 0u },   /*  1695   T2+T3+T4            */
	{ 1732, 1778,  0xFu, 0u },   /*  1755   all four            */
	{ 1790, 4095,  0x0u, 1u },   /*  1813   PLAY                */
};

void st_ladder_reset(st_ladder_t *l)
{
	l->settled_mask = 0u;
	l->settled_play = false;
	l->cand_cls     = ST_LADDER_IDLE;
	l->cand_mask    = 0u;
	l->cand_count   = 0u;
}

st_ladder_read_t st_ladder_classify(int raw, uint8_t settled_mask, bool settled_play)
{
	st_ladder_read_t r = { ST_LADDER_UNKNOWN, 0u };
	uint32_t i;

	/* ADC error. NOT idle: a single failed conversion must not drop a chord
	 * the player is still physically holding. */
	if (raw < 0) {
		return r;
	}

	/* Below the floor is idle. CLASSIFICATION is unconditional; the
	 * ST_LADDER_RELEASE_READS confirmation that actually clears a held
	 * state lives in st_ladder_update(), which is the only place any
	 * debounce belongs. */
	if (raw <= ST_LADDER_IDLE_MAX) {
		r.cls = ST_LADDER_IDLE;
		return r;
	}

	for (i = 0u; i < ST_LADDER_ROWS; i++) {
		int lo = (int)st_ladder_bands[i].lo;
		int hi = (int)st_ladder_bands[i].hi;
		bool is_settled_row = st_ladder_bands[i].play
			? settled_play
			: (!settled_play && settled_mask != 0u &&
			   st_ladder_bands[i].mask == settled_mask);

		/* Only the settled row is widened; every other row keeps its
		 * narrow, fail-safe width. */
		if (is_settled_row) {
			lo -= ST_LADDER_HYSTERESIS;
			hi += ST_LADDER_HYSTERESIS;
		}
		if (raw >= lo && raw <= hi) {
			if (st_ladder_bands[i].play) {
				r.cls = ST_LADDER_PLAY;
			} else {
				r.cls  = ST_LADDER_TRACKS;
				r.mask = st_ladder_bands[i].mask;
			}
			return r;
		}
	}
	return r;   /* guard zone: unclaimed by design */
}

void st_ladder_update(st_ladder_t *l, int raw)
{
	st_ladder_read_t r = st_ladder_classify(raw, l->settled_mask, l->settled_play);

	if (r.cls == ST_LADDER_UNKNOWN) {
		/* A NON-VOTE, not a veto and not a vote against. Hold the
		 * settled state and leave the pending candidate exactly as it
		 * was.
		 *
		 * It carries no evidence for any particular state, so it must
		 * not be allowed to argue either way. Clearing the candidate
		 * would let 50%-duty coupling stall the decoder forever (the
		 * count would oscillate and never reach the settle threshold),
		 * which is the same class of deadlock the removed slew guard
		 * caused. Eroding it is the same bug with a longer fuse. And it
		 * cannot build a FALSE candidate either: only a positively
		 * in-band reading ever names one, so guard-zone noise can never
		 * accumulate agreement for a mask the player is not holding.
		 * Idle -- which IS evidence -- still clears everything below. */
		return;
	}

	if (r.cls == ST_LADDER_IDLE) {
		/* A CONFIRMED release, not a single sample. One idle reading is
		 * exactly what a coupled dip looks like, and dropping a held
		 * chord on it would make a two-finger solo flicker mid-phrase.
		 * ST_LADDER_RELEASE_READS agreeing idles clear it -- ~16 ms at
		 * main.c's cadence, well inside the response budget, and still
		 * far quicker than the press path because restoring the full
		 * four-stem mix must feel immediate. */
		if (l->settled_mask == 0u && !l->settled_play) {
			l->cand_cls   = ST_LADDER_IDLE;
			l->cand_mask  = 0u;
			l->cand_count = 0u;
			return;
		}
		if (l->cand_cls == ST_LADDER_IDLE) {
			l->cand_count++;
		} else {
			l->cand_cls   = ST_LADDER_IDLE;
			l->cand_mask  = 0u;
			l->cand_count = 1u;
		}
		if (l->cand_count >= ST_LADDER_RELEASE_READS) {
			l->settled_mask = 0u;
			l->settled_play = false;
			l->cand_count   = 0u;
		}
		return;
	}

	/* Already what we hold: nothing to earn. */
	if ((r.cls == ST_LADDER_PLAY && l->settled_play) ||
	    (r.cls == ST_LADDER_TRACKS && !l->settled_play &&
	     r.mask == l->settled_mask)) {
		l->cand_cls   = r.cls;
		l->cand_mask  = r.mask;
		l->cand_count = 0u;
		return;
	}

	if (r.cls == l->cand_cls && r.mask == l->cand_mask) {
		if (++l->cand_count >= ST_LADDER_SETTLE_READS) {
			l->settled_play = (r.cls == ST_LADDER_PLAY);
			l->settled_mask = (r.cls == ST_LADDER_PLAY) ? 0u : r.mask;
			l->cand_count   = 0u;
		}
	} else {
		l->cand_cls   = r.cls;
		l->cand_mask  = r.mask;
		l->cand_count = 1u;
	}
}
