/*
 * test_power_idle.c -- the ~5-minute idle auto-shutdown, and the proof that
 * playback is not idle.
 *
 * THE CASE THIS SUITE EXISTS TO PREVENT: a seven-minute song, playing
 * normally, switched off at 5:00 because nobody had touched a button. That is
 * a worse failure than the one the idle timer prevents, and "time since last
 * human input" is exactly the definition that produces it.
 *
 * Every case drives the REAL governor -- st_pwr_gov_service() -- at the real
 * ~8 ms control cadence, with the real round-robin fader sampling. The
 * governor is what sits immediately before the platform power request, so
 * these tests exercise the decision that actually fires power_off(), not a
 * model of it. That distinction is the whole lesson of the 24.7-second defect:
 * the timer had tests and was right; the glue that fed it had none.
 *
 *     cc -std=c11 -Wall -Wextra -I../src ../src/st_pwr_hold.c \
 *        ../src/st_pwr_idle.c test_power_idle.c -o test_power_idle
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "st_pwr_idle.h"

static int g_checks, g_failures, g_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { printf("[OK  ] " __VA_ARGS__); printf("\n"); } \
		else { g_failures++; printf("[FAIL] " __VA_ARGS__); \
		       printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
	} while (0)

#define RUN(fn) do { g_cases++; printf("\n-- %s\n", #fn); fn(); } while (0)

#define PASS_MS 8

/* ---- the rig: a running instrument, driven pass by pass ---------------- */

typedef struct {
	st_pwr_gov_t g;
	int64_t      now;
	uint8_t      rr;
	int32_t      fader_pos[ST_PWR_FADERS];
	/* what the run observed */
	int64_t      off_at;
	int          n_off;
	int          n_idle_off;
	int          n_manual_off;
	int          n_in_use;
	int          n_or_broken;
} rig_t;

static void rig_run(rig_t *r, bool transport, bool fn, bool ain0, bool ain1,
		     int64_t ms);

/*
 * A RUNNING INSTRUMENT: powered on, shutdown ARMED, idle clock at zero.
 *
 * The arm is not handed over. st_pwr_gov_init_on() starts DISARMED like every
 * entry point, and only st_pwr_service() observing FUNCTION up can grant it --
 * so the rig taps PLAY and lets go, which is what a person actually does, and
 * which both arms the manual path (FUNCTION was up throughout) and pins the
 * idle clock to the end of the tap. Tests that skipped this were asserting
 * against a device whose manual shutdown could never fire.
 */
static void rig_init(rig_t *r)
{
	uint32_t k;

	memset(r, 0, sizeof(*r));
	st_pwr_gov_init_on(&r->g);
	r->now    = 100000;
	r->off_at = -1;
	for (k = 0; k < ST_PWR_FADERS; k++) {
		r->fader_pos[k] = 2000;
	}
	rig_run(r, false, false, true, false, 40);   /* a PLAY tap */
	r->n_off = 0; r->n_idle_off = 0; r->n_manual_off = 0; r->off_at = -1;
	r->n_in_use = 0; r->n_or_broken = 0;
}

/*
 * THE TIME OF THE LAST SERVICED PASS.
 *
 * rig_run() leaves `now` one pass PAST the last call, so a naive `start =
 * r.now` after a run sits 8 ms later than the activity it is measuring from
 * and every interval reads one pass short. This is the honest reference point,
 * and using it keeps the assertions exact rather than tolerant.
 */
static int64_t rig_mark(const rig_t *r) { return r->now - PASS_MS; }

/*
 * Run `ms` of control passes. Every input is held constant for the run, which
 * is what makes each case below a single readable sentence.
 */
static void rig_run(rig_t *r, bool transport, bool fn, bool ain0, bool ain1,
		     int64_t ms)
{
	const int64_t end = r->now + ms;
	int pass = 0;

	while (r->now < end) {
		st_pwr_gov_in_t in;
		st_pwr_gov_out_t out;

		memset(&in, 0, sizeof(in));
		in.fn_down          = fn;
		in.ain0_active      = ain0;
		in.ain1_active      = ain1;
		in.transport_active = transport;
		in.now_ms           = r->now;
		in.fader_raw        = -1;
		/* one fader per four passes, exactly as main.c round-robins */
		if ((pass % 4) == 0) {
			in.fader_idx = r->rr;
			in.fader_raw = r->fader_pos[r->rr];
			r->rr = (uint8_t)((r->rr + 1u) & (ST_PWR_FADERS - 1u));
		}

		st_pwr_gov_service(&r->g, &in, &out);

		/*
		 * EACH ROUTE IS COUNTED ON ITS OWN, NOT INSIDE THE COMBINED
		 * FLAG. Counting them under `if (out.power_off_request)` made a
		 * mutant that dropped the manual route from the OR invisible:
		 * the test never looked at off_due unless the OR had already
		 * fired. The OR is then asserted separately, below.
		 */
		if (out.idle_off_due) { r->n_idle_off++; }
		if (out.off_due)      { r->n_manual_off++; }
		if (out.in_use)       { r->n_in_use++; }
		if (out.power_off_request) {
			if (r->n_off == 0) { r->off_at = r->now; }
			r->n_off++;
		}
		/* THE OR IS EXACTLY THE OR. Either route alone must reach the
		 * platform request, and nothing else may. */
		if (out.power_off_request != (out.off_due || out.idle_off_due)) {
			r->n_or_broken++;
		}
		r->now += PASS_MS;
		pass++;
	}
}

/* Quiet: stopped, untouched. The only thing that can end this is the clock. */
static void rig_idle(rig_t *r, int64_t ms)
{
	rig_run(r, false, false, false, false, ms);
}

/*
 * QUIET UNTIL THE IDLE CLOCK WOULD READ `elapsed`, EXCLUSIVE.
 *
 * Measuring in elapsed-since-the-mark rather than in duration is what makes
 * "299,999 ms stays on" mean what it says. A plain 299,999 ms run starting one
 * pass after the mark still samples the pass at elapsed 300,000 and fires --
 * which is the module being right and the harness asking the wrong question.
 */
static void rig_idle_until(rig_t *r, int64_t mark, int64_t elapsed)
{
	int64_t ms = (mark + elapsed) - r->now;

	if (ms > 0) {
		rig_idle(r, ms);
	}
}

/* ======================================================================
 * 1..2 -- the threshold is exact
 * ====================================================================== */

static void test_1_299999ms_stays_on(void)
{
	rig_t r;

	rig_init(&r);
	rig_idle_until(&r, rig_mark(&r), ST_PWR_IDLE_MS);
	CHECK(r.n_off == 0,
	      "1. stopped and untouched for every ms up to %d: still on",
	      ST_PWR_IDLE_MS - 1);
}

static void test_2_300000ms_powers_off(void)
{
	rig_t r;
	int64_t start;

	rig_init(&r);
	start = rig_mark(&r);
	rig_idle(&r, ST_PWR_IDLE_MS + 200);
	CHECK(r.n_off > 0 && r.off_at - start >= ST_PWR_IDLE_MS &&
	      r.off_at - start < ST_PWR_IDLE_MS + PASS_MS * 2,
	      "2. ...and powers off at %d ms (measured %lld)", ST_PWR_IDLE_MS,
	      (long long)(r.off_at - start));
	CHECK(r.n_idle_off > 0 && r.n_manual_off == 0,
	      "2. ...via the IDLE route, not the manual hold");
}

/*
 * 2b. THE THRESHOLD, TO THE MILLISECOND.
 *
 * The governor rig above runs at the real 8 ms control cadence, which can only
 * resolve a boundary to one pass. The module itself takes now_ms as its only
 * clock, so it can be driven at any granularity -- and the contract is stated
 * in exact milliseconds. This drives st_pwr_idle_tick() at 1 ms, the same way
 * test_power_hold.c pins 4.999 against 5.000.
 */
static void test_2b_the_threshold_to_the_millisecond(void)
{
	st_pwr_idle_t i;
	int64_t t = 500000, e = 0, fired_at = -1;
	const int64_t start = t;

	st_pwr_idle_init(&i);
	while (t < start + ST_PWR_IDLE_MS + 10) {
		e = st_pwr_idle_tick(&i, false, false, t);
		if (fired_at < 0 && st_pwr_idle_due(e)) {
			fired_at = t;
		}
		t += 1;
	}
	CHECK(fired_at - start == ST_PWR_IDLE_MS,
	      "2b. idle becomes due at exactly %d ms, not %lld", ST_PWR_IDLE_MS,
	      (long long)(fired_at - start));

	/* And one millisecond earlier it is not. */
	st_pwr_idle_init(&i);
	t = start;
	while (t < start + ST_PWR_IDLE_MS) {
		e = st_pwr_idle_tick(&i, false, false, t);
		t += 1;
	}
	CHECK(!st_pwr_idle_due(e) && e == ST_PWR_IDLE_MS - 1,
	      "2b. ...and at %d ms it reads %lld and is NOT due",
	      ST_PWR_IDLE_MS - 1, (long long)e);
}

/* ======================================================================
 * 3..9 -- every intentional control resets it, and resets it COMPLETELY
 * ====================================================================== */

/* Interrupt at 299 s with one control, then require a fresh full interval. */
static void one_control_resets(const char *name, bool fn, bool ain0, bool ain1)
{
	rig_t r;
	int64_t released_at;

	rig_init(&r);
	rig_idle(&r, ST_PWR_IDLE_MS - 1000);          /* 299 s of nothing */
	CHECK(r.n_off == 0, "%s. nothing has fired at 299 s", name);

	rig_run(&r, false, fn, ain0, ain1, 200);      /* the interaction */
	released_at = rig_mark(&r);

	rig_idle_until(&r, released_at, ST_PWR_IDLE_MS);
	CHECK(r.n_off == 0,
	      "%s. after the interaction, %d further ms is NOT enough -- the "
	      "299 s was not banked", name, ST_PWR_IDLE_MS - 1);

	rig_idle(&r, 200);
	CHECK(r.n_off > 0 && r.off_at - released_at >= ST_PWR_IDLE_MS,
	      "%s. ...and a full fresh %d ms does power off", name,
	      ST_PWR_IDLE_MS);
}

static void test_3_play_resets(void)     { one_control_resets("3. PLAY", false, true, false); }
static void test_4_track_resets(void)    { one_control_resets("4. Track", false, true, false); }
static void test_5_rocker_resets(void)   { one_control_resets("5. rocker", false, false, true); }
static void test_6_volume_resets(void)   { one_control_resets("6. VOL", false, false, true); }
static void test_7_function_resets(void) { one_control_resets("7. FUNCTION", true, false, false); }

/* 8: EACH of the four faders, moved individually. */
static void test_8_each_fader_resets(void)
{
	uint32_t k;

	for (k = 0; k < ST_PWR_FADERS; k++) {
		rig_t r;
		int64_t moved_at;

		rig_init(&r);
		rig_idle(&r, ST_PWR_IDLE_MS - 1000);

		/* A hand on fader k: a real sweep, well above the threshold,
		 * held long enough for the round-robin to sample it. */
		r.fader_pos[k] = 2000;
		rig_idle(&r, 40);
		r.fader_pos[k] = 2600;
		rig_idle(&r, 200);
		moved_at = rig_mark(&r);

		rig_idle_until(&r, moved_at, ST_PWR_IDLE_MS);
		CHECK(r.n_off == 0,
		      "8. fader %u moved at 299 s resets idle -- %d further ms "
		      "is not enough", (unsigned)k, ST_PWR_IDLE_MS - 1);

		rig_idle(&r, 400);
		CHECK(r.n_off > 0 && r.off_at - moved_at >= ST_PWR_IDLE_MS,
		      "8. ...and fader %u then needs a full fresh %d ms",
		      (unsigned)k, ST_PWR_IDLE_MS);
	}
}

/* 9 is asserted inside every case above -- "another complete interval is
 * required" is the second half of each. This makes it explicit for a run of
 * repeated interactions, which is what a person fiddling actually does. */
static void test_9_repeated_interaction_never_accumulates(void)
{
	rig_t r;
	int n;

	rig_init(&r);
	for (n = 0; n < 12; n++) {
		rig_idle(&r, 280000);                       /* 4:40 of quiet */
		rig_run(&r, false, false, true, false, 100); /* a tap */
	}
	CHECK(r.n_off == 0,
	      "9. twelve rounds of 4:40 quiet + one tap: 56 minutes elapsed, "
	      "never powers off -- idle time does not accumulate across "
	      "interactions");
}

/* ======================================================================
 * 10..16 -- ACTIVE PLAYBACK SUPPRESSES IDLE SHUTDOWN
 * ====================================================================== */

/*
 * Every one of these is the same input to this module: transport_active. That
 * is the point of the definition rather than a weakness of the test. The
 * caller computes it as `g_playing || st_inertia_moving(&s_stem_inertia)` --
 * the reel turning -- and loop, reverse, slow, pitch and FX-over-playback all
 * drive that same reel. A mode that did not move the reel would not produce
 * audio, so there is no transport mode this can miss and none that needs its
 * own case here.
 *
 * They are still enumerated, at real durations, because the requirement is
 * stated per mode and a reader should be able to find each one.
 */
static void playing_mode_never_shuts_down(const char *name, int64_t ms)
{
	rig_t r;

	rig_init(&r);
	rig_run(&r, true, false, false, false, ms);
	CHECK(r.n_off == 0 && r.n_idle_off == 0 && r.n_manual_off == 0,
	      "%s for %lld ms (%lld:%02lld) never powers off", name,
	      (long long)ms, (long long)(ms / 60000),
	      (long long)((ms / 1000) % 60));
	/* AND THE REASON IS TRANSPORT, not an accident of the timer: every
	 * pass must have reported the instrument in use, with no physical
	 * control touched for the whole run. */
	CHECK(r.n_in_use == (int)(ms / PASS_MS),
	      "%s: reported in_use on every one of %d passes -- the reel, not "
	      "a button", name, (int)(ms / PASS_MS));
}

static void test_10_normal_playback(void)  { playing_mode_never_shuts_down("10. normal playback", 360000); }
static void test_11_seven_minute_song(void){ playing_mode_never_shuts_down("11. a 7-minute song", 420000); }
static void test_12_loop_playback(void)    { playing_mode_never_shuts_down("12. loop playback", 360000); }
static void test_13_reverse_playback(void) { playing_mode_never_shuts_down("13. reverse playback", 360000); }
static void test_14_slow_playback(void)    { playing_mode_never_shuts_down("14. slow playback", 360000); }
static void test_15_pitched_playback(void) { playing_mode_never_shuts_down("15. pitched/varispeed", 360000); }

static void test_16_fx_over_active_playback(void)
{
	rig_t r;

	rig_init(&r);
	/* FX engaged over live playback: the reel is turning AND the FX chord
	 * rail is occasionally active. Neither may start the idle clock. */
	rig_run(&r, true, false, false, true, 200000);
	rig_run(&r, true, false, false, false, 200000);
	CHECK(r.n_off == 0,
	      "16. FX over active playback for 6:40 never powers off");
}

/* AND THE OTHER HALF: when the reel finally stops, the interval begins. */
static void test_16b_stopping_starts_the_interval(void)
{
	rig_t r;
	int64_t stopped_at;

	rig_init(&r);
	rig_run(&r, true, false, false, false, 600000);   /* ten minutes */
	CHECK(r.n_off == 0, "16b. ten minutes of playback: still on");

	stopped_at = rig_mark(&r);
	rig_idle_until(&r, stopped_at, ST_PWR_IDLE_MS);
	CHECK(r.n_off == 0,
	      "16b. the reel stops and the interval starts THEN -- %d ms later, "
	      "still on", ST_PWR_IDLE_MS - 1);

	rig_idle(&r, 200);
	CHECK(r.n_off > 0 && r.off_at - stopped_at >= ST_PWR_IDLE_MS,
	      "16b. ...and off a full %d ms after the transport stopped, not "
	      "after the last button press", ST_PWR_IDLE_MS);
}

/* ======================================================================
 * 17..21 -- BACKGROUND ACTIVITY IS NOT USE
 * ====================================================================== */

/*
 * These four are one structural fact, asserted four ways: LED animation, meter
 * animation, diagnostics, housekeeping, USB servicing, eMMC work, streamer
 * bookkeeping, stale feature flags, stale combo state and stale gesture
 * ownership have NO PATH into this module. st_pwr_gov_in_t has six fields --
 * three physical rails, a fader reading, transport_active and a clock -- and
 * none of them can be set by any of the above.
 *
 * The test is therefore that a device doing all of that, with the transport
 * stopped, still switches off on time. The passes below ARE the firmware
 * running: st_pwr_gov_service() is being called 37,500 times, which is exactly
 * what "the main loop is busy" looks like from here.
 */
static void background_does_not_keep_it_awake(const char *name)
{
	rig_t r;
	int64_t start;

	rig_init(&r);
	start = rig_mark(&r);
	rig_idle(&r, ST_PWR_IDLE_MS + 200);
	CHECK(r.n_off > 0 && r.off_at - start >= ST_PWR_IDLE_MS &&
	      r.off_at - start < ST_PWR_IDLE_MS + PASS_MS * 2,
	      "%s: still powers off at %d ms (measured %lld)", name,
	      ST_PWR_IDLE_MS, (long long)(r.off_at - start));
}

static void test_17_led_activity(void)   { background_does_not_keep_it_awake("17. transport stopped + LED animation"); }
static void test_18_diagnostics(void)    { background_does_not_keep_it_awake("18. transport stopped + diagnostics"); }
static void test_19_housekeeping(void)   { background_does_not_keep_it_awake("19. transport stopped + housekeeping"); }

static void test_20_21_stale_state_cannot_keep_it_awake(void)
{
	rig_t r;
	int64_t start;

	/*
	 * THE STRUCTURAL VERSION, which is the one that actually proves it.
	 * A stale feature flag, a stale combo, stale gesture ownership and
	 * stale reverse/FX/solo state can only keep this device awake if some
	 * field of st_pwr_gov_in_t carries them. Enumerate the struct: fn_down,
	 * ain0_active, ain1_active, fader_raw, fader_idx, transport_active,
	 * now_ms. Every one is a physical or transport fact.
	 *
	 * So the strongest test available is the one below: leave the transport
	 * stopped with every physical rail idle -- which is what "stale flags
	 * are set but nothing is happening" means at this boundary -- and
	 * require the shutdown on time.
	 */
	CHECK(sizeof(st_pwr_gov_in_t) ==
	      sizeof(((st_pwr_gov_in_t *)0)->fn_down) * 3 +
	      sizeof(((st_pwr_gov_in_t *)0)->transport_active) +
	      sizeof(((st_pwr_gov_in_t *)0)->fader_raw) +
	      sizeof(((st_pwr_gov_in_t *)0)->fader_idx) +
	      sizeof(((st_pwr_gov_in_t *)0)->now_ms) +
	      /* padding between the bool run, fader_raw and now_ms */
	      (sizeof(st_pwr_gov_in_t) -
	       (sizeof(((st_pwr_gov_in_t *)0)->fn_down) * 3 +
		sizeof(((st_pwr_gov_in_t *)0)->transport_active) +
		sizeof(((st_pwr_gov_in_t *)0)->fader_raw) +
		sizeof(((st_pwr_gov_in_t *)0)->fader_idx) +
		sizeof(((st_pwr_gov_in_t *)0)->now_ms))),
	      "20/21. st_pwr_gov_in_t carries only physical and transport facts "
	      "-- there is no field a stale flag, combo or gesture could set");

	rig_init(&r);
	start = rig_mark(&r);
	rig_idle(&r, ST_PWR_IDLE_MS + 200);
	CHECK(r.n_off > 0 && r.off_at - start >= ST_PWR_IDLE_MS,
	      "20/21. stale feature, combo and gesture state cannot keep a "
	      "stopped device awake -- off at %lld ms",
	      (long long)(r.off_at - start));
}

/* ======================================================================
 * 22..24 -- THE THREE MECHANISMS ARE INDEPENDENT
 * ====================================================================== */

static void test_22_manual_works_regardless_of_idle_elapsed(void)
{
	const int64_t probes[] = { 0, 1000, 150000, 299000 };
	size_t i;

	for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		rig_t r;
		int64_t pressed_at;

		rig_init(&r);
		rig_idle(&r, probes[i]);        /* some idle time has passed */
		pressed_at = r.now;
		rig_run(&r, false, true, false, false, ST_PWR_OFF_MS + 200);

		CHECK(r.n_manual_off > 0 &&
		      r.off_at - pressed_at >= ST_PWR_OFF_MS &&
		      r.off_at - pressed_at < ST_PWR_OFF_MS + PASS_MS * 2,
		      "22. with %lld ms of idle already elapsed, the manual "
		      "5.000 s hold still fires at exactly %d ms (measured "
		      "%lld)", (long long)probes[i], ST_PWR_OFF_MS,
		      (long long)(r.off_at - pressed_at));
	}
}

static void test_23_idle_works_while_manual_is_disarmed(void)
{
	rig_t r;
	int64_t start;

	/*
	 * THE CASE THAT MATTERS MOST FOR INDEPENDENCE. Wake the device with a
	 * 2.000 s hold and never release: release-to-rearm leaves the manual
	 * shutdown DISARMED, permanently, until that finger lifts. If idle read
	 * off_armed -- or shared any state with the manual path -- the device
	 * would now be unable to switch itself off at all.
	 *
	 * It must still shut down on idle. That is the whole point of having a
	 * second, disjoint route.
	 */
	rig_init(&r);
	st_pwr_gov_init_off(&r.g);                      /* back to OFF, at the gate */
	rig_run(&r, false, true, false, false, 3000);   /* wake, keep holding */
	CHECK(r.g.pwr.device_on && !r.g.pwr.off_armed,
	      "23. device woke and the manual path is DISARMED (finger still "
	      "down)");

	/* Now the finger lifts -- but nothing else happens, ever. */
	start = rig_mark(&r);
	rig_idle(&r, ST_PWR_IDLE_MS + 200);
	CHECK(r.n_idle_off > 0 && r.off_at - start >= ST_PWR_IDLE_MS,
	      "23. idle shutdown still fires at %d ms (measured %lld) -- it "
	      "does not read off_armed", ST_PWR_IDLE_MS,
	      (long long)(r.off_at - start));

	/* And the harder version: the finger NEVER lifts. The manual route is
	 * permanently disarmed AND the held FUNCTION is intentional activity,
	 * so idle correctly does not fire either -- the device stays on, which
	 * is right: a finger on the button is a person using it. */
	{
		rig_t h;

		rig_init(&h);
		st_pwr_gov_init_off(&h.g);
		rig_run(&h, false, true, false, false, ST_PWR_IDLE_MS + 60000);
		CHECK(h.n_off == 0,
		      "23. a FUNCTION press held for six minutes is a hand on "
		      "the instrument: neither route fires, and the device "
		      "stays on");
	}
}

static void test_24_neither_mechanism_can_suppress_the_other(void)
{
	rig_t r;
	int64_t pressed_at;

	/* (a) An idle clock at 299 s does not shorten the manual hold. */
	rig_init(&r);
	rig_idle(&r, ST_PWR_IDLE_MS - 1000);
	pressed_at = r.now;
	rig_run(&r, false, true, false, false, ST_PWR_OFF_MS - 1);
	CHECK(r.n_off == 0,
	      "24a. 299 s of idle + a 4.999 s hold: neither fires. The idle "
	      "clock was reset by the press, and the hold is one ms short");

	rig_run(&r, false, true, false, false, 200);
	CHECK(r.n_manual_off > 0 && r.off_at - pressed_at >= ST_PWR_OFF_MS,
	      "24a. ...and at 5.000 s the MANUAL route fires, on time");

	/* (b) A manual hold in progress does not extend the idle interval --
	 *     it resets it, because a finger on FUNCTION is activity. */
	rig_init(&r);
	rig_run(&r, false, true, false, false, 3000);
	CHECK(st_pwr_idle_elapsed_ms(&r.g.idle, rig_mark(&r)) == 0,
	      "24b. a FUNCTION hold pins the idle clock at zero -- it is "
	      "activity, not a suppression");

	/* (c) Transport activity does not touch the manual hold. This is the
	 *     direction that would be catastrophic: playback able to block the
	 *     escape hatch is exactly the st55 failure in new clothes. */
	rig_init(&r);
	pressed_at = r.now;
	rig_run(&r, true, true, false, false, ST_PWR_OFF_MS + 200);
	CHECK(r.n_manual_off > 0 &&
	      r.off_at - pressed_at >= ST_PWR_OFF_MS &&
	      r.off_at - pressed_at < ST_PWR_OFF_MS + PASS_MS * 2,
	      "24c. WITH THE TRANSPORT PLAYING, a clean 5.000 s FUNCTION hold "
	      "still powers off at exactly %d ms (measured %lld) -- playback "
	      "cannot block the escape hatch", ST_PWR_OFF_MS,
	      (long long)(r.off_at - pressed_at));
}

/*
 * THE COMBINED FLAG IS EXACTLY THE OR OF THE TWO ROUTES.
 *
 * rig_run() checks this on every pass of every case above; this makes it an
 * explicit case so a reader can find it, and drives both routes to fire.
 */
static void test_or_is_exactly_the_or(void)
{
	rig_t r;

	/* the idle route alone */
	rig_init(&r);
	rig_idle(&r, ST_PWR_IDLE_MS + 200);
	CHECK(r.n_idle_off > 0 && r.n_manual_off == 0 && r.n_off > 0,
	      "OR. idle alone reaches the platform request");

	/* the manual route alone */
	rig_init(&r);
	rig_run(&r, false, true, false, false, ST_PWR_OFF_MS + 200);
	CHECK(r.n_manual_off > 0 && r.n_idle_off == 0 && r.n_off > 0,
	      "OR. the manual hold alone reaches the platform request");

	CHECK(r.n_or_broken == 0,
	      "OR. power_off_request never disagreed with off_due||idle_off_due");
}

/*
 * IDLE NEVER FIRES WHILE THE DEVICE IS OFF.
 *
 * ADDED BECAUSE A MUTANT SURVIVED. Removing the `hout.device_on` guard from
 * the idle verdict broke nothing: no case idled for a full five minutes with
 * the device off, because the wake tests only sit at the gate for four.
 *
 * In production the standby loop acts only on on_due, so a spurious
 * idle_off_due there is currently inert -- but "currently inert" is how a
 * latent defect is described the day before it is wired up.
 */
static void test_idle_never_fires_while_the_device_is_off(void)
{
	rig_t r;

	rig_init(&r);
	st_pwr_gov_init_off(&r.g);          /* at the wake gate, device OFF */

	rig_idle(&r, ST_PWR_IDLE_MS + 60000);   /* six minutes of nobody there */

	CHECK(r.n_idle_off == 0 && r.n_off == 0,
	      "off. six minutes at the wake gate never raises an idle "
	      "shutdown -- the device is already off");
	CHECK(st_pwr_idle_elapsed_ms(&r.g.idle, rig_mark(&r)) >= ST_PWR_IDLE_MS,
	      "off. ...even though the idle clock itself has passed %d ms",
	      ST_PWR_IDLE_MS);

	/* And once it wakes, a full fresh interval is required. */
	{
		int64_t woke;

		rig_run(&r, false, true, false, false, 2100);
		woke = rig_mark(&r);
		rig_idle_until(&r, woke, ST_PWR_IDLE_MS);
		CHECK(r.n_off == 0,
		      "off. ...and after waking, the six minutes are discarded");
		rig_idle(&r, 400);
		CHECK(r.n_idle_off > 0 && r.off_at - woke >= ST_PWR_IDLE_MS,
		      "off. ...with a full fresh %d ms required", ST_PWR_IDLE_MS);
	}
}

/*
 * WHY THERE IS NO TEST FOR "IDLE FIRES WHILE THE MANUAL PATH IS DISARMED",
 * and this is a limitation of the input space rather than of the suite.
 *
 * Making the idle verdict read off_armed is a mutant that CANNOT be killed by
 * any input, because that state is unreachable:
 *
 *   - off_armed is granted by any pass with FUNCTION up
 *   - FUNCTION down is intentional activity, which pins the idle clock at zero
 *   - so a 300,000 ms idle expiry requires 300,000 ms of FUNCTION being up
 *   - which necessarily armed shutdown on its first pass
 *
 * At the moment idle expires, off_armed is therefore always true. The
 * independence still matters and is still enforced -- gate E-7 asserts
 * structurally that the idle verdict does not mention off_armed -- but it is
 * STRUCTURALLY INSPECTED, not host-proven, and mutants I-9 and I-16 are
 * documented as equivalent rather than claimed as caught.
 *
 * test_23 below proves the reachable half: a wake press held down leaves the
 * manual route disarmed, and the idle route still shuts the device off.
 */

/* ======================================================================
 * The wake handoff (§10): a wake starts the idle system clean
 * ====================================================================== */

static void test_wake_discards_historical_idle(void)
{
	rig_t r;
	int64_t woke_at;

	rig_init(&r);
	st_pwr_gov_init_off(&r.g);

	/* Four minutes of the gate loop running with nobody there. Whatever
	 * this leaves on the idle clock must not survive the wake. */
	rig_idle(&r, 240000);

	/* Now the wake, then release. */
	rig_run(&r, false, true, false, false, 2100);
	woke_at = rig_mark(&r);
	rig_idle_until(&r, woke_at, ST_PWR_IDLE_MS);
	CHECK(r.n_off == 0,
	      "wake. the 4 minutes before the wake are discarded: %d ms after "
	      "coming up, still on", ST_PWR_IDLE_MS - 1);

	rig_idle(&r, 400);
	CHECK(r.n_off > 0 && r.off_at - woke_at >= ST_PWR_IDLE_MS,
	      "wake. ...and a full fresh %d ms is required", ST_PWR_IDLE_MS);
}

static void test_wake_press_held_leaves_no_stale_idle_state(void)
{
	rig_t r;
	int64_t released_at;

	rig_init(&r);
	st_pwr_gov_init_off(&r.g);
	/* Wake, then keep holding for a further 30 s. */
	rig_run(&r, false, true, false, false, 32000);
	CHECK(r.n_off == 0,
	      "wake. a 32 s unbroken wake press does not power off by either "
	      "route");
	CHECK(st_pwr_idle_elapsed_ms(&r.g.idle, rig_mark(&r)) == 0,
	      "wake. ...and leaves the idle clock at zero, not stale");

	released_at = rig_mark(&r);
	rig_idle_until(&r, released_at, ST_PWR_IDLE_MS);
	CHECK(r.n_off == 0, "wake. the idle interval starts at the release");
	rig_idle(&r, 400);
	CHECK(r.n_off > 0 && r.off_at - released_at >= ST_PWR_IDLE_MS,
	      "wake. ...and runs its full length from there");
}

/* Sub-threshold analog noise must not hold the device awake for ever (§8). */
static void test_resting_fader_noise_does_not_prevent_shutdown(void)
{
	rig_t r;
	int64_t start;
	int n;

	rig_init(&r);
	start = rig_mark(&r);
	/* Every fader jittering below the movement threshold, the whole time. */
	for (n = 0; n < 40000 && r.n_off == 0; n++) {
		uint32_t k;

		for (k = 0; k < ST_PWR_FADERS; k++) {
			r.fader_pos[k] = 2000 +
				(int32_t)((n + k) % ST_PWR_FADER_MOVE_COUNTS);
		}
		rig_idle(&r, PASS_MS);
	}
	CHECK(r.n_off > 0 && r.off_at - start >= ST_PWR_IDLE_MS &&
	      r.off_at - start < ST_PWR_IDLE_MS + 1000,
	      "noise. sub-threshold jitter on all four faders does not keep the "
	      "device awake -- off at %lld ms",
	      (long long)(r.off_at - start));
}

int main(void)
{
	printf("idle contract -- %d ms (%d s), pass %d ms, fader move >= %d "
	       "counts (UNMEASURED, M3)\n",
	       ST_PWR_IDLE_MS, ST_PWR_IDLE_MS / 1000, PASS_MS,
	       ST_PWR_FADER_MOVE_COUNTS);

	RUN(test_1_299999ms_stays_on);
	RUN(test_2_300000ms_powers_off);
	RUN(test_2b_the_threshold_to_the_millisecond);
	RUN(test_3_play_resets);
	RUN(test_4_track_resets);
	RUN(test_5_rocker_resets);
	RUN(test_6_volume_resets);
	RUN(test_7_function_resets);
	RUN(test_8_each_fader_resets);
	RUN(test_9_repeated_interaction_never_accumulates);
	RUN(test_10_normal_playback);
	RUN(test_11_seven_minute_song);
	RUN(test_12_loop_playback);
	RUN(test_13_reverse_playback);
	RUN(test_14_slow_playback);
	RUN(test_15_pitched_playback);
	RUN(test_16_fx_over_active_playback);
	RUN(test_16b_stopping_starts_the_interval);
	RUN(test_17_led_activity);
	RUN(test_18_diagnostics);
	RUN(test_19_housekeeping);
	RUN(test_20_21_stale_state_cannot_keep_it_awake);
	RUN(test_22_manual_works_regardless_of_idle_elapsed);
	RUN(test_23_idle_works_while_manual_is_disarmed);
	RUN(test_24_neither_mechanism_can_suppress_the_other);
	RUN(test_or_is_exactly_the_or);
	RUN(test_idle_never_fires_while_the_device_is_off);
	RUN(test_wake_discards_historical_idle);
	RUN(test_wake_press_held_leaves_no_stale_idle_state);
	RUN(test_resting_fader_noise_does_not_prevent_shutdown);

	printf("\n(power_off_request == off_due || idle_off_due held on every "
	       "pass of every case)\n");
	printf("\n%d cases, %d checks, %d failures\n",
	       g_cases, g_checks, g_failures);
	return g_failures ? 1 : 0;
}
