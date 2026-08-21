/*
 * test_fx_ctl.c — the FX control overlay, driving the REAL st_fx_ctl.c.
 *
 * Every case in the directive's proof list is here, plus the ones that decide
 * whether the overlay can corrupt normal mode: a withheld volume press must
 * still reach master volume, and a Track button the overlay owns must never
 * also reach the solo/chord handler.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_fx_ctl.c \
 *      firmware/stemtape_player/tests/test_fx_ctl.c -o test_fx_ctl
 */

#include <stdio.h>
#include <string.h>

#include "st_fx_ctl.h"

static int g_checks, g_failures, g_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { printf("[OK  ] " __VA_ARGS__); printf("\n"); } \
		else { g_failures++; printf("[FAIL] " __VA_ARGS__); \
		       printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
	} while (0)

/* ---------------------------------------------------------------------- *
 * A tiny driver: hold the button picture, step time, service.
 * ---------------------------------------------------------------------- */
typedef struct {
	st_fx_ctl_t  s;
	st_fx_in_t   in;
	st_fx_out_t  out;
	/* Accumulated across a scenario so a one-pass edge cannot be missed. */
	int          opens, closes, target_changes, pairings, ambiguous;
	int          vol_minus_fires, vol_plus_fires;
} drv_t;

static void drv_init(drv_t *d)
{
	memset(d, 0, sizeof(*d));
	st_fx_ctl_reset(&d->s);
}

static void step(drv_t *d, uint32_t dt_ms)
{
	d->in.now_ms += dt_ms;
	st_fx_ctl_service(&d->s, &d->in, &d->out);
	if (d->out.opened) d->opens++;
	if (d->out.closed) d->closes++;
	if (d->out.target_changed) d->target_changes++;
	if (d->out.pairing) d->pairings++;
	if (d->out.ambiguous) d->ambiguous++;
	if (d->out.vol_minus_fire) d->vol_minus_fires++;
	if (d->out.vol_plus_fire) d->vol_plus_fires++;
}

/* Run the arrival window out, one 10 ms scan at a time. */
static void settle(drv_t *d, uint32_t ms)
{
	uint32_t t;

	for (t = 0; t < ms; t += 10u) {
		step(d, 10u);
	}
}

/* ====================================================================== */
static void case_signal_order(void)
{
	g_cases++;
	printf("\n-- DSP order is Filter, Distortion, Gate, Echo -- NOT button order\n");
	CHECK(st_fx_signal_order[0] == ST_FX_FILTER, "1st in the chain is Filter (T1)");
	CHECK(st_fx_signal_order[1] == ST_FX_DIRT,   "2nd is Distortion (T3)");
	CHECK(st_fx_signal_order[2] == ST_FX_GATE,   "3rd is Gate/Stutter (T4)");
	CHECK(st_fx_signal_order[3] == ST_FX_ECHO,
	      "4th is Delay/Echo (T2) -- it repeats the completed processed sound");
}

static void case_enter_minus_first(void)
{
	drv_t d; drv_init(&d);
	g_cases++;
	printf("\n-- Volume- then Volume+ (inside the 120 ms window)\n");

	d.in.vol_minus_down = true;  step(&d, 1);
	CHECK(!d.out.vol_minus_fire, "the lone press is WITHHELD, not dispatched");
	step(&d, 40);
	d.in.vol_plus_down = true;   step(&d, 1);
	CHECK(d.s.chord == ST_FX_CHORD_ARMED, "chord armed at 41 ms arrival");
	d.in.vol_minus_down = false; step(&d, 30);

	CHECK(d.opens == 1, "exactly one open");
	CHECK(d.out.fx_open, "FX mode is open");
	CHECK(d.out.scope == ST_FX_SCOPE_STEM, "STEM scope (no FUNCTION)");
	CHECK(d.vol_minus_fires == 0 && d.vol_plus_fires == 0,
	      "NEITHER volume action was dispatched -- volume cannot change");
}

static void case_enter_plus_first(void)
{
	drv_t d; drv_init(&d);
	g_cases++;
	printf("\n-- Volume+ then Volume- (symmetric)\n");

	d.in.vol_plus_down = true;   step(&d, 1);
	step(&d, 60);
	d.in.vol_minus_down = true;  step(&d, 1);
	d.in.vol_plus_down = false;  step(&d, 20);

	CHECK(d.opens == 1, "exactly one open");
	CHECK(d.vol_minus_fires == 0 && d.vol_plus_fires == 0, "no volume change");
}

static void case_same_scan(void)
{
	drv_t d; drv_init(&d);
	g_cases++;
	printf("\n-- both detected in the SAME scan (arrival 0)\n");

	d.in.vol_minus_down = true;
	d.in.vol_plus_down = true;
	step(&d, 1);
	CHECK(d.s.chord == ST_FX_CHORD_ARMED, "armed immediately");
	d.in.vol_minus_down = false;
	d.in.vol_plus_down = false;
	step(&d, 20);
	CHECK(d.opens == 1, "exactly one open");
	CHECK(d.vol_minus_fires == 0 && d.vol_plus_fires == 0, "no volume change");
}

static void case_single_button(void)
{
	drv_t d;
	g_cases++;
	printf("\n-- either Volume button ALONE still does its ordinary job\n");

	drv_init(&d);
	d.in.vol_minus_down = true;
	settle(&d, 200);
	CHECK(d.vol_minus_fires == 1,
	      "Volume- dispatched exactly once after the window expired");
	CHECK(d.opens == 0, "and no FX toggle");
	d.in.vol_minus_down = false;
	settle(&d, 50);
	CHECK(d.vol_minus_fires == 1, "still exactly once after release");

	drv_init(&d);
	d.in.vol_plus_down = true;
	settle(&d, 200);
	d.in.vol_plus_down = false;
	settle(&d, 50);
	CHECK(d.vol_plus_fires == 1, "Volume+ alone dispatched exactly once");

	/* A short tap that never reaches the window still has to act. */
	drv_init(&d);
	d.in.vol_plus_down = true;  step(&d, 10);
	d.in.vol_plus_down = false; step(&d, 10);
	CHECK(d.vol_plus_fires == 1,
	      "a 10 ms tap dispatches on release -- the window is a ceiling, "
	      "not a required wait");
}

static void case_arrival_too_wide(void)
{
	drv_t d; drv_init(&d);
	g_cases++;
	printf("\n-- second button arrives AFTER 120 ms: two singles, no chord\n");

	d.in.vol_minus_down = true;
	settle(&d, 200);
	CHECK(d.vol_minus_fires == 1, "the first became an ordinary press");
	d.in.vol_plus_down = true;
	step(&d, 10);
	CHECK(d.vol_plus_fires == 1, "the second is its own ordinary press");
	CHECK(d.opens == 0, "no FX toggle at a 200 ms arrival");
}

static void case_held_chord(void)
{
	drv_t d; drv_init(&d);
	g_cases++;
	printf("\n-- holding both buttons emits exactly ONE toggle\n");

	d.in.vol_minus_down = true; step(&d, 1);
	d.in.vol_plus_down = true;  step(&d, 1);
	settle(&d, 400);
	CHECK(d.opens == 0, "nothing fires while both are still held");
	d.in.vol_minus_down = false; step(&d, 1);
	CHECK(d.opens == 1, "the first release resolves it");
	d.in.vol_plus_down = false;  settle(&d, 100);
	CHECK(d.opens == 1, "and the second release adds nothing");
}

static void case_bounce(void)
{
	drv_t d; drv_init(&d);
	int i;
	g_cases++;
	printf("\n-- contact bounce cannot retrigger\n");

	d.in.vol_minus_down = true;
	d.in.vol_plus_down = true;
	step(&d, 1);
	d.in.vol_minus_down = false; step(&d, 1);   /* resolves */
	CHECK(d.opens == 1, "one open");
	/* Now bounce Volume- for a while with Volume+ still down. */
	for (i = 0; i < 12; i++) {
		d.in.vol_minus_down = (i & 1) != 0;
		step(&d, 2);
	}
	CHECK(d.opens == 1 && d.closes == 0,
	      "12 bounce edges with the other button still down produce NOTHING");
	d.in.vol_minus_down = false;
	d.in.vol_plus_down = false;
	settle(&d, 50);
	CHECK(d.opens == 1 && d.closes == 0, "and still nothing after both lift");
}

static void case_staggered_release(void)
{
	drv_t d; drv_init(&d);
	g_cases++;
	printf("\n-- releasing one long before the other\n");

	d.in.vol_minus_down = true;  step(&d, 1);
	d.in.vol_plus_down = true;   step(&d, 1);
	d.in.vol_minus_down = false; step(&d, 1);
	CHECK(d.opens == 1, "resolved on the first release");
	settle(&d, 500);
	d.in.vol_plus_down = false;
	settle(&d, 50);
	CHECK(d.opens == 1 && d.closes == 0, "the late release changes nothing");
	CHECK(d.vol_plus_fires == 0,
	      "and the long-held second button never becomes a volume step");
}

static void case_rapid_toggle(void)
{
	drv_t d; drv_init(&d);
	int i;
	g_cases++;
	printf("\n-- rapid intentional off/on toggles\n");

	for (i = 0; i < 6; i++) {
		d.in.vol_minus_down = true;
		d.in.vol_plus_down = true;
		step(&d, 2);
		d.in.vol_minus_down = false;
		d.in.vol_plus_down = false;
		step(&d, 2);
		step(&d, 20);
	}
	CHECK(d.opens == 3 && d.closes == 3,
	      "6 chords = 3 opens + 3 closes (%d/%d)", d.opens, d.closes);
	CHECK(!d.out.fx_open, "and it ends closed");
	CHECK(d.vol_minus_fires == 0 && d.vol_plus_fires == 0,
	      "with no volume change anywhere");
}

static void case_global_scope(void)
{
	drv_t d; drv_init(&d);
	g_cases++;
	printf("\n-- FUNCTION FIRST + both Volume = GLOBAL scope\n");

	d.in.function_down = true;   step(&d, 20);
	d.in.vol_minus_down = true;  step(&d, 1);
	d.in.vol_plus_down = true;   step(&d, 1);
	d.in.vol_minus_down = false; step(&d, 1);

	CHECK(d.opens == 1, "opened");
	CHECK(d.out.scope == ST_FX_SCOPE_GLOBAL, "in GLOBAL scope");
	CHECK(d.out.function_consumed,
	      "FUNCTION is consumed -- its own tap/hold row must not also fire");
}

static void case_function_after_chord_does_not_convert(void)
{
	drv_t d; drv_init(&d);
	g_cases++;
	printf("\n-- FUNCTION pressed AFTER the chord begins must NOT make it GLOBAL\n");

	d.in.vol_minus_down = true;  step(&d, 1);   /* chord begins, FN is up */
	d.in.function_down = true;   step(&d, 1);   /* FN arrives late */
	d.in.vol_plus_down = true;   step(&d, 1);
	d.in.vol_minus_down = false; step(&d, 1);

	CHECK(d.opens == 1, "opened");
	CHECK(d.out.scope == ST_FX_SCOPE_STEM,
	      "STEM scope: scope is latched at CHORD BEGIN, not at release");
}

static void case_pairing_and_ambiguous(void)
{
	drv_t d;
	g_cases++;
	printf("\n-- the 600-2000 ms no-op band and the 2000 ms pairing gesture\n");

	drv_init(&d);
	d.in.vol_minus_down = true;
	d.in.vol_plus_down = true;
	step(&d, 1);
	settle(&d, 900);
	d.in.vol_minus_down = false;
	step(&d, 1);
	CHECK(d.opens == 0 && d.ambiguous == 1,
	      "900 ms overlap is the diagnostics-only no-op, not a toggle");

	drv_init(&d);
	d.in.vol_minus_down = true;
	d.in.vol_plus_down = true;
	step(&d, 1);
	settle(&d, 2100);
	CHECK(d.pairings == 1, "2100 ms reaches the pairing gesture");
	CHECK(d.opens == 0, "and never toggles FX");
}

static void case_target_walking(void)
{
	drv_t d; drv_init(&d);
	g_cases++;
	printf("\n-- STEM scope: FUNCTION + Volume walks the one rack across stems\n");

	/* open in STEM scope */
	d.in.vol_minus_down = true;
	d.in.vol_plus_down = true;
	step(&d, 1);
	d.in.vol_minus_down = false;
	d.in.vol_plus_down = false;
	settle(&d, 30);
	CHECK(d.out.fx_open && d.out.scope == ST_FX_SCOPE_STEM, "open, STEM");
	CHECK(d.out.target_stem == 0, "starts on stem 0 (Vocal)");

	d.in.function_down = true;
	/* forward three times */
	for (int i = 0; i < 3; i++) {
		d.in.vol_plus_down = true;
		settle(&d, 200);
		d.in.vol_plus_down = false;
		settle(&d, 30);
	}
	CHECK(d.out.target_stem == 3, "three Volume+ presses -> stem 3 (Instrument)");
	CHECK(d.target_changes == 3, "exactly three target changes");
	CHECK(d.vol_plus_fires == 0,
	      "and master volume never moved -- the presses were consumed");

	/* wrap forward */
	d.in.vol_plus_down = true;
	settle(&d, 200);
	d.in.vol_plus_down = false;
	settle(&d, 30);
	CHECK(d.out.target_stem == 0, "wraps 3 -> 0");

	/* wrap backward */
	d.in.vol_minus_down = true;
	settle(&d, 200);
	d.in.vol_minus_down = false;
	settle(&d, 30);
	CHECK(d.out.target_stem == 3, "and 0 -> 3 backwards");
}

static void case_no_target_walk_in_global(void)
{
	drv_t d; drv_init(&d);
	g_cases++;
	printf("\n-- GLOBAL scope has no target: FUNCTION + Volume is not consumed\n");

	d.in.function_down = true;   step(&d, 20);
	d.in.vol_minus_down = true;  step(&d, 1);
	d.in.vol_plus_down = true;   step(&d, 1);
	d.in.vol_minus_down = false;
	d.in.vol_plus_down = false;  settle(&d, 30);
	CHECK(d.out.scope == ST_FX_SCOPE_GLOBAL, "open in GLOBAL");

	d.in.vol_plus_down = true;
	settle(&d, 200);
	CHECK(d.target_changes == 0, "no target walking in GLOBAL scope");
	CHECK(d.vol_plus_fires == 1,
	      "the press falls through to its ordinary behaviour instead of "
	      "moving a stem target that does not exist");
}

static void case_momentary(void)
{
	drv_t d; drv_init(&d);
	g_cases++;
	printf("\n-- momentary: hold a Track, that effect sounds until release\n");

	d.in.vol_minus_down = true;
	d.in.vol_plus_down = true;
	step(&d, 1);
	d.in.vol_minus_down = false;
	d.in.vol_plus_down = false;
	settle(&d, 30);

	d.in.track_down = ST_FX_BIT(ST_FX_FILTER);
	step(&d, 5);
	CHECK(d.out.active_mask == ST_FX_BIT(ST_FX_FILTER), "T1 -> Filter active");
	CHECK((d.out.track_consumed & ST_FX_BIT(ST_FX_FILTER)) != 0,
	      "and the Track button is CONSUMED, so no solo/chord handler sees it");

	/* All four at once, independently. */
	d.in.track_down = 0x0Fu;
	step(&d, 5);
	CHECK(d.out.active_mask == 0x0Fu, "all four held together are all active");

	d.in.track_down = ST_FX_BIT(ST_FX_ECHO) | ST_FX_BIT(ST_FX_GATE);
	step(&d, 5);
	CHECK(d.out.active_mask == (ST_FX_BIT(ST_FX_ECHO) | ST_FX_BIT(ST_FX_GATE)),
	      "releasing two clears only those two");

	d.in.track_down = 0u;
	step(&d, 5);
	CHECK(d.out.active_mask == 0u, "releasing all clears all");
}

static void case_latch(void)
{
	drv_t d; drv_init(&d);
	g_cases++;
	printf("\n-- latch: FUNCTION first, then a Track button\n");

	d.in.vol_minus_down = true;
	d.in.vol_plus_down = true;
	step(&d, 1);
	d.in.vol_minus_down = false;
	d.in.vol_plus_down = false;
	settle(&d, 30);

	d.in.function_down = true;   step(&d, 10);
	d.in.track_down = ST_FX_BIT(ST_FX_DIRT);
	step(&d, 5);
	CHECK(d.out.latch_mask == ST_FX_BIT(ST_FX_DIRT), "FUNCTION + T3 latches Distortion");
	CHECK(d.out.momentary_mask == 0u, "and does NOT also start a momentary");
	d.in.track_down = 0u;        step(&d, 5);
	CHECK(d.out.active_mask == ST_FX_BIT(ST_FX_DIRT),
	      "it keeps sounding after the Track button is released");

	/* Latch a second one; both run. */
	d.in.track_down = ST_FX_BIT(ST_FX_GATE);
	step(&d, 5);
	d.in.track_down = 0u;        step(&d, 5);
	CHECK(d.out.latch_mask == (ST_FX_BIT(ST_FX_DIRT) | ST_FX_BIT(ST_FX_GATE)),
	      "several effects latch simultaneously in the one rack");

	/* A momentary hold of an ALREADY LATCHED effect must not disable it. */
	d.in.function_down = false;
	d.in.track_down = ST_FX_BIT(ST_FX_DIRT);
	step(&d, 5);
	d.in.track_down = 0u;        step(&d, 5);
	CHECK((d.out.latch_mask & ST_FX_BIT(ST_FX_DIRT)) != 0,
	      "holding and releasing a latched effect leaves it latched");

	/* Unlatch. */
	d.in.function_down = true;
	d.in.track_down = ST_FX_BIT(ST_FX_DIRT);
	step(&d, 5);
	d.in.track_down = 0u;        step(&d, 5);
	CHECK(d.out.latch_mask == ST_FX_BIT(ST_FX_GATE),
	      "FUNCTION + T3 again unlatches ONLY Distortion");
}

static void case_close_keeps_latches(void)
{
	drv_t d; drv_init(&d);
	g_cases++;
	printf("\n-- closing the overlay keeps latches and ends momentary\n");

	d.in.vol_minus_down = true;
	d.in.vol_plus_down = true;
	step(&d, 1);
	d.in.vol_minus_down = false;
	d.in.vol_plus_down = false;
	settle(&d, 30);

	d.in.function_down = true;
	d.in.track_down = ST_FX_BIT(ST_FX_ECHO);
	step(&d, 5);
	d.in.track_down = 0u;
	d.in.function_down = false;
	step(&d, 5);
	CHECK(d.out.latch_mask == ST_FX_BIT(ST_FX_ECHO), "Echo latched");

	/* hold a momentary, then close while it is still held */
	d.in.track_down = ST_FX_BIT(ST_FX_FILTER);
	step(&d, 5);
	CHECK(d.out.active_mask == (ST_FX_BIT(ST_FX_ECHO) | ST_FX_BIT(ST_FX_FILTER)),
	      "latched Echo + momentary Filter both active");

	d.in.vol_minus_down = true;
	d.in.vol_plus_down = true;
	step(&d, 1);
	d.in.vol_minus_down = false;
	d.in.vol_plus_down = false;
	settle(&d, 30);
	CHECK(d.closes == 1, "closed");
	CHECK(d.out.latch_mask == ST_FX_BIT(ST_FX_ECHO),
	      "the LATCHED effect survives -- it keeps sounding with the overlay shut");
	CHECK(d.out.momentary_mask == 0u, "the momentary does not");
	CHECK(d.out.track_consumed == 0u,
	      "and with the overlay closed the Track button is released back to "
	      "normal mode");

	/* Reopening restores the latch. */
	d.in.track_down = 0u;
	d.in.vol_minus_down = true;
	d.in.vol_plus_down = true;
	step(&d, 1);
	d.in.vol_minus_down = false;
	d.in.vol_plus_down = false;
	settle(&d, 30);
	CHECK(d.out.latch_mask == ST_FX_BIT(ST_FX_ECHO), "reopening restores it");
}

static void case_boot_default(void)
{
	drv_t d; drv_init(&d);
	g_cases++;
	printf("\n-- boot default\n");
	step(&d, 1);
	CHECK(!d.out.fx_open, "FX mode is CLOSED after reset");
	CHECK(d.out.latch_mask == 0u && d.out.momentary_mask == 0u, "nothing active");
	CHECK(d.out.scope == ST_FX_SCOPE_STEM && d.out.target_stem == 0u,
	      "STEM scope, stem 0");
}

int main(void)
{
	printf("== Stem Tape FX CONTROL OVERLAY ==\n");
	printf("driving the REAL st_fx_ctl.c; chord window %u ms, release %u ms, "
	       "pairing %u ms\n",
	       ST_FX_CHORD_ARRIVAL_MS, ST_FX_CHORD_RELEASE_MS, ST_FX_CHORD_PAIRING_MS);

	case_signal_order();
	case_boot_default();
	case_enter_minus_first();
	case_enter_plus_first();
	case_same_scan();
	case_single_button();
	case_arrival_too_wide();
	case_held_chord();
	case_bounce();
	case_staggered_release();
	case_rapid_toggle();
	case_global_scope();
	case_function_after_chord_does_not_convert();
	case_pairing_and_ambiguous();
	case_target_walking();
	case_no_target_walk_in_global();
	case_momentary();
	case_latch();
	case_close_keeps_latches();

	printf("\n");
	if (g_failures) {
		printf("FX CONTROL FAILED (%d cases, %d checks, %d failures)\n",
		       g_cases, g_checks, g_failures);
		return 1;
	}
	printf("FX CONTROL PASSED (%d cases, %d checks, 0 failures)\n",
	       g_cases, g_checks);
	printf("NOTE: this proves the CONTROL GRAMMAR. It is not audio and not "
	       "physical verification.\n");
	return 0;
}
