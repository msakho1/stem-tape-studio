/*
 * st_loop.c — see st_loop.h for the grammar, the exit rule, the length
 * provenance and the deliberate direction divergence from surface.ts.
 * PURE: no Zephyr, no clock, no allocation, no floating point.
 */

#include "st_loop.h"

/*
 * The ladder, shortest to longest, as num/den BEATS. Index
 * ST_LOOP_LEN_DEFAULT (4) is one bar, which is both the product default and
 * surface.ts's own `division: 1` default.
 *
 *   0  1/4 beat      4  4 beats  == 1 bar   <- default
 *   1  1/2 beat      5  8 beats  == 2 bars
 *   2  1 beat        6  16 beats == 4 bars
 *   3  2 beats       7  32 beats == 8 bars
 *
 * Volume - steps toward index 0, Volume + toward index 7, and both CLAMP:
 * there is no wraparound, so a player leaning on one button lands on the
 * shortest or longest length and stays there rather than jumping across the
 * whole range.
 */
const st_loop_len_t st_loop_lengths[ST_LOOP_LEN_COUNT] = {
	{  1u, 4u },   /* 1/4 beat */
	{  1u, 2u },   /* 1/2 beat */
	{  1u, 1u },   /* 1 beat   */
	{  2u, 1u },   /* 2 beats  */
	{  4u, 1u },   /* 1 bar    */
	{  8u, 1u },   /* 2 bars   */
	{ 16u, 1u },   /* 4 bars   */
	{ 32u, 1u },   /* 8 bars   */
};

void st_loop_reset(st_loop_t *l)
{
	l->state               = ST_LOOP_OFF;
	l->start_frame         = 0u;
	l->end_frame           = 0u;
	l->length_index        = (uint8_t)ST_LOOP_LEN_DEFAULT;
	l->play_consumed       = false;
	l->play_was_down       = false;
	l->fn_was_down         = false;
	l->entered_this_press  = false;
}

uint32_t st_loop_window_frames(uint8_t index, uint32_t frames_per_beat,
			        uint32_t start, uint32_t song_frames)
{
	uint64_t frames;
	uint32_t room;

	/* No trustworthy tempo means no musical window. Fail closed rather
	 * than invent a length -- the same rule st_beat_phase.c follows. */
	if (frames_per_beat == 0u || index >= ST_LOOP_LEN_COUNT) {
		return 0u;
	}
	if (start >= song_frames) {
		return 0u;
	}

	frames = ((uint64_t)frames_per_beat * (uint64_t)st_loop_lengths[index].num) /
		 (uint64_t)st_loop_lengths[index].den;
	if (frames == 0u) {
		return 0u;   /* absurdly fast tempo: nothing to loop over */
	}

	/* NEVER READ BEYOND THE COMMITTED SONG. A window that would run past
	 * the end is clamped to the remaining room; a capture point with no
	 * room left yields 0 and the caller must not loop. */
	room = song_frames - start;
	if (frames > (uint64_t)room) {
		frames = (uint64_t)room;
	}
	return (uint32_t)frames;
}

uint32_t st_loop_next_frame(const st_loop_t *l, uint32_t frame, uint32_t song_frames)
{
	if (l->state != ST_LOOP_OFF && l->end_frame > l->start_frame) {
		/* THE WRAP, decided in exactly one place. `end_frame` is
		 * exclusive: the last frame the loop plays is end_frame - 1, and
		 * the very next frame is start_frame. No frame is skipped and
		 * none is played twice. */
		if (frame + 1u >= l->end_frame) {
			return l->start_frame;
		}
		return frame + 1u;
	}
	if (song_frames != 0u && frame + 1u >= song_frames) {
		return 0u;
	}
	return frame + 1u;
}

/* Recompute end_frame from the current index. start_frame is NEVER touched:
 * a resize moves only where the loop turns over. Returns false when no usable
 * window exists. */
static bool apply_window(st_loop_t *l, const st_loop_in_t *in)
{
	uint32_t len = st_loop_window_frames(l->length_index, in->frames_per_beat,
					      l->start_frame, in->song_frames);

	if (len == 0u) {
		return false;
	}
	l->end_frame = l->start_frame + len;
	return true;
}

st_loop_action_t st_loop_tick(st_loop_t *l, const st_loop_in_t *in)
{
	bool play_edge = in->play_down && !l->play_was_down;
	bool play_rel  = !in->play_down && l->play_was_down;
	bool fn_edge   = in->function_down && !l->fn_was_down;
	st_loop_action_t act = ST_LOOP_ACT_NONE;

	/* ---- PLAY press edge ------------------------------------------- */
	if (play_edge) {
		if (l->state == ST_LOOP_LATCHED) {
			/* EXIT ON THE PRESS EDGE, not on the release. The press
			 * is then CONSUMED: its release must do nothing at all
			 * -- no pause, no new loop, no transport command -- or
			 * one gesture would do two things. */
			l->state          = ST_LOOP_OFF;
			l->play_consumed  = true;
			l->play_was_down  = in->play_down;
			l->fn_was_down    = in->function_down;
			return ST_LOOP_ACT_EXIT;
		}
		l->entered_this_press = false;
	}

	/* ---- PLAY release ---------------------------------------------- */
	if (play_rel) {
		bool consumed = l->play_consumed;

		l->play_consumed      = false;
		l->entered_this_press = false;
		l->play_was_down      = in->play_down;
		l->fn_was_down        = in->function_down;

		if (consumed) {
			return ST_LOOP_ACT_NONE;   /* the exit press's release */
		}
		if (l->state == ST_LOOP_MOMENTARY) {
			/* Released without ever latching: leave immediately and
			 * resume at the captured frame. */
			l->state = ST_LOOP_OFF;
			return ST_LOOP_ACT_EXIT;
		}
		/* LATCHED: releasing PLAY does nothing and, crucially, does not
		 * relocate the window. OFF: not ours -- main.c's play/pause tap
		 * owns a short press. */
		return ST_LOOP_ACT_NONE;
	}

	/* ---- FUNCTION latches, and only latches ------------------------- */
	if (fn_edge && l->state == ST_LOOP_MOMENTARY) {
		l->state = ST_LOOP_LATCHED;
		act = ST_LOOP_ACT_LATCH;
	}

	/* ---- entering: PLAY held past the threshold --------------------- */
	if (l->state == ST_LOOP_OFF && in->play_down && !l->play_consumed &&
	    !l->entered_this_press && in->playing &&
	    in->play_held_ms >= ST_LOOP_HOLD_MS) {
		/* CAPTURE THE CURRENT MASTER FRAME. Not zero, not a sector
		 * boundary, not a re-derived position: the frame the audio path
		 * is playing right now. Entering never pauses and never seeks. */
		l->start_frame  = in->song_frame;
		l->length_index = (uint8_t)ST_LOOP_LEN_DEFAULT;
		if (apply_window(l, in)) {
			l->state              = ST_LOOP_MOMENTARY;
			l->entered_this_press = true;
			act = ST_LOOP_ACT_ENTER;
		} else {
			/* No tempo, or no room left in the song. Refuse rather
			 * than loop over nothing; this press simply does not
			 * start a loop. */
			l->entered_this_press = true;
		}
	}

	/* ---- length selection, only while a loop owns the transport ----- */
	if (l->state != ST_LOOP_OFF && (in->vol_minus_edge || in->vol_plus_edge)) {
		uint8_t before = l->length_index;

		if (in->vol_plus_edge && l->length_index + 1u < ST_LOOP_LEN_COUNT) {
			l->length_index++;          /* longer */
		}
		if (in->vol_minus_edge && l->length_index > 0u) {
			l->length_index--;          /* shorter */
		}
		if (l->length_index != before) {
			/* start_frame is deliberately NOT recomputed here. */
			if (apply_window(l, in)) {
				act = ST_LOOP_ACT_RESIZE;
			} else {
				l->length_index = before;
				(void)apply_window(l, in);
			}
		}
	}

	l->play_was_down = in->play_down;
	l->fn_was_down   = in->function_down;
	return act;
}
