/*
 * st_loop.c — see st_loop.h for the grammar, the exit rule, the length
 * provenance and the deliberate direction divergence from surface.ts.
 * PURE: no Zephyr, no clock, no allocation, no floating point.
 */

#include "st_loop.h"

/*
 * THE DIVISIONS, shortest to longest, as num/den BEATS -- exactly the four
 * surface.ts:939 ships (`order: [1, 2, 4, 8]`, bar fractions), with the same
 * whole-bar default at index ST_LOOP_LEN_DEFAULT.
 *
 *   0  1/2 beat == 1/8 bar        2  2 beats == 1/2 bar
 *   1  1 beat   == 1/4 bar        3  4 beats == 1 bar     <- default
 *
 * Volume - steps toward index 0, Volume + toward index 3, and both CLAMP:
 * there is no wraparound, so a player leaning on one button lands on 1/8 bar
 * or 1 bar and stays there rather than jumping across the whole range.
 */
const st_loop_len_t st_loop_lengths[ST_LOOP_LEN_COUNT] = {
	{ 1u, 2u },   /* 1/2 beat == 1/8 bar */
	{ 1u, 1u },   /* 1 beat   == 1/4 bar */
	{ 2u, 1u },   /* 2 beats  == 1/2 bar */
	{ 4u, 1u },   /* 4 beats  == 1 bar   */
};

void st_loop_reset(st_loop_t *l)
{
	l->state               = ST_LOOP_OFF;
	l->start_frame         = 0u;
	l->end_frame           = 0u;
	l->resume_frame        = 0u;
	l->cand_start          = 0u;
	l->cand_end            = 0u;
	l->cand_valid          = false;
	/* COLD BOOT = ONE BAR, always. This function is the only place the
	 * division is defaulted, and main.c calls it once at startup, so no
	 * length can survive a power cycle -- not from retained RAM, not from
	 * another song, not from the classic engine. Within a session the
	 * player's choice sticks: entry deliberately does NOT re-default it. */
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
		 * exclusive: the last frame the window contains is end_frame-1,
		 * and the frame after it is start_frame. No frame is skipped
		 * and none is played twice.
		 *
		 * A frame that has fallen outside the window (a resize just
		 * shortened it under the playhead) is pulled back to the start
		 * rather than left to run away. */
		if (frame < l->start_frame || frame >= l->end_frame) {
			return l->start_frame;
		}
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

/* THE ONE PLACE the resume position is decided: end_frame, the first frame
 * after the half-open looped section [start, end). A window clamped to the
 * song end has no frame after it, so that -- and only that -- resumes at 0,
 * which is exactly what the streamer already does when ordinary playback
 * reaches the song end. Seeking past the song is never an option. */
static void set_resume(st_loop_t *l, const st_loop_in_t *in)
{
	l->resume_frame = (l->end_frame < in->song_frames) ? l->end_frame : 0u;
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
			l->cand_valid = false;
			set_resume(l, in);
			l->state          = ST_LOOP_OFF;
			l->play_consumed  = true;
			l->play_was_down  = in->play_down;
			l->fn_was_down    = in->function_down;
			return ST_LOOP_ACT_EXIT;
		}
		l->entered_this_press = false;

		/* ---- ARM: capture the loop start HERE, on the down edge ----
		 * `in->song_frame` is the audio thread's own published playhead,
		 * so this is the frame the player is actually hearing when the
		 * finger lands -- not a control-thread estimate and not the
		 * frame the threshold will expire on. Everything downstream
		 * uses this value verbatim. */
		if (in->playing && l->state == ST_LOOP_OFF && !l->play_consumed &&
		    !in->function_down) {
			/* FUNCTION already down means this is the inherited
			 * FUNCTION+PLAY combo, not a loop gesture. Arming it would
			 * let the loop steal that combo's hold at 450 ms, before the
			 * combo's own 700 ms could fire. */
			uint32_t len = st_loop_window_frames(l->length_index,
							      in->frames_per_beat,
							      in->song_frame,
							      in->song_frames);

			if (len != 0u) {
				l->cand_start = in->song_frame;
				l->cand_end   = in->song_frame + len;
				l->cand_valid = true;
				l->play_was_down = in->play_down;
				l->fn_was_down   = in->function_down;
				return ST_LOOP_ACT_ARM;
			}
			/* No tempo, or no room left: nothing to arm. The press
			 * stays an ordinary transport tap. */
			l->cand_valid = false;
		}
	}

	/* ---- PLAY release ---------------------------------------------- */
	if (play_rel) {
		bool consumed = l->play_consumed;

		/* Released, whatever happened: the candidate is spent. A tap
		 * discards it (main.c unpins and the press is an ordinary
		 * play/pause); an ENTER already copied it into the window. */
		l->cand_valid         = false;
		l->play_consumed      = false;
		l->entered_this_press = false;
		l->play_was_down      = in->play_down;
		l->fn_was_down        = in->function_down;

		if (consumed) {
			return ST_LOOP_ACT_NONE;   /* the exit press's release */
		}
		if (l->state == ST_LOOP_MOMENTARY) {
			/* Released without ever latching: leave immediately, and
			 * rejoin the song directly after the looped section. */
			set_resume(l, in);
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
	    l->cand_valid && !l->entered_this_press && in->playing &&
	    in->play_held_ms >= ST_LOOP_HOLD_MS) {
		/* THE ARMED FRAME, not the current one. The window opens where
		 * the finger landed ST_LOOP_HOLD_MS ago; main.c seeks back to
		 * it on this action, so the first repetition is audible AT the
		 * threshold rather than a whole window later.
		 *
		 * length_index is deliberately NOT re-defaulted: a division the
		 * player selected earlier in this powered session stays
		 * selected. Only st_loop_reset() defaults it, at cold boot. */
		l->start_frame  = l->cand_start;
		if (apply_window(l, in)) {
			/* Publish the resume point NOW, at entry, not at exit:
			 * main.c pins that sector while there is time, and the
			 * earliest possible exit is the PLAY release still to
			 * come. */
			set_resume(l, in);
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

	/* ---- DIVISION: VOLUME -/+ while a loop is running AND a modifier is
	 * physically held ------------------------------------------------
	 * The modifier is whichever hand is already on the instrument:
	 *
	 *   MOMENTARY (PLAY still held)  -> PLAY itself is the modifier.
	 *   LATCHED   (PLAY released)    -> FUNCTION must be held.
	 *
	 * That is not two rules but one: a bare VOLUME press, with no button
	 * held, is master volume and the loop never takes it. Taking it would
	 * silently break the one control a player uses constantly.
	 *
	 * PLAY-held + VOLUME is CONFIRMED WORKING on hardware and is preserved
	 * verbatim. FUNCTION + VOLUME is the latched case, which could not work
	 * before: the FUNCTION branch consumed the control pass, so nothing
	 * downstream ever saw FUNCTION held. */
	if (l->state != ST_LOOP_OFF && (in->play_down || in->function_down) &&
	    (in->vol_minus_edge || in->vol_plus_edge)) {
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
			/* A resize moves the window's end, so it moves where an
			 * exit will land -- and therefore which sector main.c
			 * must have pinned. Recomputed on every resize, taken or
			 * rejected, so the two can never disagree. */
			set_resume(l, in);
		}
	}

	l->play_was_down = in->play_down;
	l->fn_was_down   = in->function_down;
	return act;
}
