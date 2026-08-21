/*
 * st_ctl.h — THE Stem Tape control dispatcher.
 *
 * PURE: no Zephyr, no ADC, no clock, no allocation. main.c samples the rails
 * once per control pass, hands the raw values here, and acts on what comes
 * back. This module owns the whole arbitration:
 *
 *   one ladder sample
 *     -> st_ladder: one stable physical classification
 *       -> a Track mask, or a PLAY event
 *         -> the PLAY/FUNCTION/VOLUME gesture state machine
 *           -> st_loop: transport, window, direction, latch
 *             -> ONE published output the mixer and the LEDs both consume
 *
 * ======================================================================
 * WHY THIS FILE EXISTS
 * ======================================================================
 * st15 had no such path. The same raw ladder value was interpreted twice --
 * by the inherited decode_tracks() and by a separate chord decoder -- and
 * PLAY had two owners with two thresholds: the inherited Tape Looper's
 * 400 ms hold-to-restart and the loop's own 450 ms. On real hardware the
 * loop did eventually run, so the collision was not fatal; what it produced
 * was a restart firing first, the transport dropping while it re-seeked and
 * re-primed the read-ahead ring, and the loop unable to enter until playback
 * came back. That is the three-to-four seconds the player felt.
 *
 * FUNCTION was worse than delayed -- it was unreachable. main.c's FUNCTION
 * branch ends every path in `continue`, and the loop was ticked below it, so
 * `function_down` was a structural constant false and no latch could ever
 * happen. That is why this module is called from ABOVE that branch, and why
 * it reports back which presses it consumed.
 *
 * WHENEVER A STEM TAPE SONG IS SELECTED this module is the sole owner of the
 * Track buttons and of PLAY, including the ordinary play/stop tap. With no
 * stem song it publishes nothing and the inherited engine is untouched --
 * that behaviour is a different instrument's and is not ours to redefine.
 *
 * ======================================================================
 * TIMING
 * ======================================================================
 * main.c's control loop runs on an ~8 ms cadence. Response budgets:
 *
 *   Track / chord     3 settled reads       ~24 ms
 *   Track release     2 confirmed idle reads ~16 ms
 *   PLAY tap          3 settled reads        ~24 ms after release
 *   Loop entry        ST_LOOP_HOLD_MS + settle, ~474 ms worst case
 *
 * The loop-entry figure is the threshold plus the PLAY debounce; nothing
 * else is added, and there is no second owner to lose a race to.
 */

#ifndef ST_CTL_H_
#define ST_CTL_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_ladder.h"
#include "st_loop.h"

/* How many consecutive agreeing reads commit a VOLUME button. The same
 * discipline the ladder uses, for the same reason. */
#define ST_CTL_VOL_SETTLE 3u

/* Why a PLAY hold did not produce a loop. Reported so the firmware can say
 * so over CDC instead of silently doing nothing. */
typedef enum {
	ST_CTL_REFUSE_NONE = 0,
	ST_CTL_REFUSE_NO_TEMPO,   /* frames_per_beat == 0 for this song */
	ST_CTL_REFUSE_NO_ROOM,    /* the capture point is at/past the song end */
} st_ctl_refuse_t;

typedef struct {
	/* ONE ladder sample per pass. -1 means the ADC read failed; that is a
	 * HOLD, never a release. */
	int      ladder_raw;

	/* VOLUME, decoded to a direction by main.c's existing decode_vol():
	 * -1 = Volume -, 0 = none, +1 = Volume +. Debounced HERE. */
	int8_t   vol_dir;

	/* The FUNCTION GPIO, live. A separate pin, so holding it does not move
	 * the ladder voltage. */
	bool     function_down;

	bool     stem_song;        /* a Stem Tape song is selected */
	bool     playing;          /* transport actually running */

	/* THE AUDIO THREAD'S OWN published playhead, not a control-thread
	 * estimate. This is the frame the player is hearing right now, and it
	 * is what a PLAY-down edge captures. */
	uint32_t song_frame;
	uint32_t song_frames;
	uint32_t frames_per_beat;

	uint32_t now_ms;
} st_ctl_in_t;

typedef struct {
	/* ---- what the mixer and the LEDs BOTH consume -------------------- */
	uint8_t  track_mask;       /* bits 0..3; 0 == nothing held */

	/* ---- transport ---------------------------------------------------- */
	bool     play_tap;         /* a completed short press: toggle play/stop */

	/* ---- loop, as one-shot actions ------------------------------------ */
	bool     loop_arm;         /* candidate captured: pin both sectors NOW */
	bool     loop_enter;       /* SEEK to loop_start, then start wrapping   */
	bool     loop_resize;
	bool     loop_latch;
	bool     loop_direction;
	bool     loop_exit;        /* stop wrapping, SEEK to loop_resume        */

	/* ---- loop, as levels ---------------------------------------------- */
	bool     loop_active;
	bool     loop_latched;
	bool     loop_reverse;
	uint32_t loop_start;       /* inclusive */
	uint32_t loop_end;         /* EXCLUSIVE -- half-open, everywhere */
	uint32_t loop_resume;      /* first normal frame after an exit */

	/* ---- what main.c must have resident before the loop runs ---------- */
	bool     pin_valid;
	uint32_t pin_entry_frame;  /* the seek-back target: loop/candidate start */
	uint32_t pin_exit_frame;   /* the window's LAST frame; see st_ctl.c      */

	/* ---- what main.c must NOT also act on ----------------------------- */
	bool     function_consumed;/* this FUNCTION press belongs to the loop */
	bool     vol_consumed;     /* this VOLUME press belongs to the loop   */

	/* ---- diagnosis, never a silent no-op ------------------------------ */
	st_ctl_refuse_t refused;   /* set once, on the pass the hold expired */
} st_ctl_out_t;

typedef struct {
	st_ladder_t ladder;
	st_loop_t   loop;

	bool     play_prev;
	uint32_t play_down_ms;
	bool     play_hold_spent;  /* this press already crossed the threshold */

	int8_t   vol_cand;
	uint8_t  vol_cand_n;
	int8_t   vol_settled;

	bool     fn_consumed;      /* latched for the rest of this FN press */
	bool     seeded;
} st_ctl_t;

/*
 * Cold boot. Resets the ladder, the loop (which is where the one-bar default
 * lives) and every gesture edge. Call once at startup and never again -- a
 * mid-session reset would silently discard the player's chosen division.
 */
void st_ctl_reset(st_ctl_t *c);

/*
 * ONE control pass. Total and deterministic: same state plus same inputs
 * gives the same output, with no hidden time source -- `now_ms` is the only
 * clock and it is supplied.
 *
 * `out` is fully written on every call; the caller never needs to clear it.
 * With `stem_song` false everything is published as inactive and the caller
 * must fall through to the inherited engine.
 */
void st_ctl_service(st_ctl_t *c, const st_ctl_in_t *in, st_ctl_out_t *out);

#endif /* ST_CTL_H_ */
