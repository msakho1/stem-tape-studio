/*
 * st_pgate.h -- the planar CPU gate, as a controlled experiment.
 *
 * ======================================================================
 * WHY THIS EXISTS: THE FIRST ATTEMPT WAS NOT AN EXPERIMENT
 * ======================================================================
 * The first version of this gate armed the planar read pattern, handed the
 * device back to playback and counted underruns. It produced 32 and 41
 * underruns at four diverging tracks, which was reported as "the CPU cannot
 * keep up". That claim did not survive an audit, and the failures were all in
 * the METHOD rather than the result:
 *
 *   NO CONTROL. The shipped read pattern was never measured under the same
 *   conditions, so nothing the armed run produced could be attributed to the
 *   pattern rather than to the conditions.
 *
 *   THE WARM-UP WAS COUNTED. Leaving transfer mode sets g_slot_switch_req,
 *   which reloads the song and restarts the read-ahead ring EMPTY. Counters
 *   were cleared before that, so the prime -- 13 sectors in a window needing
 *   ~71 -- was inside the number.
 *
 *   CUMULATIVE COUNTERS. Nothing distinguished "underran while settling" from
 *   "underran throughout".
 *
 *   NO CONFIRMATION OF PLAYBACK. A run where the transport never started
 *   looked the same as a run where it did.
 *
 * So this module runs the thing properly: SETTLE, then a BASELINE window at
 * the shipped read pattern, then a TEST window at the pattern under test, then
 * a verdict comparing the two. One command, one answer, and no dependence on
 * the operator doing things in the right order or on host-side timing.
 *
 * ======================================================================
 * THE THIRD OUTCOME, which the first gate could not express
 * ======================================================================
 * PASS and FAIL are not the only answers. If the BASELINE window itself
 * underruns, the device was already struggling and the test window says
 * nothing about the read pattern. That is INCONCLUSIVE, and reporting it as
 * FAIL -- which the first gate necessarily did, having no baseline -- is how a
 * pre-existing problem gets blamed on the feature under test.
 *
 * ======================================================================
 * PURE
 * ======================================================================
 * No clock, no I/O: the caller supplies now_ms and the free-running counters,
 * and the module returns which read pattern the streamer should be using right
 * now. That is what makes the whole A/B host-testable, including the parts
 * that only happen once every twenty seconds on real hardware.
 */

#ifndef STEMTAPE_PLAYER_PGATE_H_
#define STEMTAPE_PLAYER_PGATE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * How long playback must run before ANY counting starts.
 *
 * Sized against the thing it exists to exclude: leaving transfer mode reloads
 * the song and the read-ahead ring refills from empty. Four seconds is many
 * times the ring's own depth in audio, so the prime is comfortably over before
 * the baseline window opens.
 */
#define ST_PGATE_SETTLE_MS 4000u

/*
 * Each measurement window. Twenty seconds is ~2800 sectors of audio -- long
 * enough that a single unlucky read cannot dominate, and long enough for a
 * slow leak to show, while keeping the whole run (settle + two windows) under
 * a minute so it is actually run rather than skipped.
 */
#define ST_PGATE_WINDOW_MS 20000u

/* Audio one sector holds. Mirrors ST_LAT_SECTOR_US; see st_readcost.h for why
 * this module keeps its own copy, and the test that pins them equal. */
#define ST_PGATE_SECTOR_US 7083u

typedef enum {
	ST_PGATE_IDLE = 0,   /* not running; streamer uses the shipped pattern */
	ST_PGATE_SETTLE,     /* playing, but warming up -- nothing counted */
	ST_PGATE_BASELINE,   /* measuring the SHIPPED pattern */
	ST_PGATE_TEST,       /* measuring the pattern under test */
	ST_PGATE_DONE,       /* verdict ready */
} st_pgate_phase_t;

typedef enum {
	ST_PGATE_VERDICT_NONE = 0,
	ST_PGATE_VERDICT_PASS,          /* baseline clean, test clean */
	ST_PGATE_VERDICT_FAIL,          /* baseline clean, test underran */
	ST_PGATE_VERDICT_INCONCLUSIVE,  /* the BASELINE underran -- see above */
} st_pgate_verdict_t;

typedef struct {
	uint32_t ms;         /* how long the window actually ran */
	uint32_t sectors;    /* sectors fetched during it */
	uint32_t underruns;  /* underrun EPISODES -- see below, not a dropout count */
	/*
	 * FRAMES OF SILENCE, and the only one of these that decides whether a
	 * listener hears anything. `underruns` counts transitions and records
	 * no duration, so one stalled frame and one stalled block both add 1.
	 * A run that sounded perfect once reported 384 of them, which is
	 * anywhere from 8 ms across 20 seconds to 2 whole seconds. The verdict
	 * is taken from THIS field for exactly that reason.
	 */
	uint32_t silence_frames;
	uint32_t worst_us;   /* worst single sector FETCH (all its reads) */
} st_pgate_win_t;

typedef struct {
	st_pgate_phase_t   phase;
	st_pgate_verdict_t verdict;
	uint32_t level;          /* tracks diverging in the TEST window, 1..4 */
	uint32_t phase_ms;       /* when the current phase began */
	uint32_t s0, u0, f0;     /* counter snapshots at phase entry */
	uint32_t worst_us;       /* worst fetch seen in the current phase */
	bool     was_playing;
	st_pgate_win_t base, test;
} st_pgate_t;

/* Arm the experiment for `level` diverging tracks. Nothing is measured until
 * playback has been running for ST_PGATE_SETTLE_MS. */
void st_pgate_start(st_pgate_t *g, uint32_t level, uint32_t now_ms);

/* Abandon a run and return to the shipped pattern. */
void st_pgate_abort(st_pgate_t *g);

/*
 * ONE PASS. Call at the diagnostic cadence with the free-running cumulative
 * counters; the module takes its own deltas, so the caller never resets
 * anything and no other reader is disturbed.
 *
 * `worst_us` is the worst SECTOR FETCH since the last call -- the whole
 * fetch including every read it is made of, which is the quantity that
 * matters to the ring. Naming it precisely here because the first gate
 * compared exactly this number against a SINGLE-read figure and drew a
 * conclusion from the mismatch.
 *
 * PLAYBACK STOPPING RESTARTS THE EXPERIMENT rather than quietly folding a gap
 * into a window: a paused transport fetches nothing and underruns nothing,
 * which would read as a flawless result.
 */
st_pgate_phase_t st_pgate_tick(st_pgate_t *g, uint32_t now_ms, bool playing,
			        uint32_t sectors, uint32_t underruns,
			        uint32_t silence_frames, uint32_t worst_us);

/*
 * WHICH READ PATTERN THE STREAMER MUST USE RIGHT NOW: 0 (the shipped
 * full-sector read) while idle, settling or measuring the baseline; `level`
 * only inside the test window. This is what makes the A/B automatic.
 */
uint32_t st_pgate_level_now(const st_pgate_t *g);

/* Sectors a window of `ms` needs for playback to keep up. */
static inline uint32_t st_pgate_sectors_needed(uint32_t ms)
{
	return (uint32_t)(((uint64_t)ms * 1000u) / ST_PGATE_SECTOR_US);
}

/*
 * Keep-up in percent, AND ITS ASSUMPTION, which is load-bearing: it divides
 * sectors fetched by sectors a window needs AT EXACTLY 1x, FORWARD, NO LOOP.
 * Pitch the tape down, engage slow mode or run a loop and the transport
 * legitimately consumes fewer sectors per real second, so this reads low with
 * nothing wrong -- 63% is simply what it reports at 0.63x. It is a diagnostic
 * for a transport known to be at unity, never evidence of starvation on its
 * own. Silence frames is the number that carries that meaning.
 */
static inline uint32_t st_pgate_keepup_pct(const st_pgate_win_t *w)
{
	const uint32_t need = st_pgate_sectors_needed(w->ms);

	return need ? (uint32_t)(((uint64_t)w->sectors * 100u) / need) : 0u;
}

/* Silence as parts-per-million of the window, at 48 kHz -- the audible
 * quantity, independent of how the episodes were grouped. */
static inline uint32_t st_pgate_silence_ppm(const st_pgate_win_t *w)
{
	if (w->ms == 0u) {
		return 0u;
	}
	return (uint32_t)(((uint64_t)w->silence_frames * 1000000u) /
			   ((uint64_t)w->ms * 48u));
}

#endif /* STEMTAPE_PLAYER_PGATE_H_ */
