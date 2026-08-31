/*
 * test_pgate.c -- the planar CPU gate as a controlled experiment.
 *
 * Every case here exists because the FIRST version of this gate got it wrong
 * in that specific way and reported a verdict anyway. The failures were:
 * no control window, the post-resume prime counted inside the measurement,
 * cumulative counters that could not separate settling from steady state, and
 * no way to notice that playback never happened. Each has a case below, and
 * each is written so that the old behaviour fails it.
 *
 * Build (from the repo root):
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_pgate.c \
 *      firmware/stemtape_player/tests/test_pgate.c \
 *      -o test_pgate && ./test_pgate
 */

#include <stdio.h>
#include <string.h>

#include "st_latency.h"
#include "st_pgate.h"

static int g_cases, g_checks, g_failures;

#define CHECK(cond, fmt, ...)                                                  \
	do {                                                                   \
		g_checks++;                                                    \
		if (!(cond)) {                                                 \
			g_failures++;                                          \
			printf("  FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,  \
			       ##__VA_ARGS__);                                 \
		}                                                              \
	} while (0)

_Static_assert(ST_PGATE_SECTOR_US == ST_LAT_SECTOR_US,
	       "st_pgate.h and st_latency.h disagree on sector duration");

/* ---- a rig that plays a song at a chosen health ----------------------- */

#define PASS_MS 500u          /* the real diagnostic cadence */

typedef struct {
	st_pgate_t g;
	uint32_t   now;
	uint32_t   sectors;       /* free-running, as in firmware */
	uint32_t   underruns;
	uint32_t   phase_seen[5]; /* passes spent in each phase */
} rig_t;

static void rig_init(rig_t *r)
{
	memset(r, 0, sizeof(*r));
	r->now = 9000u;      /* not zero, so a naive impl gets no free pass */
}

/*
 * Advance `ms` of playback. `und_per_win` underruns are injected per window
 * of ST_PGATE_WINDOW_MS, spread evenly, but ONLY while the given pattern is
 * the one actually in force -- `und_when_level` selects which.
 *
 *   und_when_level = 0  -> a device that struggles on the SHIPPED pattern
 *   und_when_level > 0  -> a device that struggles only when diverging
 */
static void play(rig_t *r, uint32_t ms, bool playing, uint32_t und_per_win,
		  uint32_t und_when_level, uint32_t sectors_per_win)
{
	const uint32_t passes = ms / PASS_MS;
	uint32_t i;

	for (i = 0; i < passes; i++) {
		const uint32_t lvl = st_pgate_level_now(&r->g);
		const bool     hit = und_when_level == 0u ? (lvl == 0u) : (lvl > 0u);
		const uint32_t per = ST_PGATE_WINDOW_MS / PASS_MS;

		r->now += PASS_MS;
		if (playing) {
			r->sectors += sectors_per_win / per;
			if (hit && und_per_win) {
				/* Spread, not bunched: bunching at a window edge
				 * would let an off-by-one in the phase
				 * boundaries hide. */
				if ((i % (per / (und_per_win < per ? und_per_win : per))) == 0u) {
					r->underruns++;
				}
			}
		}
		r->phase_seen[st_pgate_tick(&r->g, r->now, playing, r->sectors,
					     r->underruns, 1500u)]++;
	}
}

/* Sectors a full window needs to exactly keep up. */
static uint32_t need_per_win(void)
{
	return st_pgate_sectors_needed(ST_PGATE_WINDOW_MS);
}

static const char *vname(st_pgate_verdict_t v)
{
	switch (v) {
	case ST_PGATE_VERDICT_PASS:         return "PASS";
	case ST_PGATE_VERDICT_FAIL:         return "FAIL";
	case ST_PGATE_VERDICT_INCONCLUSIVE: return "INCONCLUSIVE";
	default:                            return "none";
	}
}

/* ======================================================================
 * 1. A HEALTHY DEVICE THAT ALSO COPES WITH DIVERGENCE PASSES.
 * ====================================================================== */
static void case_pass(void)
{
	rig_t r;

	g_cases++;
	printf("\n-- clean baseline, clean test -> PASS\n");

	rig_init(&r);
	st_pgate_start(&r.g, 4u, r.now);
	play(&r, 60000u, true, 0u, 0u, need_per_win() + 40u);

	printf("     baseline %u sectors / %u underruns, test %u / %u -> %s\n",
	       r.g.base.sectors, r.g.base.underruns,
	       r.g.test.sectors, r.g.test.underruns, vname(r.g.verdict));
	CHECK(r.g.phase == ST_PGATE_DONE, "the run never completed");
	CHECK(r.g.verdict == ST_PGATE_VERDICT_PASS,
	      "a clean run gave %s", vname(r.g.verdict));
	CHECK(st_pgate_keepup_pct(&r.g.base) >= 100u,
	      "baseline keep-up %u%% -- the rig did not feed it enough",
	      st_pgate_keepup_pct(&r.g.base));
}

/* ======================================================================
 * 2. THE PATTERN'S OWN FAILURE IS A FAIL -- and only then.
 * ====================================================================== */
static void case_fail_is_attributable(void)
{
	rig_t r;

	g_cases++;
	printf("\n-- clean baseline, underrunning test -> FAIL\n");

	rig_init(&r);
	st_pgate_start(&r.g, 4u, r.now);
	/* Underruns ONLY while the divergent pattern is in force. */
	play(&r, 60000u, true, 8u, 4u, need_per_win());

	printf("     baseline %u underruns, test %u underruns -> %s\n",
	       r.g.base.underruns, r.g.test.underruns, vname(r.g.verdict));
	CHECK(r.g.base.underruns == 0u,
	      "the baseline window caught %u underruns it should not have",
	      r.g.base.underruns);
	CHECK(r.g.test.underruns > 0u, "the test window caught none");
	CHECK(r.g.verdict == ST_PGATE_VERDICT_FAIL,
	      "an attributable failure gave %s", vname(r.g.verdict));
}

/* ======================================================================
 * 3. THE OUTCOME THE OLD GATE COULD NOT EXPRESS.
 *
 *    A device that underruns on the SHIPPED pattern says nothing about the
 *    pattern under test. The old gate had no baseline, so this case could
 *    only ever have come out FAIL -- blaming the feature for a problem that
 *    was already there. That is the single most important case in this file.
 * ====================================================================== */
static void case_broken_baseline_is_inconclusive(void)
{
	rig_t r;

	g_cases++;
	printf("\n-- underrunning BASELINE -> INCONCLUSIVE, never FAIL\n");

	rig_init(&r);
	st_pgate_start(&r.g, 4u, r.now);
	/* Underruns while the SHIPPED pattern is in force. */
	play(&r, 60000u, true, 8u, 0u, need_per_win());

	printf("     baseline %u underruns, test %u underruns -> %s\n",
	       r.g.base.underruns, r.g.test.underruns, vname(r.g.verdict));
	CHECK(r.g.base.underruns > 0u, "the rig failed to break the baseline");
	CHECK(r.g.verdict == ST_PGATE_VERDICT_INCONCLUSIVE,
	      "a broken baseline gave %s -- it must never be reported as a "
	      "property of the pattern under test", vname(r.g.verdict));
	CHECK(r.g.verdict != ST_PGATE_VERDICT_FAIL,
	      "a pre-existing problem was blamed on the feature");
}

/* ======================================================================
 * 4. THE WARM-UP IS EXCLUDED.
 *
 *    Leaving transfer mode re-primes the ring, and the old gate counted that
 *    prime. Here the first seconds of playback underrun heavily and the
 *    verdict must still be PASS, because none of it belongs to either window.
 * ====================================================================== */
static void case_settle_excludes_the_prime(void)
{
	rig_t r;
	uint32_t during_settle;

	g_cases++;
	printf("\n-- the post-resume prime is not counted\n");

	rig_init(&r);
	st_pgate_start(&r.g, 4u, r.now);

	/* A rough first two seconds: nothing fetched, underruns piling up. */
	{
		uint32_t i;

		for (i = 0; i < 4u; i++) {
			r.now += PASS_MS;
			r.underruns += 5u;      /* the ring is empty */
			(void)st_pgate_tick(&r.g, r.now, true, r.sectors,
					     r.underruns, 30000u);
		}
	}
	during_settle = r.underruns;
	CHECK(r.g.phase == ST_PGATE_SETTLE,
	      "the gate left SETTLE after only 2 s");

	/* Then a perfectly healthy song for the rest of the run. */
	play(&r, 62000u, true, 0u, 0u, need_per_win() + 40u);

	printf("     %u underruns during settle, %u in baseline, %u in test -> %s\n",
	       during_settle, r.g.base.underruns, r.g.test.underruns,
	       vname(r.g.verdict));
	CHECK(during_settle > 0u, "the rig did not inject a rough warm-up");
	CHECK(r.g.base.underruns == 0u && r.g.test.underruns == 0u,
	      "warm-up underruns leaked into a measurement window");
	CHECK(r.g.verdict == ST_PGATE_VERDICT_PASS,
	      "the prime was blamed on the read pattern (%s)",
	      vname(r.g.verdict));
	/* And the worst fetch seen during settle must not poison a window. */
	CHECK(r.g.base.worst_us < 30000u,
	      "a settle-phase fetch time (%u us) leaked into the baseline",
	      r.g.base.worst_us);
}

/* ======================================================================
 * 5. A TRANSPORT THAT NEVER PLAYS NEVER REACHES A VERDICT.
 *
 *    The old gate would happily report und=0 for a run where nothing ever
 *    played, which is indistinguishable from a pass.
 * ====================================================================== */
static void case_never_playing_never_concludes(void)
{
	rig_t r;

	g_cases++;
	printf("\n-- a run where nothing plays reaches no verdict\n");

	rig_init(&r);
	st_pgate_start(&r.g, 4u, r.now);
	play(&r, 120000u, false, 0u, 0u, need_per_win());

	printf("     after 120 s stopped: phase=%d verdict=%s\n",
	       (int)r.g.phase, vname(r.g.verdict));
	CHECK(r.g.phase == ST_PGATE_SETTLE,
	      "a stopped transport advanced past SETTLE");
	CHECK(r.g.verdict == ST_PGATE_VERDICT_NONE,
	      "a run with no playback produced the verdict %s",
	      vname(r.g.verdict));
}

/* ======================================================================
 * 6. STOPPING MID-RUN RESTARTS, RATHER THAN STITCHING A GAP TOGETHER.
 * ====================================================================== */
static void case_pause_restarts(void)
{
	rig_t r;

	g_cases++;
	printf("\n-- pausing mid-window restarts the experiment\n");

	rig_init(&r);
	st_pgate_start(&r.g, 4u, r.now);
	play(&r, 14000u, true, 0u, 0u, need_per_win() + 40u);
	CHECK(r.g.phase == ST_PGATE_BASELINE, "should be measuring by 14 s");

	play(&r, 3000u, false, 0u, 0u, 0u);          /* stopped */
	CHECK(r.g.phase == ST_PGATE_SETTLE,
	      "a stop mid-baseline did not restart the experiment");

	play(&r, 60000u, true, 0u, 0u, need_per_win() + 40u);
	printf("     after resume: %s, baseline ran %u ms\n",
	       vname(r.g.verdict), r.g.base.ms);
	CHECK(r.g.verdict == ST_PGATE_VERDICT_PASS,
	      "the restarted run gave %s", vname(r.g.verdict));
	CHECK(r.g.base.ms >= ST_PGATE_WINDOW_MS,
	      "the baseline window was only %u ms -- a gap was stitched in",
	      r.g.base.ms);
}

/* ======================================================================
 * 7. THE BASELINE RUNS THE SHIPPED PATTERN, THE TEST RUNS THE OTHER ONE.
 *
 *    If the level were applied during the baseline there would be no control
 *    at all -- which is precisely the flaw being corrected.
 * ====================================================================== */
static void case_level_applies_only_to_the_test(void)
{
	rig_t r;
	uint32_t seen_settle = 0, seen_base = 0, seen_test = 0;
	uint32_t i, passes = 120000u / PASS_MS;

	g_cases++;
	printf("\n-- only the test window diverges\n");

	rig_init(&r);
	st_pgate_start(&r.g, 3u, r.now);
	for (i = 0; i < passes; i++) {
		const st_pgate_phase_t p = r.g.phase;
		const uint32_t lvl = st_pgate_level_now(&r.g);

		if (p == ST_PGATE_SETTLE  && lvl != 0u) seen_settle++;
		if (p == ST_PGATE_BASELINE && lvl != 0u) seen_base++;
		if (p == ST_PGATE_TEST     && lvl == 3u) seen_test++;

		r.now += PASS_MS;
		r.sectors += (need_per_win() + 40u) / (ST_PGATE_WINDOW_MS / PASS_MS);
		(void)st_pgate_tick(&r.g, r.now, true, r.sectors, r.underruns, 1500u);
	}
	printf("     divergent passes: settle=%u baseline=%u, test passes at "
	       "level 3=%u\n", seen_settle, seen_base, seen_test);
	CHECK(seen_settle == 0u, "the gate diverged during SETTLE");
	CHECK(seen_base == 0u,
	      "the gate diverged during the BASELINE -- there is no control");
	CHECK(seen_test > 0u, "the test window never applied the level");
	CHECK(st_pgate_level_now(&r.g) == 0u,
	      "the gate is still diverging after DONE");
}

/* ======================================================================
 * 8. ABORT RETURNS THE SHIPPED PATTERN IMMEDIATELY.
 * ====================================================================== */
static void case_abort(void)
{
	rig_t r;

	g_cases++;
	printf("\n-- abort restores the shipped read pattern at once\n");

	rig_init(&r);
	st_pgate_start(&r.g, 4u, r.now);
	play(&r, 30000u, true, 0u, 0u, need_per_win() + 40u);
	CHECK(r.g.phase == ST_PGATE_TEST, "should be in the test window");
	CHECK(st_pgate_level_now(&r.g) == 4u, "should be diverging");

	st_pgate_abort(&r.g);
	CHECK(st_pgate_level_now(&r.g) == 0u,
	      "abort left the streamer diverging");
	CHECK(r.g.phase == ST_PGATE_IDLE, "abort did not return to idle");

	play(&r, 60000u, true, 0u, 0u, need_per_win() + 40u);
	CHECK(r.g.verdict == ST_PGATE_VERDICT_NONE,
	      "an aborted run still produced the verdict %s",
	      vname(r.g.verdict));
}

/* ======================================================================
 * 9. A VERDICT IS FINAL UNTIL THE GATE IS RE-ARMED.
 *
 *    Found by mutation: removing the DONE guard left every test passing,
 *    because nothing here stopped playback AFTER a verdict. Without the
 *    guard, stopping and restarting the song re-runs the whole experiment and
 *    silently replaces the answer -- so the number on screen would change
 *    without anyone asking it to, which is worse than no number.
 * ====================================================================== */
static void case_verdict_is_final(void)
{
	rig_t r;
	st_pgate_verdict_t first;

	g_cases++;
	printf("\n-- a verdict survives stopping and restarting the song\n");

	rig_init(&r);
	st_pgate_start(&r.g, 4u, r.now);
	play(&r, 60000u, true, 0u, 0u, need_per_win() + 40u);
	first = r.g.verdict;
	CHECK(r.g.phase == ST_PGATE_DONE && first != ST_PGATE_VERDICT_NONE,
	      "the run did not reach a verdict to begin with");

	/* Stop, then play a long and BADLY underrunning song. */
	play(&r, 5000u, false, 0u, 0u, 0u);
	play(&r, 90000u, true, 20u, 0u, need_per_win() / 2u);

	printf("     verdict before %s, after a stop and a bad song %s\n",
	       vname(first), vname(r.g.verdict));
	CHECK(r.g.phase == ST_PGATE_DONE,
	      "the gate left DONE and re-ran itself");
	CHECK(r.g.verdict == first,
	      "the verdict changed from %s to %s without being re-armed",
	      vname(first), vname(r.g.verdict));
	CHECK(st_pgate_level_now(&r.g) == 0u,
	      "the gate diverged again after DONE");
}

int main(void)
{
	printf("== Stem Tape PLANAR CPU GATE (controlled A/B) ==\n");
	printf("settle %u ms, then %u ms baseline and %u ms test; "
	       "%u sectors needed per window\n",
	       ST_PGATE_SETTLE_MS, ST_PGATE_WINDOW_MS, ST_PGATE_WINDOW_MS,
	       st_pgate_sectors_needed(ST_PGATE_WINDOW_MS));

	case_pass();
	case_fail_is_attributable();
	case_broken_baseline_is_inconclusive();
	case_settle_excludes_the_prime();
	case_never_playing_never_concludes();
	case_pause_restarts();
	case_level_applies_only_to_the_test();
	case_abort();
	case_verdict_is_final();

	printf("\n");
	if (g_failures) {
		printf("PGATE TEST FAILED (%d cases, %d checks, %d failures)\n",
		       g_cases, g_checks, g_failures);
		return 1;
	}
	printf("PGATE TEST PASSED (%d cases, %d checks, 0 failures)\n",
	       g_cases, g_checks);
	printf("NOTE: this proves the EXPERIMENT's structure -- control window, "
	       "excluded warm-up, and the INCONCLUSIVE outcome the previous "
	       "gate could not express. What the hardware then says is a "
	       "separate question.\n");
	return 0;
}
