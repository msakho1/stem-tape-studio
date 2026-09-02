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
	/* -1, not 0: 0 is track 1, and memset would otherwise leave a phantom
	 * first tap on it at cold boot. */
	c->rev_tap_trk = -1;
	st_ctl_scratch_end(c);
	c->seeded      = true;
}

/*
 * END THE SCRATCH GESTURE and forget every fader reference.
 *
 * The references MUST go. They are the position each fader was last seen at,
 * and movement is the difference from that -- so a reference surviving into
 * the next FUNCTION press would turn "wherever the player moved this fader
 * while not scratching" into one enormous instantaneous movement the moment
 * they grabbed it again. The first sample of a gesture establishes a fresh
 * reference and deliberately produces no drive.
 */
void st_ctl_scratch_end(st_ctl_t *c)
{
	uint32_t k;

	c->scr_target = ST_CTL_SCRATCH_NONE;
	for (k = 0; k < ST_PL_STEMS; k++) {
		c->scr_fader_prev[k] = ST_CTL_FADER_NONE;
	}
	c->scr_last_ms = 0u;
}

/*
 * THE SCRATCH GESTURE. Called once per pass while a stem song is selected.
 *
 * It answers one question -- what is the hand asking of which head right now
 * -- and it answers it the same way for both controls, because below
 * st_scratch_drive_from_*() the transport cannot tell them apart. That is the
 * "same underlying signed-head transport" the spec requires, obtained by the
 * two controls meeting HERE rather than by two engines agreeing later.
 */
static void scratch_service(st_ctl_t *c, const st_ctl_in_t *in, st_ctl_out_t *out)
{
	uint32_t dt_us;
	uint32_t k;
	int32_t  drive = 0;

	/* FUNCTION UP ENDS IT, and ends it completely. The hand comes off the
	 * record; main.c hands the signed rate to st_scrub's release ramp and
	 * the head stays exactly where the gesture left it. */
	if (!in->function_down) {
		if (c->scr_target != ST_CTL_SCRATCH_NONE) {
			st_ctl_scratch_end(c);
		}
		return;
	}

	/* The first pass of a press has no elapsed time to divide by, and no
	 * fader reference to subtract. Establish both and ask for nothing. */
	if (c->scr_last_ms == 0u) {
		c->scr_last_ms = in->now_ms ? in->now_ms : 1u;
		for (k = 0; k < ST_PL_STEMS; k++) {
			/*
			 * ONLY FADERS THAT ACTUALLY REPORTED. Copying the raw
			 * value unconditionally records -1 -- "not sampled" --
			 * as though it were a position, and the next real
			 * reading then differs from it by the whole ADC range.
			 * A fader nobody touched would lurch the head at full
			 * drive the moment main.c got round to polling it.
			 *
			 * An unsampled channel keeps ST_CTL_FADER_NONE and is
			 * skipped until it reports, which is what that value is
			 * for. Only the round-robin makes this reachable: during
			 * a gesture main.c reads one fader per pass, so three of
			 * the four are -1 on any given pass INCLUDING the first.
			 */
			if (in->fader_raw[k] >= 0) {
				c->scr_fader_prev[k] = in->fader_raw[k];
			}
		}
		return;
	}

	dt_us = (in->now_ms - c->scr_last_ms) * 1000u;
	c->scr_last_ms = in->now_ms ? in->now_ms : 1u;
	if (dt_us == 0u) {
		return;    /* two passes inside one millisecond: no news */
	}

	/*
	 * DELTAS FIRST, REFERENCES AFTER. Movement is the difference from the
	 * last reading, so the reference must not be advanced until every
	 * consumer of the difference has had it -- updating first makes every
	 * delta identically zero, which is a bug that looks exactly like a dead
	 * control.
	 */
	int32_t fdrive[ST_PL_STEMS];

	for (k = 0; k < ST_PL_STEMS; k++) {
		const int32_t prev = c->scr_fader_prev[k];

		fdrive[k] = 0;
		if (in->fader_raw[k] >= 0 && prev != ST_CTL_FADER_NONE) {
			fdrive[k] = st_scratch_drive_from_fader(in->fader_raw[k] - prev,
								 dt_us);
		}
	}

	/* Every fader that reported a value updates its reference, owner or
	 * not: a fader moved during someone else's gesture must not bank that
	 * movement and deliver it later. */
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (in->fader_raw[k] >= 0) {
			c->scr_fader_prev[k] = in->fader_raw[k];
		}
	}

	/*
	 * WHO OWNS THE GESTURE. First mover wins, and keeps it until FUNCTION
	 * is released -- see st_ctl_t's own note on why a brushed fader must
	 * not steal a master shuttle mid-movement.
	 */
	if (c->scr_target == ST_CTL_SCRATCH_NONE) {
		if (in->rocker_dir != 0) {
			c->scr_target = ST_CTL_SCRATCH_MASTER;
		} else {
			for (k = 0; k < ST_PL_STEMS; k++) {
				if (fdrive[k] != 0) {
					c->scr_target = (uint8_t)k;
					break;
				}
			}
		}
	}

	if (c->scr_target == ST_CTL_SCRATCH_NONE) {
		/*
		 * FUNCTION IS HELD BUT NOTHING HAS MOVED YET. Not a gesture, so
		 * nothing is published and nothing is consumed -- FUNCTION is
		 * still free to mean whatever else it means, which is what keeps
		 * merely holding it from suppressing the other FUNCTION chords.
		 */
		return;
	}

	if (c->scr_target == ST_CTL_SCRATCH_MASTER) {
		drive = st_scratch_drive_from_rocker(in->rocker_dir);
		out->rocker_consumed = true;
	} else {
		drive = fdrive[c->scr_target];
		out->fader_consumed_mask = (uint8_t)(1u << c->scr_target);
	}

	out->scratch_active     = true;
	out->scratch_target     = c->scr_target;
	out->scratch_drive_q16  = drive;

	/*
	 * THE FUNCTION PRESS IS SPENT. Nothing else may read it as its own
	 * modifier for the rest of this press -- the reverse double-tap least
	 * of all, since a hand shuttling the song should not also be able to
	 * flip a track into reverse by brushing it.
	 */
	out->function_consumed = true;
	c->fn_consumed         = true;
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
	 * The exit end is end_frame itself -- the first frame of ordinary
	 * playback after the loop, and exactly where the exit seek lands. An
	 * exit can happen on the very next pass after entry, so neither region
	 * can be left until the release. */
	if (st_loop_active(&c->loop)) {
		out->pin_valid       = true;
		out->pin_entry_frame = c->loop.start_frame;
		out->pin_exit_frame  = c->loop.end_frame;
	} else if (st_loop_armed(&c->loop)) {
		out->pin_valid       = true;
		out->pin_entry_frame = st_loop_cand_start(&c->loop);
		out->pin_exit_frame  = st_loop_cand_end(&c->loop);
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
		c->play_fn_chord  = false;
		c->fn_consumed    = false;
		c->trk_prev       = 0u;
		c->rev_tap_trk    = -1;
		c->rev_fn_held    = false;
		/* A song going away mid-gesture must not leave a fader reference
		 * behind for the next song to scratch by. */
		st_ctl_scratch_end(c);
		st_loop_reset_gesture_edges(&c->loop);
		publish_levels(c, out);
		out->track_mask = 0u;
		return;
	}

	/*
	 * THE SCRATCH GESTURE RUNS FIRST, and that ordering is the arbitration.
	 *
	 * It is the only handler that can claim the rocker or a fader, and it
	 * marks the FUNCTION press spent when it does. Everything below then
	 * sees a consumed press and declines to reinterpret it -- so one
	 * physical movement does exactly one thing, which is the rule the spec
	 * states and the one this file was written to enforce for PLAY.
	 */
	scratch_service(c, in, out);

	/* ---- ONE classification, TWO consumers, no second interpretation --
	 * The mask published here is the same object the mixer solos with and
	 * the same object the LED path lights. There is no second decode of
	 * this rail while a stem song is selected. */
	out->track_mask = st_ladder_mask(&c->ladder);
	/* Bits another owner has claimed -- the FX overlay, whose Track buttons
	 * are effects -- are dropped HERE, after the decode, so they cannot
	 * solo. PLAY is deliberately NOT affected: it shares this rail, and
	 * suppressing it along with the Track bits is what made looping
	 * impossible while an effect was held. */
	out->track_mask &= (uint8_t)~in->track_consumed_mask;

	/* ---- FUNCTION + DOUBLE-TAP TRACK: per-track reverse ---------------
	 * The SAME published mask, one pass later, is what makes a press an
	 * edge -- so this extends the arbitration that already exists rather
	 * than decoding the rail a second time. Bits the FX overlay has
	 * claimed are already gone from it, which is why an effect held on a
	 * Track button cannot also toggle reverse.
	 *
	 * ONE TRACK AT A TIME, physically: the ladder classifies a chord as a
	 * chord, so two Track bits rising together is not two taps -- it is a
	 * mask this gesture ignores, because a reverse toggle names exactly
	 * one track.
	 */
	{
		const uint8_t trk = out->track_mask;
		const uint8_t down_edges = (uint8_t)(trk & ~c->trk_prev);
		const uint8_t up_edges   = (uint8_t)(c->trk_prev & ~trk);

		/* FUNCTION must be held for the WHOLE press. Set on the down
		 * edge, cleared the moment FUNCTION is seen up while the button
		 * is still down -- see st_ctl_t's own note on why neither edge
		 * alone is enough. */
		if (down_edges != 0u) {
			/* NOT DURING A SCRATCH. A hand shuttling the whole song
			 * with FUNCTION held will brush Track buttons; without
			 * this, the brush arms a reverse double-tap and the
			 * player finds a stem playing backwards afterwards with
			 * no idea why. The scratch owns this FUNCTION press. */
			c->rev_fn_held = in->function_down && !out->scratch_active;
		} else if (trk != 0u && (!in->function_down || out->scratch_active)) {
			c->rev_fn_held = false;
		}

		if (up_edges != 0u && c->rev_fn_held) {
			/* Exactly one bit, or it is a chord and not a tap. */
			const bool single = (up_edges & (uint8_t)(up_edges - 1u)) == 0u;

			if (single) {
				uint8_t k = 0u;

				while (((up_edges >> k) & 1u) == 0u) {
					k++;
				}
				if (c->rev_tap_trk == (int8_t)k &&
				    (in->now_ms - c->rev_tap_ms) <= ST_CTL_REVERSE_DBLTAP_MS) {
					/* THE SECOND TAP. Toggle, and clear the
					 * pending tap so a THIRD tap starts a
					 * fresh pair rather than immediately
					 * toggling back -- a triple-tap is one
					 * toggle and a new first tap, which is
					 * what a player rolling three fingers
					 * across the button actually means. */
					out->reverse_toggle = true;
					out->reverse_track  = k;
					c->rev_tap_trk      = -1;
					/* The FUNCTION press is spent: it named
					 * a reverse toggle and must not also
					 * reach the loop's own FN gestures. */
					c->fn_consumed = true;
				} else {
					c->rev_tap_trk = (int8_t)k;
					c->rev_tap_ms  = in->now_ms;
				}
			} else {
				c->rev_tap_trk = -1;
			}
			c->rev_fn_held = false;
		} else if (up_edges != 0u) {
			/* An UNMODIFIED tap. It is not part of this gesture, and
			 * it also invalidates a pending first tap: FUNCTION +
			 * tap, then a bare tap, is not a double-tap. */
			c->rev_tap_trk = -1;
			c->rev_fn_held = false;
		}

		/* The window is checked at the second tap rather than expired
		 * here on a timer, so this needs no per-pass work at all -- and
		 * a first tap left pending forever is harmless, because the
		 * elapsed test is what rejects it. */
		c->trk_prev = trk;
	}

	/* ---- PLAY edges, from that same classification ------------------- */
	play_now       = st_ladder_play(&c->ladder);
	play_edge_down = play_now && !c->play_prev;
	play_edge_up   = !play_now && c->play_prev;

	if (play_edge_down) {
		c->play_down_ms    = in->now_ms;
		c->play_hold_spent = false;
		c->play_fn_chord   = false;
	}

	/*
	 * THE CHORD LATCH. Set the moment FUNCTION and PLAY are down together,
	 * and held until PLAY is released.
	 *
	 * Tested on EVERY pass the button is down, not only at the press edge,
	 * so the gesture works in both finger orders: FUNCTION-then-PLAY and
	 * PLAY-then-FUNCTION. Only checking the press edge would leave the
	 * second order still toggling the transport, which is the same class
	 * of half-fix as testing at the release edge.
	 */
	if (play_now && in->function_down) {
		c->play_fn_chord = true;
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
		/* ...and not one that FUNCTION qualified. A chorded press has
		 * already meant something else; letting it ALSO toggle the
		 * transport is the "one press, two actions" bug this whole
		 * block exists to prevent. */
		if (!c->play_hold_spent && !c->play_fn_chord) {
			out->play_tap = true;
		}
		c->play_hold_spent = false;
		/* play_fn_chord is deliberately NOT cleared here. It is read
		 * only on this edge, and the next press edge clears it before
		 * anything can read it again -- so a second clear would be a
		 * line no behaviour depends on. Mutation testing found exactly
		 * that: deleting it changed no result. The press-edge clear is
		 * the one that matters, and "a chord followed by a bare tap"
		 * in tests/test_ctl.c is what holds it. */
	}

	if (!in->function_down) {
		c->fn_consumed = false;
	}
	out->function_consumed = c->fn_consumed;

	c->play_prev = play_now;
	publish_levels(c, out);
}
