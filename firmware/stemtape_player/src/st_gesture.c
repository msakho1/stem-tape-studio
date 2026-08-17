/*
 * st_gesture.c — see st_gesture.h. PURE.
 *
 * Design note on "tap" timing: every COMPOUND action (loop latch, FX track
 * latch/unlatch, scrub latch-arm, the two-volume FX-scope chord) fires the
 * instant the SECOND/triggering control's edge makes the chord unambiguous
 * (a press, not a release) rather than waiting for a release to confirm a
 * "tap" — this is the most responsive reading consistent with the task's
 * grammar and keeps the state machine's firing rule uniform: compound
 * actions fire on the triggering press; single-control actions (play/pause,
 * mute) fire on release once a hold-vs-tap decision is resolved. Each
 * control's `joined` flag marks "this press's release must NOT also fire
 * a bare/single interpretation" — set at the exact moment a compound
 * action consumes it.
 */

#include "st_gesture.h"

#include <string.h>

#include "st_fx_catalog.h"

static void emit(st_cmd_batch_t *out, st_cmd_id_t id, uint8_t stem, int8_t dir, uint8_t val)
{
	if (out->count >= ST_GESTURE_MAX_CMDS_PER_EVENT) {
		return; /* defensive; no single real gesture emits this many */
	}
	out->cmds[out->count].id = id;
	out->cmds[out->count].stem = stem;
	out->cmds[out->count].direction = dir;
	out->cmds[out->count].value_q8 = val;
	out->count++;
}

void st_gesture_reset(st_gesture_state_t *s, uint32_t now_ms)
{
	memset(s, 0, sizeof(*s));
	s->boot_ms = now_ms;
	s->settled = false;
	s->fx_track_holding = 0xFFu;
	s->scrub_speed_index = 1u; /* DEFAULT_SCRUB_SPEED_INDEX (st_scrub.h) */
	s->active_stem = 0u;
	for (uint8_t i = 0; i < 4u; i++) {
		s->fader_raw_last[i] = 0xFFFFu; /* pickup pending */
	}
}

bool st_gesture_is_settled(const st_gesture_state_t *s, uint32_t now_ms)
{
	if (s->settled) {
		return true;
	}
	return (uint32_t)(now_ms - s->boot_ms) >= ST_GESTURE_STARTUP_SETTLE_MS;
}

static bool control_is_track(st_control_id_t c, uint8_t *bank_out)
{
	uint8_t button;

	switch (c) {
	case ST_CTRL_TRACK1: button = 0; break;
	case ST_CTRL_TRACK2: button = 1; break;
	case ST_CTRL_TRACK3: button = 2; break;
	case ST_CTRL_TRACK4: button = 3; break;
	default: return false;
	}
	*bank_out = st_fx_bank_of_button(button);
	return true;
}

/* ------------------------------------------------------------ FUNCTION --- */
static void handle_function_press(st_gesture_state_t *s, uint32_t now_ms, st_cmd_batch_t *out)
{
	s->ctrl[ST_CTRL_FUNCTION].down = true;
	s->ctrl[ST_CTRL_FUNCTION].down_since_ms = now_ms;
	s->ctrl[ST_CTRL_FUNCTION].joined = false;
	s->function_down_since_ms = now_ms;

	/* Priority 1: PLAY held with an active (unlatched) momentary loop -> latch it. */
	if (s->ctrl[ST_CTRL_PLAY].down && s->loop_momentary_active && !s->loop_latched) {
		emit(out, ST_CMD_LOOP_LATCH, 0, 0, 0);
		s->loop_latched = true;
		s->ctrl[ST_CTRL_FUNCTION].joined = true;
		return;
	}

	/* Priority 2: a Track is currently held -> FX momentary/latch toggle.
	 * Explicitly still applies during a latched scrub (task item 8). */
	if (s->fx_scope != ST_FX_SCOPE_NONE && s->fx_track_holding != 0xFFu) {
		uint8_t bank = s->fx_track_holding;

		if (s->fx_track_latched[bank]) {
			s->fx_track_latched[bank] = false;
			emit(out, ST_CMD_FX_TRACK_UNLATCH, bank, 0, 0);
		} else {
			s->fx_track_latched[bank] = true;
			emit(out, ST_CMD_FX_TRACK_LATCH, bank, 0, 0);
		}
		s->ctrl[ST_CTRL_FUNCTION].joined = true;
		/* Mark the physical track control joined too so its own
		 * release does not also fire a MOMENTARY_END. */
		for (uint8_t i = 0; i < 4u; i++) {
			uint8_t b;

			if (control_is_track((st_control_id_t)(ST_CTRL_TRACK1 + i), &b) && b == bank) {
				s->ctrl[ST_CTRL_TRACK1 + i].joined = true;
			}
		}
		return;
	}

	/* Priority 3: rocker held and scrub already momentary-active -> arm latch. */
	if (s->scrub_active && !s->scrub_latched &&
	    (s->ctrl[ST_CTRL_ROCKER_FWD].down || s->ctrl[ST_CTRL_ROCKER_RWD].down)) {
		emit(out, ST_CMD_SCRUB_LATCH_ARM, 0, s->scrub_direction, 0);
		s->scrub_latch_armed_this_hold = true;
		s->ctrl[ST_CTRL_FUNCTION].joined = true;
		return;
	}

	/* Otherwise: a plain FUNCTION press. Long-hold power-off is handled
	 * in st_gesture_process_tick(); a short release below may still
	 * resolve to a "bare tap" (scrub unlatch) or nothing. */
}

static void handle_function_release(st_gesture_state_t *s, uint32_t now_ms, st_cmd_batch_t *out)
{
	uint32_t held_ms = (uint32_t)(now_ms - s->ctrl[ST_CTRL_FUNCTION].down_since_ms);

	s->ctrl[ST_CTRL_FUNCTION].down = false;

	if (s->ctrl[ST_CTRL_FUNCTION].joined) {
		s->ctrl[ST_CTRL_FUNCTION].joined = false;
		return; /* already handled at press time */
	}

	/* Bare tap: nothing else was joined to this press. */
	if (held_ms <= ST_GESTURE_BARE_TAP_MAX_MS && s->scrub_latched) {
		emit(out, ST_CMD_SCRUB_UNLATCH, 0, s->scrub_direction, 0);
		s->scrub_latched = false;
		s->scrub_active = false;
		s->scrub_direction = 0;
	}
}

/* ---------------------------------------------------------------- PLAY --- */
static void handle_play_press(st_gesture_state_t *s, uint32_t now_ms, st_cmd_batch_t *out)
{
	s->ctrl[ST_CTRL_PLAY].down = true;
	s->ctrl[ST_CTRL_PLAY].down_since_ms = now_ms;
	s->ctrl[ST_CTRL_PLAY].joined = false;

	if (s->loop_latched) {
		emit(out, ST_CMD_LOOP_EXIT, 0, 0, 0);
		s->loop_latched = false;
		s->loop_momentary_active = false;
		s->ctrl[ST_CTRL_PLAY].joined = true;
	}
}

static void handle_play_release(st_gesture_state_t *s, uint32_t now_ms, st_cmd_batch_t *out)
{
	(void)now_ms;
	s->ctrl[ST_CTRL_PLAY].down = false;

	if (s->ctrl[ST_CTRL_PLAY].joined) {
		s->ctrl[ST_CTRL_PLAY].joined = false;
		if (s->loop_momentary_active && !s->loop_latched) {
			emit(out, ST_CMD_LOOP_MOMENTARY_END, 0, 0, 0);
			s->loop_momentary_active = false;
		}
		return; /* the exit-on-press case already fired; nothing else to do */
	}

	/* Never crossed the hold threshold: a genuine tap. */
	emit(out, ST_CMD_PLAY_PAUSE_TOGGLE, 0, 0, 0);
	s->playing = !s->playing;
}

/* -------------------------------------------------------------- ROCKER --- */
static void handle_rocker_press(st_gesture_state_t *s, st_control_id_t which, uint32_t now_ms,
				 st_cmd_batch_t *out)
{
	int8_t dir = (which == ST_CTRL_ROCKER_FWD) ? 1 : -1;

	s->ctrl[which].down = true;
	s->ctrl[which].down_since_ms = now_ms;

	if (s->ctrl[ST_CTRL_FUNCTION].down && !s->scrub_active) {
		emit(out, ST_CMD_SCRUB_MOMENTARY_START, 0, dir, 0);
		s->scrub_active = true;
		s->scrub_latched = false;
		s->scrub_direction = dir;
		s->scrub_latch_armed_this_hold = false;
		s->ctrl[which].joined = true;
	}
	/* Else: "touching the rocker alone must not unlatch it" -- inert,
	 * whether scrub is inactive (FUNCTION not held: no gesture defined)
	 * or already active/latched (rocker-alone touch is a no-op). */
}

static void handle_rocker_release(st_gesture_state_t *s, st_control_id_t which, uint32_t now_ms,
				   st_cmd_batch_t *out)
{
	(void)now_ms;
	s->ctrl[which].down = false;

	if (!s->ctrl[which].joined) {
		return; /* this touch never started/owned the scrub gesture */
	}
	s->ctrl[which].joined = false;

	if (s->scrub_latch_armed_this_hold) {
		s->scrub_latched = true;
		s->scrub_latch_armed_this_hold = false;
		/* scrub_active stays true; no additional command -- the arm
		 * itself (ST_CMD_SCRUB_LATCH_ARM) already told the audio
		 * engine to keep the rate held rather than ramp back. */
	} else {
		emit(out, ST_CMD_SCRUB_RELEASE, 0, s->scrub_direction, 0);
		s->scrub_active = false;
		s->scrub_direction = 0;
	}
}

/* -------------------------------------------------------------- VOLUME --- */
static void fire_single_volume_action(st_gesture_state_t *s, int8_t dir, st_cmd_batch_t *out)
{
	if (s->scrub_active) {
		int new_index = (int)s->scrub_speed_index + dir;

		if (new_index < 0) {
			new_index = 0;
		}
		if (new_index > 3) {
			new_index = 3;
		}
		s->scrub_speed_index = (uint8_t)new_index;
		emit(out, ST_CMD_SCRUB_SPEED_SELECT, s->scrub_speed_index, dir, 0);
	} else if (s->fx_scope == ST_FX_SCOPE_STEM) {
		emit(out, dir > 0 ? ST_CMD_FX_STEM_CYCLE_NEXT : ST_CMD_FX_STEM_CYCLE_PREV, 0, dir, 0);
	} else if (s->loop_momentary_active || s->loop_latched) {
		emit(out, dir > 0 ? ST_CMD_LOOP_DIVISION_NEXT : ST_CMD_LOOP_DIVISION_PREV, 0, dir, 0);
	} else if (s->ctrl[ST_CTRL_FUNCTION].down) {
		emit(out, dir > 0 ? ST_CMD_STEM_SELECT_NEXT : ST_CMD_STEM_SELECT_PREV, 0, dir, 0);
	} else {
		emit(out, ST_CMD_MASTER_VOLUME_STEP, 0, dir, 0);
	}
}

static void handle_volume_press(st_gesture_state_t *s, st_control_id_t which, uint32_t now_ms,
				 st_cmd_batch_t *out)
{
	bool is_minus = (which == ST_CTRL_VOL_MINUS);
	st_control_id_t other = is_minus ? ST_CTRL_VOL_PLUS : ST_CTRL_VOL_MINUS;

	s->ctrl[which].down = true;
	s->ctrl[which].down_since_ms = now_ms;
	s->ctrl[which].joined = false;

	if (s->ctrl[other].down &&
	    (uint32_t)(now_ms - s->ctrl[other].down_since_ms) <= ST_GESTURE_VOLUME_CHORD_WINDOW_MS) {
		/* Chord completes now. */
		s->ctrl[which].joined = true;
		s->ctrl[other].joined = true;
		if (s->ctrl[ST_CTRL_FUNCTION].down) {
			if (s->fx_scope == ST_FX_SCOPE_NONE) {
				s->fx_scope = ST_FX_SCOPE_GLOBAL;
				emit(out, ST_CMD_FX_SCOPE_OPEN_GLOBAL, 0, 0, 0);
			}
			/* already open: no-op, per header design note */
		} else {
			if (s->fx_scope == ST_FX_SCOPE_NONE) {
				s->fx_scope = ST_FX_SCOPE_STEM;
				emit(out, ST_CMD_FX_SCOPE_OPEN_STEM, 0, 0, 0);
			} else {
				s->fx_scope = ST_FX_SCOPE_NONE;
				emit(out, ST_CMD_FX_SCOPE_CLOSE, 0, 0, 0);
			}
		}
	}
	/* Else: pending -- resolved on release or by the chord window
	 * elapsing in st_gesture_process_tick(). */
}

static void handle_volume_release(st_gesture_state_t *s, st_control_id_t which, uint32_t now_ms,
				   st_cmd_batch_t *out)
{
	(void)now_ms;
	s->ctrl[which].down = false;

	if (s->ctrl[which].joined) {
		s->ctrl[which].joined = false;
		return; /* consumed by the chord */
	}
	fire_single_volume_action(s, (which == ST_CTRL_VOL_MINUS) ? -1 : 1, out);
}

/* --------------------------------------------------------------- TRACK --- */
static void handle_track_press(st_gesture_state_t *s, st_control_id_t which, uint32_t now_ms,
				st_cmd_batch_t *out)
{
	uint8_t bank;

	(void)control_is_track(which, &bank);
	s->ctrl[which].down = true;
	s->ctrl[which].down_since_ms = now_ms;
	s->ctrl[which].joined = false;

	if (s->fx_scope != ST_FX_SCOPE_NONE) {
		s->fx_track_holding = bank;
		emit(out, ST_CMD_FX_TRACK_MOMENTARY_START, bank, 0, 0);
	}
	/* Non-FX mute/solo/link: resolved on release (tap vs. hold), see below. */
}

static void handle_track_release(st_gesture_state_t *s, st_control_id_t which, uint32_t now_ms,
				  st_cmd_batch_t *out)
{
	uint8_t bank;
	uint32_t held_ms = (uint32_t)(now_ms - s->ctrl[which].down_since_ms);

	(void)control_is_track(which, &bank);
	s->ctrl[which].down = false;

	if (s->fx_scope != ST_FX_SCOPE_NONE) {
		if (s->fx_track_holding == bank) {
			s->fx_track_holding = 0xFFu;
		}
		if (s->ctrl[which].joined) {
			s->ctrl[which].joined = false;
			return; /* upgraded to latch/unlatch already, at FUNCTION-press time */
		}
		emit(out, ST_CMD_FX_TRACK_MOMENTARY_END, bank, 0, 0);
		return;
	}

	/* SCOPED, NOT VERBATIM (see st_gesture.h): tap = mute, hold past the
	 * contract's solo/link threshold = solo. A verbatim port of the full
	 * mute/solo/link/song-select contract is deferred (see the firmware
	 * README). */
	if (held_ms < ST_GESTURE_SOLO_LINK_THRESHOLD_MS) {
		emit(out, ST_CMD_TRACK_MUTE_TOGGLE, bank, 0, 0);
	} else {
		emit(out, ST_CMD_TRACK_SOLO_TOGGLE, bank, 0, 0);
	}
}

/* ----------------------------------------------------------- dispatch --- */
void st_gesture_process_edge(st_gesture_state_t *s, st_control_id_t ctrl, bool pressed,
			      uint32_t now_ms, st_cmd_batch_t *out)
{
	out->count = 0;

	if (!st_gesture_is_settled(s, now_ms)) {
		/* Absorb silently, but still track physical state so a
		 * control already held across the settle boundary is not
		 * misread as a fresh press once gestures are accepted. */
		s->ctrl[ctrl].down = pressed;
		s->ctrl[ctrl].down_since_ms = now_ms;
		return;
	}
	s->settled = true;

	switch (ctrl) {
	case ST_CTRL_FUNCTION:
		pressed ? handle_function_press(s, now_ms, out) : handle_function_release(s, now_ms, out);
		break;
	case ST_CTRL_PLAY:
		pressed ? handle_play_press(s, now_ms, out) : handle_play_release(s, now_ms, out);
		break;
	case ST_CTRL_ROCKER_FWD:
	case ST_CTRL_ROCKER_RWD:
		pressed ? handle_rocker_press(s, ctrl, now_ms, out)
			: handle_rocker_release(s, ctrl, now_ms, out);
		break;
	case ST_CTRL_VOL_MINUS:
	case ST_CTRL_VOL_PLUS:
		pressed ? handle_volume_press(s, ctrl, now_ms, out)
			: handle_volume_release(s, ctrl, now_ms, out);
		break;
	case ST_CTRL_TRACK1: case ST_CTRL_TRACK2: case ST_CTRL_TRACK3: case ST_CTRL_TRACK4:
		pressed ? handle_track_press(s, ctrl, now_ms, out)
			: handle_track_release(s, ctrl, now_ms, out);
		break;
	default:
		break;
	}
}

void st_gesture_process_tick(st_gesture_state_t *s, uint32_t now_ms, st_cmd_batch_t *out)
{
	out->count = 0;

	if (!st_gesture_is_settled(s, now_ms)) {
		return;
	}

	/* PLAY hold-threshold crossing -> momentary loop start. */
	if (s->ctrl[ST_CTRL_PLAY].down && !s->ctrl[ST_CTRL_PLAY].joined &&
	    !s->loop_momentary_active && s->playing &&
	    (uint32_t)(now_ms - s->ctrl[ST_CTRL_PLAY].down_since_ms) >= ST_GESTURE_PLAY_TAP_HOLD_MS) {
		emit(out, ST_CMD_LOOP_MOMENTARY_START, 0, 0, 0);
		s->loop_momentary_active = true;
		s->ctrl[ST_CTRL_PLAY].joined = true;
	}

	/* Volume chord window elapsing without a partner -> fire the single
	 * action for whichever side is still down and unresolved. */
	if (s->ctrl[ST_CTRL_VOL_MINUS].down && !s->ctrl[ST_CTRL_VOL_MINUS].joined &&
	    (uint32_t)(now_ms - s->ctrl[ST_CTRL_VOL_MINUS].down_since_ms) > ST_GESTURE_VOLUME_CHORD_WINDOW_MS &&
	    out->count < ST_GESTURE_MAX_CMDS_PER_EVENT) {
		fire_single_volume_action(s, -1, out);
		s->ctrl[ST_CTRL_VOL_MINUS].joined = true; /* fired; release must not fire again */
	}
	if (s->ctrl[ST_CTRL_VOL_PLUS].down && !s->ctrl[ST_CTRL_VOL_PLUS].joined &&
	    (uint32_t)(now_ms - s->ctrl[ST_CTRL_VOL_PLUS].down_since_ms) > ST_GESTURE_VOLUME_CHORD_WINDOW_MS &&
	    out->count < ST_GESTURE_MAX_CMDS_PER_EVENT) {
		fire_single_volume_action(s, 1, out);
		s->ctrl[ST_CTRL_VOL_PLUS].joined = true;
	}
}

/* ----------------------------------------------------------- faders --- */
void st_gesture_arm_fader_pickup(st_gesture_state_t *s, uint8_t stem)
{
	if (stem < 4u) {
		s->fader_picked_up[stem] = false;
	}
}

void st_gesture_process_fader(st_gesture_state_t *s, uint8_t stem, uint16_t raw12,
			       uint16_t persisted_raw12, uint32_t now_ms, st_cmd_t *out)
{
	(void)now_ms;
	out->id = ST_CMD_NONE;
	if (stem >= 4u) {
		return;
	}

	if (s->fader_raw_last[stem] == 0xFFFFu) {
		/* First-ever sample: this is an "initial fader snapshot" --
		 * never user input. Seed the cache, require pickup. */
		s->fader_raw_last[stem] = raw12;
		s->fader_picked_up[stem] = false;
		return;
	}

	/* Captured ONCE, before any mutation this call: both the pickup
	 * crossing test and the deadband delta below must compare against
	 * the value from the PREVIOUS call, not against `raw12` itself. */
	uint16_t prev = s->fader_raw_last[stem];

	if (!s->fader_picked_up[stem]) {
		/* Pickup: needs the physical fader to CROSS the persisted
		 * level (not just be near it) before it takes effect. */
		bool crossed = (prev <= persisted_raw12 && raw12 >= persisted_raw12) ||
			       (prev >= persisted_raw12 && raw12 <= persisted_raw12);

		if (!crossed) {
			s->fader_raw_last[stem] = raw12;
			return;
		}
		s->fader_picked_up[stem] = true;
		/* falls through: the crossing sample itself is reported */
	}

	{
		int32_t delta = (int32_t)raw12 - (int32_t)prev;

		if (delta > -(int32_t)ST_GESTURE_FADER_DEADBAND &&
		    delta < (int32_t)ST_GESTURE_FADER_DEADBAND) {
			s->fader_raw_last[stem] = raw12;
			return; /* jitter suppressed */
		}
	}
	s->fader_raw_last[stem] = raw12;
	out->id = ST_CMD_FADER_LEVEL;
	out->stem = stem;
	out->value_q8 = (uint8_t)(raw12 >> 4); /* 12-bit -> 8-bit, same shift convention as elsewhere */
}
