/*
 * st_fnplay.h -- the FUNCTION + PLAY tap gesture: x1 slow playback, x2 snap
 * home. Pure arbitration, no I/O, so the one property that matters can be
 * host-tested.
 *
 * ======================================================================
 * WHY THIS GESTURE AND NOT FX + PLAY
 * ======================================================================
 * The companion app is the specification here. src/input/gestures.ts puts
 * slow playback on FUNCTION + PLAY, deferred, with three mutually exclusive
 * outcomes:
 *
 *     x1  half speed        x2  snap to 1.0x        x3  Heads
 *
 * and marks PLAY-while-FUNCTION-qualifies as a DEFERRED control precisely
 * because those three cannot be told apart at the first tap. This module is
 * the firmware half of that decision.
 *
 * x3 (Heads) does not exist in this firmware and is NOT invented here. Only
 * the two outcomes that have a real implementation are arbitrated; a third
 * tap simply lands as a fresh first tap, which is the honest behaviour for a
 * gesture that is not implemented rather than a silent misfire.
 *
 * ======================================================================
 * THE WINDOW IS THE FIRMWARE'S OWN, NOT THE COMPANION'S
 * ======================================================================
 * gestures.ts uses fnPlayDecisionMs = 300. This module uses 450, because
 * FUNCTION + PLAY in main.c ALREADY had a 450 ms double-tap window on exactly
 * this gesture (the snap that this module now owns), and a player's fingers
 * are already calibrated to it. Reusing the existing figure is the same
 * decision made for the rocker's semitones, which took the Tape Looper's own
 * 350 ms rather than inventing one: where the device already has an answer,
 * the device's answer wins over the companion's.
 *
 * ======================================================================
 * WHAT MUST NOT HAPPEN, AND WHY THE SINGLE IS HELD
 * ======================================================================
 * x1 and x2 are mutually exclusive. Firing x1 optimistically on the first tap
 * and then "correcting" on the second would drop the song to half speed for
 * up to 450 ms before snapping home -- an audible octave dive on a gesture
 * whose entire purpose is to come home cleanly. So a single tap is HELD and
 * committed only once the window closes with no second tap. A double
 * therefore NEVER emits the single action at all: not as a timing accident,
 * but because the only place x1 is emitted is the window expiry.
 *
 * The cost is that x1 is one window late. That is the same trade the rocker's
 * semitones already make, for the same reason, and it is what "deferred"
 * means in the companion's own model.
 *
 * ======================================================================
 * WHAT THIS MODULE DOES NOT TOUCH
 * ======================================================================
 * FUNCTION + PLAY already carries two LONGER gestures in main.c: a release
 * between 350 ms and 5 s toggles the loop-length mode, and a hold through 5 s
 * toggles brightness. Neither is a TAP, and neither passes through here --
 * main.c calls st_fnplay_cancel() when a press turns out to be one of them,
 * so a hold can never also leave a tap pending. That is the collision this
 * module is written around: the tap slot was free, the hold slots were not.
 */

#ifndef STEMTAPE_PLAYER_FNPLAY_H_
#define STEMTAPE_PLAYER_FNPLAY_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * A press longer than this is a HOLD, not a tap, and belongs to the
 * mode/brightness gestures main.c owns. Matches main.c's own 350 ms mode
 * threshold so there is exactly one boundary between "tap" and "hold" rather
 * than two that could drift apart.
 */
#define ST_FNPLAY_TAP_MAX_MS 350u

/* The double-tap window -- main.c's own long-standing figure. See above. */
#define ST_FNPLAY_DOUBLE_MS 450u

typedef enum {
	ST_FNPLAY_ACT_NONE = 0,
	ST_FNPLAY_ACT_SLOW,   /* x1: toggle slow playback */
	ST_FNPLAY_ACT_SNAP,   /* x2: snap home -- unity pitch, slow off */
} st_fnplay_action_t;

/*
 * ======================================================================
 * THE WINDOW IS PRESS-TO-PRESS, NOT RELEASE-TO-RELEASE
 * ======================================================================
 * A double tap is defined by WHEN THE PLAYER PRESSES, not by how long they
 * happen to hold. Timing it from the releases looks equivalent and is not: a
 * second press landing at 440 ms -- comfortably inside the window -- but held
 * for 40 ms releases at 480 ms, outside it, and the first tap's single would
 * already have been emitted. The gesture would then fire slow AND snap, which
 * is the one outcome this module exists to prevent, and it would do it only
 * for players who press deliberately rather than flick.
 *
 * So the press edge opens and closes the window, and the release exists only
 * to answer a different question: was this a TAP at all, or a hold belonging
 * to the mode/brightness gestures? main.c's own long-standing code already
 * fires the snap on the press edge for the same reason; this preserves that.
 */
typedef struct {
	bool     pend;        /* a short tap waiting out its window */
	uint32_t pend_ms;     /* the PRESS edge of that tap */
	bool     have_tap;    /* a previous tap is still inside the window */
	uint32_t last_ms;     /* that tap's PRESS edge */
	bool     down;        /* a press is in progress */
	uint32_t down_ms;     /* when it started */
	bool     spent;       /* the current press already produced an action */
} st_fnplay_t;

/* Drops any pending tap and forgets the window. */
void st_fnplay_reset(st_fnplay_t *f);

/*
 * THE PLAY PRESS EDGE, while FUNCTION is held.
 *
 * Returns ST_FNPLAY_ACT_SNAP when this press completed a double, in which
 * case the pending single is discarded WITHOUT EVER HAVING BEEN EMITTED and
 * the press is marked spent so its release cannot also arm a tap.
 */
st_fnplay_action_t st_fnplay_press(st_fnplay_t *f, uint32_t now_ms);

/*
 * THE PLAY RELEASE EDGE. Arms the deferred single only if the press was short
 * enough to be a tap and was not already spent on the snap. A longer press
 * belongs to the mode or brightness gesture and is dropped here, so main.c
 * does not have to remember to cancel it.
 *
 * Never emits: the single is only ever emitted by st_fnplay_tick().
 */
void st_fnplay_release(st_fnplay_t *f, uint32_t now_ms);

/*
 * Call every control pass, INCLUDING passes where FUNCTION is no longer held.
 *
 * A tap's window outlives the buttons that made it: the player can release
 * FUNCTION and PLAY well before 450 ms have passed, and the single still owes
 * them an action. The companion says the same thing about its own deferred
 * taps -- they "commit up to trackDecisionMs after release, long after
 * FUNCTION may have been released". A tick that only ran while FUNCTION was
 * down would drop most real single taps on the floor.
 */
st_fnplay_action_t st_fnplay_tick(st_fnplay_t *f, uint32_t now_ms);

/*
 * The press turned out to be a HOLD (mode toggle, brightness) or was
 * otherwise consumed. Drops anything pending so one press cannot produce two
 * actions, and clears the double-tap window so the NEXT tap starts fresh
 * rather than pairing with a tap that was already spent on something else.
 */
void st_fnplay_cancel(st_fnplay_t *f);

/* True while a tap is waiting out its window -- diagnostics only. */
static inline bool st_fnplay_pending(const st_fnplay_t *f)
{
	return f->pend;
}

#endif /* STEMTAPE_PLAYER_FNPLAY_H_ */
