/*
 * st_pwr_idle.h -- the stock SP-1's ~5-minute idle auto-shutdown, and the
 * governor that combines it with the manual power contract.
 *
 * ======================================================================
 * WHY THIS IS NOT "TIME SINCE LAST BUTTON PRESS"
 * ======================================================================
 *
 * Stem Tape can be used like an iPod. A song can easily run longer than five
 * minutes, and a player listening to one is USING the instrument even though
 * they have not touched a control since they pressed PLAY. A timer that only
 * measured human input would switch the device off in the middle of a
 * seven-minute song, which is a worse failure than the one it prevents.
 *
 * So the rule is stated about the INSTRUMENT, not about the user:
 *
 *     idle = the transport is not actively traversing the song
 *            AND there has been no intentional physical control activity
 *
 *     STOPPED and untouched for ST_PWR_IDLE_MS  ->  OFF
 *
 * ======================================================================
 * WHAT "TRANSPORT ACTIVE" MEANS, AND WHAT IT DELIBERATELY DOES NOT
 * ======================================================================
 *
 * This module does not compute transport state -- it is told. The caller's
 * definition is the safety-relevant part, so it is written down here and
 * asserted by CI at the call site:
 *
 *     transport_active = g_playing || st_inertia_moving(&s_stem_inertia)
 *
 * THE REEL IS TURNING. That is the semantic test, and it is the right one
 * because every mode that manipulates audio drives the same reel: normal
 * playback, loop playback, reverse, slow mode, pitch/varispeed, FX over live
 * playback, solo/mute while playing, and every future scratch, scrub or
 * shuttle mode. None of them needs a special case here, and none of them can
 * be forgotten -- a transport mode that did not move the reel would not
 * produce audio either.
 *
 * st_inertia_moving() is what keeps a STOP from becoming a mute: the reel
 * spins down over several hundred milliseconds after g_playing goes false, and
 * that spin-down is audible, pitched audio read from the tape. It is playback,
 * so it is not idle, and the idle clock starts when the reel actually stops.
 *
 * WHAT IS NOT TRANSPORT ACTIVITY, stated as a list because the tempting wrong
 * definitions are all on it: LED animation, meter animation, housekeeping,
 * diagnostics, counters, background USB servicing, background eMMC work,
 * streamer bookkeeping while stopped, a feature flag merely remaining set,
 * stale combo state, stale gesture ownership, and stale reverse/FX/solo state
 * while nothing is playing. None of those touches g_playing or the inertia
 * state, which is exactly why this definition was chosen over "the audio
 * callback is running" or "the streamer thread is alive" -- both of which are
 * true forever and would disable the idle shutdown entirely.
 *
 * AN ACTIVE USB UPLOAD IS ALSO ACTIVE USE.
 *
 * `transfer_active` enters the idle rule exactly as `transport_active` does,
 * and for the same reason: a multi-minute upload is the instrument being used,
 * and nobody is touching a control during it. It enters the MANUAL rule not at
 * all. A deliberate FUNCTION-only 5.000 s hold must switch the device off
 * during an upload -- the A/B commit design makes that safe by construction
 * (an interrupted upload writes only the frozen INACTIVE pair, and the single
 * 512-byte magic block is the only thing that can promote it), so there is no
 * integrity reason to suppress the escape hatch and every safety reason not to.
 *
 * ======================================================================
 * THREE INDEPENDENT PROTECTIONS
 * ======================================================================
 *
 *   A. deliberate wake       FUNCTION-only 2.000 s            -> ON
 *   B. deliberate shutdown   after release-to-rearm,
 *                            FUNCTION-only 5.000 s            -> OFF
 *   C. idle shutdown         true inactivity 300.000 s        -> OFF
 *
 * They converge on one platform power request and NOTHING ELSE. Their
 * qualification logic is disjoint: the idle clock never reads off_armed,
 * function_consumed, the hold's elapsed time or any feature flag; the manual
 * hold never reads the idle clock or transport_active. st_pwr_service() does
 * not even receive transport_active -- st_pwr_in_t has no such field, and CI
 * asserts that st_pwr_hold.c never mentions transport at all. So a broken
 * idle path cannot disable the manual escape hatch, and a manual path stuck
 * disarmed cannot disable the idle shutdown.
 *
 *   D. bootloader recovery   Track 1 + Track 4 + USB
 *
 * D is outside the application entirely and none of the above can reach it.
 */

#ifndef STEMTAPE_PLAYER_PWR_IDLE_H_
#define STEMTAPE_PLAYER_PWR_IDLE_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_pwr_hold.h"

/* Five minutes. The stock SP-1's behaviour, and a second escape hatch: if some
 * future feature ever broke the manual FUNCTION shutdown, a stopped device
 * still switches itself off rather than draining the battery -- which is the
 * failure this whole Stage 0 exists because of. */
#define ST_PWR_IDLE_MS 300000

/*
 * THE IDLE FADER DETECTOR IS A SECOND, SEPARATE ONE, and that is deliberate.
 *
 * The hold's detector (st_pwr_t) is TRANSACTION-SCOPED: it is sampled only
 * while FUNCTION is down and every baseline is dropped on the rising edge, so
 * nothing a fader did before a power hold can affect that hold. That is the
 * fix for the 24.7-second defect and it must not be weakened.
 *
 * But idle needs the opposite: a fader moved with FUNCTION up is exactly the
 * intentional activity that should keep the device awake, so this one is
 * sampled CONTINUOUSLY. Keeping them as two independent st_pwr_fader_t
 * instances is what lets both be true at once. Neither can see the other's
 * baselines, so idle history cannot leak into a power transaction.
 */
typedef struct {
	int64_t        last_active_ms;
	bool           seeded;
	st_pwr_fader_t fader;
} st_pwr_idle_t;

/* Fresh. No history, no fader baselines, clock unstarted -- the next tick
 * seeds it, so a wake never inherits an elapsed idle time from before. */
void st_pwr_idle_init(st_pwr_idle_t *i);

/*
 * One pass. Returns the idle elapsed time in milliseconds.
 *
 * `transport_active` and `activity` are ORed into "the instrument is in use";
 * while either is true the clock is pinned at zero and the last-active stamp
 * follows `now_ms`. There is no pause, no bank and no partial credit: when use
 * stops, a full ST_PWR_IDLE_MS of continuous non-use is required.
 */
int64_t st_pwr_idle_tick(st_pwr_idle_t *i, bool transport_active,
			  bool activity, int64_t now_ms);

/* Sample one fader for the idle detector. Unlike the hold's, this is called on
 * every pass regardless of FUNCTION. raw < 0 (an ADC error) is not movement. */
bool st_pwr_idle_fader(st_pwr_idle_t *i, uint32_t idx, int32_t raw,
			int64_t now_ms);

static inline bool st_pwr_idle_due(int64_t elapsed_ms)
{
	return elapsed_ms >= ST_PWR_IDLE_MS;
}

static inline int64_t st_pwr_idle_elapsed_ms(const st_pwr_idle_t *i,
					      int64_t now_ms)
{
	if (!i->seeded || now_ms <= i->last_active_ms) {
		return 0;
	}
	return now_ms - i->last_active_ms;
}

/*
 * ======================================================================
 * THE GOVERNOR -- the whole decision, one call, immediately before power_off()
 * ======================================================================
 *
 * This exists because of the 24.7-second defect and the rule drawn from it:
 * the layer with the tests must be the layer that can be wrong. Combining the
 * manual and idle verdicts in main.c would put an untested boolean expression
 * between the tested modules and the platform power call, which is precisely
 * the shape of that defect.
 *
 * So the combination lives here, is pure, and is driven end to end by
 * tests/test_power_idle.c. What remains in main.c is five reads and one call.
 */

/* One control pass, as the caller observed it. Every field is a PHYSICAL or
 * TRANSPORT fact; there is no field a feature could set to change a verdict. */
typedef struct {
	bool    fn_down;           /* FUNCTION, straight off its own GPIO */
	bool    ain0_active;       /* settled PLAY/Track state */
	bool    ain1_active;       /* decoded VOL / rocker / FX chord != none */
	int32_t fader_raw;         /* this pass's round-robin reading, <0 = none */
	uint8_t fader_idx;         /* which fader that reading belongs to */
	bool    transport_active;  /* THE REEL IS TURNING -- see the top of file */
	bool    transfer_active;   /* a USB block upload is in progress */
	int64_t now_ms;
} st_pwr_gov_in_t;

typedef struct {
	/* A -- the wake */
	bool    on_due;
	/* B -- the manual hold */
	int64_t hold_elapsed_ms;
	bool    off_due;
	bool    off_armed;
	bool    other_active;
	bool    began;
	/* C -- idle */
	int64_t idle_elapsed_ms;
	bool    idle_off_due;
	bool    in_use;            /* why the idle clock is or is not running */
	/* the one thing the caller acts on for shutdown */
	bool    power_off_request; /* off_due || idle_off_due */
	bool    device_on;
} st_pwr_gov_out_t;

typedef struct {
	st_pwr_t      pwr;
	st_pwr_idle_t idle;
} st_pwr_gov_t;

/* The two entry points, mirroring st_pwr_init_off()/st_pwr_init_on(). Both
 * start the manual side DISARMED (release-to-rearm) and the idle side FRESH. */
void st_pwr_gov_init_off(st_pwr_gov_t *g);
void st_pwr_gov_init_on(st_pwr_gov_t *g);

/*
 * ONE PASS, EVERY POWER DECISION. Pure: no Zephyr, no GPIO, no ADC, no clock
 * of its own, no allocation, bounded time. `out` is fully written every call.
 *
 * On the OFF -> ON transition the idle system is RE-INITIALISED, so no idle
 * time accumulated before the device came up can carry into the session. The
 * wake press itself is ordinary activity while it is held, and the moment it
 * is released the idle clock starts from zero like any other release.
 */
void st_pwr_gov_service(st_pwr_gov_t *g, const st_pwr_gov_in_t *in,
			 st_pwr_gov_out_t *out);

#endif /* STEMTAPE_PLAYER_PWR_IDLE_H_ */
