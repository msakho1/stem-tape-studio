/*
 * st_pgate.c -- see st_pgate.h for why the first attempt at this gate was not
 * an experiment, and what each phase exists to exclude.
 */

#include "st_pgate.h"

static void enter(st_pgate_t *g, st_pgate_phase_t phase, uint32_t now_ms,
		   uint32_t sectors, uint32_t underruns, uint32_t silence_frames)
{
	g->phase    = phase;
	g->phase_ms = now_ms;
	g->s0       = sectors;
	g->u0       = underruns;
	g->f0       = silence_frames;
	g->worst_us = 0u;
}

static void close_window(st_pgate_t *g, st_pgate_win_t *w, uint32_t now_ms,
			  uint32_t sectors, uint32_t underruns,
			  uint32_t silence_frames)
{
	w->ms             = now_ms - g->phase_ms;
	w->sectors        = sectors - g->s0;
	w->underruns      = underruns - g->u0;
	w->silence_frames = silence_frames - g->f0;
	w->worst_us       = g->worst_us;
}

void st_pgate_start(st_pgate_t *g, uint32_t level, uint32_t now_ms)
{
	const st_pgate_win_t zero = { 0u, 0u, 0u, 0u, 0u };

	g->level       = level;
	g->verdict     = ST_PGATE_VERDICT_NONE;
	g->base        = zero;
	g->test        = zero;
	g->was_playing = false;
	enter(g, ST_PGATE_SETTLE, now_ms, 0u, 0u, 0u);
}

void st_pgate_abort(st_pgate_t *g)
{
	g->phase   = ST_PGATE_IDLE;
	g->verdict = ST_PGATE_VERDICT_NONE;
	g->level   = 0u;
}

uint32_t st_pgate_level_now(const st_pgate_t *g)
{
	/* The ONLY phase that diverges. Settling and the baseline must both run
	 * the shipped pattern, or the "baseline" would not be one. */
	return (g->phase == ST_PGATE_TEST) ? g->level : 0u;
}

st_pgate_phase_t st_pgate_tick(st_pgate_t *g, uint32_t now_ms, bool playing,
			        uint32_t sectors, uint32_t underruns,
			        uint32_t silence_frames, uint32_t worst_us)
{
	if (g->phase == ST_PGATE_IDLE || g->phase == ST_PGATE_DONE) {
		return g->phase;
	}

	/*
	 * A STOPPED TRANSPORT RESTARTS THE EXPERIMENT.
	 *
	 * Not "pauses" it: while stopped nothing is fetched and nothing
	 * underruns, so folding the gap into an open window would make a
	 * paused run look like a flawless one -- and resuming re-primes the
	 * ring anyway, which is exactly what SETTLE exists to skip.
	 */
	if (!playing) {
		enter(g, ST_PGATE_SETTLE, now_ms, sectors, underruns, silence_frames);
		g->was_playing = false;
		return g->phase;
	}
	if (!g->was_playing) {
		/* Playback just started; the settle clock starts HERE, not at
		 * the moment the operator armed the gate. */
		enter(g, ST_PGATE_SETTLE, now_ms, sectors, underruns, silence_frames);
		g->was_playing = true;
		return g->phase;
	}

	if (worst_us > g->worst_us) {
		g->worst_us = worst_us;
	}

	switch (g->phase) {
	case ST_PGATE_SETTLE:
		if (now_ms - g->phase_ms >= ST_PGATE_SETTLE_MS) {
			enter(g, ST_PGATE_BASELINE, now_ms, sectors, underruns, silence_frames);
		}
		break;

	case ST_PGATE_BASELINE:
		if (now_ms - g->phase_ms >= ST_PGATE_WINDOW_MS) {
			close_window(g, &g->base, now_ms, sectors, underruns, silence_frames);
			enter(g, ST_PGATE_TEST, now_ms, sectors, underruns, silence_frames);
		}
		break;

	case ST_PGATE_TEST:
		if (now_ms - g->phase_ms >= ST_PGATE_WINDOW_MS) {
			close_window(g, &g->test, now_ms, sectors, underruns, silence_frames);
			/*
			 * THE BASELINE IS JUDGED FIRST. If the shipped pattern
			 * could not hold up under these conditions, the test
			 * window cannot say anything about the pattern under
			 * test -- and calling that FAIL is how a pre-existing
			 * problem gets blamed on the feature.
			 */
			/*
			 * JUDGED ON SILENCE, NOT EPISODES. An episode count
			 * cannot separate 21 us from 5.3 ms, and a run that
			 * sounded perfect once reported 384 of them. Frames
			 * silenced is what a listener actually hears.
			 */
			if (g->base.silence_frames > 0u) {
				g->verdict = ST_PGATE_VERDICT_INCONCLUSIVE;
			} else if (g->test.silence_frames > 0u) {
				g->verdict = ST_PGATE_VERDICT_FAIL;
			} else {
				g->verdict = ST_PGATE_VERDICT_PASS;
			}
			g->phase = ST_PGATE_DONE;
		}
		break;

	default:
		break;
	}
	return g->phase;
}
