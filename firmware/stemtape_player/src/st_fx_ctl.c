/*
 * st_fx_ctl.c — see st_fx_ctl.h.
 */

#include "st_fx_ctl.h"

#include <string.h>

/* Filter -> Distortion -> Gate -> Echo, as button-order indices. */
const uint8_t st_fx_signal_order[ST_FX_COUNT] = {
	ST_FX_FILTER, ST_FX_DIRT, ST_FX_GATE, ST_FX_ECHO,
};

void st_fx_ctl_reset(st_fx_ctl_t *s)
{
	memset(s, 0, sizeof(*s));
	s->scope = ST_FX_SCOPE_STEM;
	s->chord = ST_FX_CHORD_IDLE;
}

/* Wrap-safe elapsed time. */
static uint32_t since(uint32_t now, uint32_t then)
{
	return now - then;
}

/*
 * THE CHORD. Returns nothing; it drives s->chord and writes the consumption
 * and fire flags into `out`.
 *
 * WHY A SINGLE VOLUME PRESS IS WITHHELD. On a device with a real scan loop
 * there is no way to know whether a press is a lone volume step or the first
 * half of a chord until either the second button arrives or the window
 * expires. Dispatching immediately and "taking it back" would emit an audible
 * volume step before every FX entry. So the press is held for at most
 * ST_FX_CHORD_ARRIVAL_MS and then dispatched -- which is exactly why that
 * constant is 120 and not 400.
 */
static void chord_service(st_fx_ctl_t *s, const st_fx_in_t *in, st_fx_out_t *out)
{
	const bool m = in->vol_minus_down;
	const bool p = in->vol_plus_down;
	const bool m_edge = m && !s->prev_minus;
	const bool p_edge = p && !s->prev_plus;
	const bool any = m || p;
	const bool both = m && p;

	switch (s->chord) {
	case ST_FX_CHORD_IDLE:
		if (both) {
			/* Both seen in the SAME scan: arrival is zero. */
			s->first_down_ms = in->now_ms;
			s->second_down_ms = in->now_ms;
			s->first_was_minus = true;
			s->chord_fn = in->function_down;
			s->chord = ST_FX_CHORD_ARMED;
		} else if (m_edge || p_edge) {
			s->first_down_ms = in->now_ms;
			s->first_was_minus = m_edge;
			/* SCOPE IS LATCHED AT CHORD BEGIN, not at release. A
			 * FUNCTION press that arrives after the chord has begun
			 * must not retroactively turn a STEM entry into a
			 * GLOBAL one. */
			s->chord_fn = in->function_down;
			s->chord = ST_FX_CHORD_ONE_DOWN;
		}
		break;

	case ST_FX_CHORD_ONE_DOWN:
		if (both) {
			if (since(in->now_ms, s->first_down_ms) <= ST_FX_CHORD_ARRIVAL_MS) {
				s->second_down_ms = in->now_ms;
				s->chord = ST_FX_CHORD_ARMED;
			} else {
				/* Arrival too wide: not a chord. Both buttons
				 * behave as ordinary presses. */
				s->chord = ST_FX_CHORD_SINGLE;
			}
		} else if (!any) {
			/* Released inside the window: an ordinary short tap. */
			if (s->first_was_minus) out->vol_minus_fire = true;
			else                    out->vol_plus_fire = true;
			s->chord = ST_FX_CHORD_IDLE;
		} else if (since(in->now_ms, s->first_down_ms) > ST_FX_CHORD_ARRIVAL_MS) {
			/* Window expired with one button down: dispatch it now,
			 * once, and stop withholding. */
			if (s->first_was_minus) out->vol_minus_fire = true;
			else                    out->vol_plus_fire = true;
			s->chord = ST_FX_CHORD_SINGLE;
		}
		break;

	case ST_FX_CHORD_SINGLE:
		/* A second button arriving now is its own ordinary press. */
		if (m_edge) out->vol_minus_fire = true;
		if (p_edge) out->vol_plus_fire = true;
		if (!any) {
			s->chord = ST_FX_CHORD_IDLE;
		}
		break;

	case ST_FX_CHORD_ARMED: {
		uint32_t overlap = since(in->now_ms, s->second_down_ms);

		if (overlap >= ST_FX_CHORD_PAIRING_MS) {
			out->pairing = true;
			s->chord = ST_FX_CHORD_WAIT_RELEASE;
			break;
		}
		if (both) {
			break;   /* still held, still deciding */
		}
		/* FIRST RELEASE resolves the chord. */
		if (overlap < ST_FX_CHORD_RELEASE_MS) {
			if (s->open) {
				s->open = false;
				out->closed = true;
				/* Latches and scope are NOT cleared: latched
				 * effects keep sounding in the rack's last
				 * scope, and reopening restores them. */
			} else {
				s->open = true;
				s->scope = s->chord_fn ? ST_FX_SCOPE_GLOBAL
						       : ST_FX_SCOPE_STEM;
				out->opened = true;
			}
		} else {
			out->ambiguous = true;   /* 600..2000 ms band */
		}
		s->chord = ST_FX_CHORD_WAIT_RELEASE;
		break;
	}

	case ST_FX_CHORD_WAIT_RELEASE:
		/* Exactly one resolution per chord. Nothing fires again -- not a
		 * re-press, not contact bounce -- until BOTH buttons have been
		 * seen up. */
		if (!any) {
			s->chord = ST_FX_CHORD_IDLE;
		}
		break;
	}

	/* Whatever the outcome, a Volume button that is down while the chord
	 * machine owns it is claimed: no master-volume step may leak. The two
	 * fire flags above are the only way an ordinary volume action escapes. */
	if (s->chord != ST_FX_CHORD_IDLE) {
		if (m) out->vol_minus_consumed = true;
		if (p) out->vol_plus_consumed = true;
	}
	if (s->chord == ST_FX_CHORD_ARMED || s->chord == ST_FX_CHORD_WAIT_RELEASE) {
		if (s->chord_fn) {
			out->function_consumed = true;
		}
	}
}

/*
 * TARGET WALKING. FUNCTION + one Volume, STEM scope only. Fires on the press
 * once the chord window has ruled out a two-volume chord, so it can never be
 * confused with overlay entry. One deliberate press = exactly one stem; there
 * is no repeat, because the contract defines no repeat timing for it.
 */
static void target_service(st_fx_ctl_t *s, const st_fx_in_t *in, st_fx_out_t *out)
{
	if (!s->open || s->scope != ST_FX_SCOPE_STEM || !in->function_down) {
		return;
	}
	if (!out->vol_minus_fire && !out->vol_plus_fire) {
		return;
	}
	/* The volume press belongs to the overlay, not to master volume. */
	if (out->vol_minus_fire) {
		s->target_stem = (uint8_t)((s->target_stem + ST_FX_STEM_COUNT - 1u)
					    % ST_FX_STEM_COUNT);
	} else {
		s->target_stem = (uint8_t)((s->target_stem + 1u) % ST_FX_STEM_COUNT);
	}
	out->vol_minus_fire = false;
	out->vol_plus_fire = false;
	out->vol_minus_consumed = in->vol_minus_down;
	out->vol_plus_consumed = in->vol_plus_down;
	out->function_consumed = true;
	out->target_changed = true;
}

/*
 * MOMENTARY AND LATCH.
 *
 *   FUNCTION already down, then Track down  -> latch toggle, no momentary.
 *   Track down with FUNCTION up             -> momentary on until release.
 *
 * The two are independent bits and `active = momentary | latch`, which is what
 * makes a momentary hold of an already-latched effect harmless: releasing the
 * button clears only the momentary bit.
 */
static void bank_service(st_fx_ctl_t *s, const st_fx_in_t *in, st_fx_out_t *out)
{
	uint8_t e;

	if (!s->open) {
		/* Closing the overlay ends momentary sound but keeps latches. */
		s->momentary = 0u;
		return;
	}

	for (e = 0; e < ST_FX_COUNT; e++) {
		const uint8_t bit = ST_FX_BIT(e);
		const bool now_down = (in->track_down & bit) != 0u;
		const bool was_down = (s->prev_track & bit) != 0u;

		if (now_down && !was_down) {
			if (in->function_down) {
				s->latch ^= bit;          /* toggle this one only */
				out->function_consumed = true;
			} else {
				s->momentary |= bit;
			}
			out->track_consumed |= bit;
		} else if (!now_down && was_down) {
			s->momentary &= (uint8_t)~bit;
			out->track_consumed |= bit;
		} else if (now_down) {
			/* Held: keep claiming it so no Track solo/chord handler
			 * sees a button the overlay owns. */
			out->track_consumed |= bit;
		}
	}
}

void st_fx_ctl_service(st_fx_ctl_t *s, const st_fx_in_t *in, st_fx_out_t *out)
{
	memset(out, 0, sizeof(*out));

	chord_service(s, in, out);
	target_service(s, in, out);
	bank_service(s, in, out);

	out->fx_open = s->open;
	out->scope = s->scope;
	out->target_stem = s->target_stem;
	out->momentary_mask = s->momentary;
	out->latch_mask = s->latch;
	out->active_mask = (uint8_t)(s->momentary | s->latch);

	s->prev_minus = in->vol_minus_down;
	s->prev_plus = in->vol_plus_down;
	s->prev_function = in->function_down;
	s->prev_track = in->track_down;
}
