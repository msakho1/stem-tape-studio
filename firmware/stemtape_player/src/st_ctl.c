/*
 * st_ctl.c — see st_ctl.h for the arbitration order, the ownership rule and
 * the timing budgets. PURE: no Zephyr, no ADC, no clock, no allocation.
 */

#include <string.h>

#include "st_ctl.h"

void st_ctl_reset(st_ctl_t *c)
{
	memset(c, 0, sizeof(*c));
	st_ladder_reset(&c->ladder);
	st_loop_reset(&c->loop);      /* THE one-bar cold-boot default lives here */
	c->vol_cand    = 0;
	c->vol_settled = 0;
	c->seeded      = true;
}

/* VOLUME debounce -> a single press EDGE. main.c's rocker is on its own
 * ladder and is read once per pass; three agreeing reads commit it, and an
 * edge is reported only on the transition away from "none", so a held button
 * steps the division exactly once. */
static int8_t vol_edge(st_ctl_t *c, int8_t dir)
{
	int8_t before = c->vol_settled;

	if (dir == c->vol_cand) {
		if (c->vol_cand_n < ST_CTL_VOL_SETTLE) {
			c->vol_cand_n++;
		}
	} else {
		c->vol_cand   = dir;
		c->vol_cand_n = 1u;
	}
	if (c->vol_cand_n >= ST_CTL_VOL_SETTLE) {
		c->vol_settled = c->vol_cand;
	}
	if (c->vol_settled != before && c->vol_settled != 0) {
		return c->vol_settled;
	}
	return 0;
}

static void publish_levels(const st_ctl_t *c, st_ctl_out_t *out)
{
	out->loop_active  = st_loop_active(&c->loop);
	out->loop_latched = (c->loop.state == ST_LOOP_LATCHED);
	out->loop_reverse = st_loop_reverse(&c->loop);
	out->loop_start   = c->loop.start_frame;
	out->loop_end     = c->loop.end_frame;
	out->loop_resume  = st_loop_resume_frame(&c->loop);

	/* WHAT MUST BE RESIDENT, and from when. Between the ARM and the ENTER
	 * it is the CANDIDATE's two ends; once the loop is running it is the
	 * live window's. Both are published as frames, not sectors: main.c owns
	 * the sector geometry and this module owns none of it.
	 *
	 * The entry end is the seek-back target -- up to a whole hold's worth
	 * of song behind the playhead, so it is certainly not resident and
	 * certainly must be fetched during the hold. It is also every forward
	 * wrap's destination.
	 *
	 * The exit end is published as end_frame - 1, the LAST frame the window
	 * contains, rather than end_frame itself. That one frame of slack makes
	 * a single pinned region serve both jobs: it is the reverse wrap's
	 * destination outright, and it is in the same sector as end_frame (or
	 * the one before it), so the region's depth still covers the forward
	 * exit with runway to spare. An exit can happen on the very next pass
	 * after entry, so neither can be left until the release. */
	if (st_loop_active(&c->loop)) {
		out->pin_valid       = true;
		out->pin_entry_frame = c->loop.start_frame;
		out->pin_exit_frame  = (c->loop.end_frame > 0u)
					? c->loop.end_frame - 1u : 0u;
	} else if (st_loop_armed(&c->loop)) {
		out->pin_valid       = true;
		out->pin_entry_frame = st_loop_cand_start(&c->loop);
		out->pin_exit_frame  = (st_loop_cand_end(&c->loop) > 0u)
					? st_loop_cand_end(&c->loop) - 1u : 0u;
	} else {
		out->pin_valid = false;
	}
}

void st_ctl_service(st_ctl_t *c, const st_ctl_in_t *in, st_ctl_out_t *out)
{
	st_loop_in_t li;
	st_loop_action_t la;
	int8_t ve;
	bool play_now, play_edge_down, play_edge_up;

	memset(out, 0, sizeof(*out));

	/* NOT OURS. With no stem song selected the inherited Tape Looper owns
	 * the whole surface, exactly as it always has. Nothing is published and
	 * nothing is consumed -- but the ladder is still fed, so that selecting
	 * a song mid-hold cannot inherit a stale settled state. */
	st_ladder_update(&c->ladder, in->ladder_raw);
	if (!in->stem_song) {
		c->play_prev      = false;
		c->play_hold_spent = false;
		c->fn_consumed    = false;
		st_loop_reset_gesture_edges(&c->loop);
		publish_levels(c, out);
		out->track_mask = 0u;
		return;
	}

	/* ---- ONE classification, TWO consumers, no second interpretation --
	 * The mask published here is the same object the mixer solos with and
	 * the same object the LED path lights. There is no second decode of
	 * this rail while a stem song is selected. */
	out->track_mask = st_ladder_mask(&c->ladder);

	/* ---- PLAY edges, from that same classification ------------------- */
	play_now       = st_ladder_play(&c->ladder);
	play_edge_down = play_now && !c->play_prev;
	play_edge_up   = !play_now && c->play_prev;

	if (play_edge_down) {
		c->play_down_ms    = in->now_ms;
		c->play_hold_spent = false;
	}

	/* ---- VOLUME, debounced to one edge per press --------------------- */
	ve = vol_edge(c, in->vol_dir);

	/* ---- the loop engine, with FUNCTION ACTUALLY VISIBLE --------------
	 * This is the whole point of calling st_ctl_service() above main.c's
	 * FUNCTION branch. In st15 the loop was ticked below it, and every
	 * path out of that branch is a `continue`, so function_down could
	 * never be anything but false and the latch was unreachable. */
	memset(&li, 0, sizeof(li));
	li.play_down       = play_now;
	li.play_held_ms    = play_now ? (uint32_t)(in->now_ms - c->play_down_ms) : 0u;
	li.function_down   = in->function_down;
	li.playing         = in->playing;
	li.song_frame      = in->song_frame;
	li.song_frames     = in->song_frames;
	li.frames_per_beat = in->frames_per_beat;
	li.vol_minus_edge  = (ve < 0);
	li.vol_plus_edge   = (ve > 0);

	la = st_loop_tick(&c->loop, &li);

	switch (la) {
	case ST_LOOP_ACT_ARM:
		out->loop_arm = true;
		break;
	case ST_LOOP_ACT_ENTER:
		out->loop_enter   = true;
		c->play_hold_spent = true;
		break;
	case ST_LOOP_ACT_RESIZE:
		out->loop_resize  = true;
		out->vol_consumed = true;
		break;
	case ST_LOOP_ACT_LATCH:
		out->loop_latch = true;
		/* CONSUME THE FUNCTION PRESS for the whole of its hold, so the
		 * inherited branch cannot also read it as a song change, a
		 * power-off countdown, a brightness step or a bank move. It is
		 * released when FUNCTION is. */
		c->fn_consumed = true;
		break;
	case ST_LOOP_ACT_DIRECTION:
		out->loop_direction = true;
		c->play_hold_spent  = true;   /* never also a tap */
		c->fn_consumed      = true;
		break;
	case ST_LOOP_ACT_EXIT:
		out->loop_exit     = true;
		c->play_hold_spent = true;    /* the exit press is not a tap */
		break;
	case ST_LOOP_ACT_NONE:
	default:
		break;
	}

	/* A resize is reported once, on the edge; but the VOLUME press that
	 * caused it must stay consumed for as long as it is physically down,
	 * or main.c's master-volume handler would take the same press on the
	 * following pass. */
	if (st_loop_active(&c->loop) && c->vol_settled != 0 &&
	    (play_now || in->function_down)) {
		out->vol_consumed = true;
	}

	/* ---- the hold that produced nothing, said out loud ---------------
	 * A PLAY hold that crosses the threshold with no usable window is not
	 * a tap and must not toggle the transport -- but it must not be
	 * silent either. Reported once, on the pass the threshold expires. */
	if (play_now && !c->play_hold_spent && !st_loop_active(&c->loop) &&
	    li.play_held_ms >= ST_LOOP_HOLD_MS) {
		c->play_hold_spent = true;
		out->refused = (in->frames_per_beat == 0u) ? ST_CTL_REFUSE_NO_TEMPO
							   : ST_CTL_REFUSE_NO_ROOM;
	}

	/* ---- THE PLAY TAP, and the only place the transport toggles -------
	 * A press that never crossed the threshold and was never consumed by
	 * the loop is an ordinary play/stop. There is no second PLAY owner:
	 * the inherited 400 ms hold-to-restart is gated off entirely while a
	 * stem song is selected, so nothing competes for this press and no
	 * restart can fire underneath it. */
	if (play_edge_up) {
		if (!c->play_hold_spent) {
			out->play_tap = true;
		}
		c->play_hold_spent = false;
	}

	if (!in->function_down) {
		c->fn_consumed = false;
	}
	out->function_consumed = c->fn_consumed;

	c->play_prev = play_now;
	publish_levels(c, out);
}
