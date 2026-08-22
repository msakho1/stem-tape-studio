/*
 * st_track_hold.c — see st_track_hold.h.
 */

#include "st_track_hold.h"

void st_track_hold_reset(st_track_hold_t *s)
{
	s->solo_active = false;
}

bool st_track_hold_tick(st_track_hold_t *s, bool pressed, uint32_t held_ms, uint32_t threshold_ms)
{
	if (!pressed) {
		/* Momentary: release unconditionally clears, whether or not
		 * the threshold was ever reached this press. */
		s->solo_active = false;
		return false;
	}
	if (!s->solo_active && held_ms >= threshold_ms) {
		s->solo_active = true;
	}
	return s->solo_active;
}
