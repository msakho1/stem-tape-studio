/*
 * st_track_chord.c — see st_track_chord.h for the ladder model, the one
 * measured chord band, the T3+T4 hardware finding and the fail-safe band
 * policy. PURE: no Zephyr, no ADC, no clock.
 */

#include "st_track_chord.h"

/*
 * THE BAND TABLE. Ascending by voltage, which is also ascending by mask
 * weight -- see the header: adding a button always raises the reading.
 *
 * `lo`/`hi` keep only the central ~42% of each gap to the neighbouring
 * centre. The unclaimed remainder is guard zone, where a reading holds the
 * previous settled mask instead of naming a new one. Centres are listed per
 * row so a bench measurement can be compared against the prediction directly.
 *
 * Every chord containing BOTH T3 and T4 is absent on purpose: those four
 * masks land at or above ST_CHORD_PLAY_FLOOR, on top of the transport
 * control. See the header. Do not add them without new hardware evidence
 * that the ladder differs from the pinned looper's.
 */
const st_chord_band_t st_chord_bands[] = {
	/*  lo    hi   mask                              centre  provenance     */
	{  129,  253, ST_CHORD_T1                     }, /*  213  measured single */
	{  363,  440, ST_CHORD_T2                     }, /*  403  measured single */
	{  541,  610, ST_CHORD_T1 | ST_CHORD_T2       }, /*  578  model           */
	{  700,  764, ST_CHORD_T3                     }, /*  733  measured single */
	{  849,  907, ST_CHORD_T1 | ST_CHORD_T3       }, /*  879  model           */
	{  984, 1038, ST_CHORD_T2 | ST_CHORD_T3       }, /* 1012  model           */
	{ 1110, 1154, ST_CHORD_T1|ST_CHORD_T2|ST_CHORD_T3 }, /* 1136  model       */
	{ 1202, 1243, ST_CHORD_T4                     }, /* 1220  measured single */
	{ 1306, 1350, ST_CHORD_T1 | ST_CHORD_T4       }, /* 1329  MEASURED chord:
	                                                  * the DFU failsafe band
	                                                  * 1280..1390, narrowed to
	                                                  * the model centre       */
	{ 1408, 1448, ST_CHORD_T2 | ST_CHORD_T4       }, /* 1429  model           */
	{ 1503, 1541, ST_CHORD_T1|ST_CHORD_T2|ST_CHORD_T4 }, /* 1523  model       */
};

const uint32_t st_chord_band_count =
	(uint32_t)(sizeof(st_chord_bands) / sizeof(st_chord_bands[0]));

void st_track_chord_reset(st_track_chord_t *c)
{
	c->settled    = 0u;
	c->cand       = 0u;
	c->cand_count = 0u;
	c->have_last  = false;
	c->last_raw   = 0;
}

bool st_track_chord_lookup(int raw, uint8_t settled_mask, uint8_t *mask_out)
{
	uint32_t i;

	/* Idle settles instantly and unconditionally -- see the header on why
	 * release is not debounced like press. */
	if (raw <= ST_CHORD_IDLE_MAX) {
		*mask_out = 0u;
		return true;
	}

	/* At or above PLAY's floor this reading is the transport's, or one of
	 * the four masks that collide with it. Either way it is not a chord. */
	if (raw >= ST_CHORD_PLAY_FLOOR) {
		return false;
	}

	for (i = 0; i < st_chord_band_count; i++) {
		int lo = (int)st_chord_bands[i].lo;
		int hi = (int)st_chord_bands[i].hi;

		/* A mask that is ALREADY settled keeps a slightly wider band, so a
		 * held chord resting near its own edge cannot flicker out. Only the
		 * settled mask is widened; every other band keeps its narrow,
		 * fail-safe width. */
		if (settled_mask != 0u && st_chord_bands[i].mask == settled_mask) {
			lo -= ST_CHORD_HYSTERESIS;
			hi += ST_CHORD_HYSTERESIS;
		}
		if (raw >= lo && raw <= hi) {
			*mask_out = st_chord_bands[i].mask;
			return true;
		}
	}
	return false;   /* guard zone: unclaimed by design */
}

uint8_t st_track_chord_update(st_track_chord_t *c, int raw)
{
	uint8_t seen;
	int delta;

	/* SLEW GUARD. A finger travelling between two chords crosses several
	 * legal bands; suppressing everything while the reading is still moving
	 * is what stops those transit states from being committed as chords the
	 * player never formed. Idle is exempt: a release must not be delayed by
	 * the very fact that it is a fast movement. */
	if (c->have_last && raw > ST_CHORD_IDLE_MAX) {
		delta = raw - c->last_raw;
		if (delta < 0) {
			delta = -delta;
		}
		if (delta > ST_CHORD_SLEW_MAX) {
			c->last_raw   = raw;
			c->cand       = c->settled;
			c->cand_count = 0u;
			return c->settled;
		}
	}
	c->last_raw  = raw;
	c->have_last = true;

	if (!st_track_chord_lookup(raw, c->settled, &seen)) {
		/* Guard zone, or PLAY's territory. HOLD -- never guess, and never
		 * let an unclaimed voltage clear a chord the player is still
		 * holding. */
		c->cand       = c->settled;
		c->cand_count = 0u;
		return c->settled;
	}

	if (seen == c->settled) {
		c->cand       = seen;
		c->cand_count = 0u;
		return c->settled;
	}

	/* Full release settles in one read (header: restoring the mix must be
	 * immediate). Everything else must earn its place. */
	if (seen == 0u) {
		c->settled    = 0u;
		c->cand       = 0u;
		c->cand_count = 0u;
		return 0u;
	}

	if (seen == c->cand) {
		c->cand_count++;
	} else {
		c->cand       = seen;
		c->cand_count = 1u;
	}
	if (c->cand_count >= ST_CHORD_SETTLE_READS) {
		c->settled    = seen;
		c->cand_count = 0u;
	}
	return c->settled;
}
