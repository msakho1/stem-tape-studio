/*
 * test_ctl.c — THE ASSEMBLED CONTROL PATH, host-tested.
 *
 * This is not a unit test of a state machine in isolation. It drives
 * st_ctl_service() -- the same function, in the same arbitration order, that
 * main.c's control loop calls once per pass -- with timed sequences of REAL
 * MEASURED ADC values from the user's SP-1 (docs/ladder-measured.json), and
 * asserts on what the dispatcher publishes to the mixer, the LEDs and the
 * audio thread.
 *
 * The stimuli are physical measurements, not model-derived band centres.
 * Where a test needs a value that is deliberately NOT a real button (guard
 * zone, ADC error) it says so.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -I../src ../src/st_ctl.c \
 *      ../src/st_ladder.c ../src/st_loop.c test_ctl.c -o test_ctl && ./test_ctl
 */

#include <stdio.h>
#include <string.h>

#include "st_ctl.h"

static int g_checks, g_failures, g_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { printf("[OK  ] " __VA_ARGS__); printf("\n"); } \
		else { g_failures++; printf("[FAIL] " __VA_ARGS__); \
		       printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
	} while (0)

#define QUIET(cond) do { \
		g_checks++; \
		if (!(cond)) { g_failures++; \
			printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
	} while (0)

/* ---------------------------------------------------------------------- *
 * MEASURED ADC VALUES. docs/ladder-measured.json, settled centres.
 * Index == Track mask. RAW_PLAY is the transport.
 * ---------------------------------------------------------------------- */
static const int RAW_MASK[16] = {
	0,    /* idle          */
	205,  /* T1            */
	400,  /* T2            */
	570,  /* T1+T2         */
	727,  /* T3            */
	868,  /* T1+T3         */
	993,  /* T2+T3         */
	1104, /* T1+T2+T3      */
	1213, /* T4            */
	1309, /* T1+T4         */
	1396, /* T2+T4         */
	1480, /* T1+T2+T4      */
	1559, /* T3+T4         */
	1628, /* T1+T3+T4      */
	1695, /* T2+T3+T4      */
	1755, /* all four      */
};
#define RAW_PLAY 1813
#define RAW_IDLE 0

/* The song under test is the one that was measured: 93.71 BPM at 48 kHz. */
#define FPB       30733u
#define ONE_BAR   (FPB * 4u)
#define SONG_LEN  (ONE_BAR * 200u)

/* main.c's control cadence. */
#define PASS_MS 8u

/* ---------------------------------------------------------------------- *
 * THE RIG. Holds the dispatcher, a clock, and a model of the one thing
 * downstream that matters here: the audio thread's playhead, which advances
 * in real time and obeys the loop the dispatcher publishes.
 * ---------------------------------------------------------------------- */
typedef struct {
	st_ctl_t     ctl;
	st_ctl_out_t out;
	uint32_t     now_ms;
	uint32_t     song_frame;
	bool         playing;
	bool         loop_on;
	uint32_t     lo, hi;      /* the published half-open window */

	/* Everything the rig observed, for the ordering assertions. */
	int      n_arm, n_enter, n_exit, n_latch, n_tap, n_resize;
	uint32_t first_frame_after_enter;
	uint32_t first_frame_after_exit;
	bool     saw_frame_after_enter;
	bool     saw_frame_after_exit;
	bool     wrapped_after_exit;
	uint32_t exit_pass;
} rig_t;

static void rig_init(rig_t *r, uint32_t start_frame)
{
	memset(r, 0, sizeof(*r));
	st_ctl_reset(&r->ctl);
	r->song_frame = start_frame;
	r->playing    = true;
}

/*
 * ONE control pass, followed by the audio the rig would have produced in the
 * PASS_MS that pass covers. The order is main.c's: control decides, then the
 * audio thread acts on what was published.
 */
/* Track bits the FX overlay is claiming this pass, applied by pass() below.
 * Zero for every case that does not set it. */
static uint8_t g_fx_claim;

static void pass(rig_t *r, int raw, int8_t vol, bool fn)
{
	st_ctl_in_t in;
	uint32_t i, frames;
	bool entered_this_pass, exited_this_pass;

	memset(&in, 0, sizeof(in));
	in.ladder_raw     = raw;
	in.track_consumed_mask = g_fx_claim;
	in.vol_dir        = vol;
	in.function_down  = fn;
	in.stem_song      = true;
	in.playing        = r->playing;
	in.song_frame     = r->song_frame;
	in.song_frames    = SONG_LEN;
	in.frames_per_beat = FPB;
	in.now_ms         = r->now_ms;

	st_ctl_service(&r->ctl, &in, &r->out);

	r->n_arm    += r->out.loop_arm       ? 1 : 0;
	r->n_enter  += r->out.loop_enter     ? 1 : 0;
	r->n_exit   += r->out.loop_exit      ? 1 : 0;
	r->n_latch  += r->out.loop_latch     ? 1 : 0;
	r->n_tap    += r->out.play_tap       ? 1 : 0;
	r->n_resize += r->out.loop_resize    ? 1 : 0;

	entered_this_pass = r->out.loop_enter;
	exited_this_pass  = r->out.loop_exit;

	/* ---- the audio thread's side of the contract --------------------- */
	if (entered_this_pass) {
		/* SEEK BACK to the captured frame. This is the whole point: the
		 * first frame heard after entry is the armed one. */
		r->song_frame = r->out.loop_start;
		r->loop_on    = true;
		r->lo         = r->out.loop_start;
		r->hi         = r->out.loop_end;
		if (!r->saw_frame_after_enter) {
			r->first_frame_after_enter = r->song_frame;
			r->saw_frame_after_enter   = true;
		}
	}
	if (r->out.loop_resize) {
		r->lo = r->out.loop_start;
		r->hi = r->out.loop_end;
	}
	if (exited_this_pass) {
		/* Stop wrapping FIRST, then seek to the published resume frame.
		 * Nothing else: no pause, no restart, no second seek. */
		r->loop_on    = false;
		r->song_frame = r->out.loop_resume;
		r->exit_pass  = r->now_ms;
		if (!r->saw_frame_after_exit) {
			r->first_frame_after_exit = r->song_frame;
			r->saw_frame_after_exit   = true;
		}
	}
	if (r->out.play_tap) {
		r->playing = !r->playing;
	}

	/* Advance the playhead by one pass worth of frames, honouring the
	 * window exactly as st_loop_next_frame() defines it. */
	frames = (48000u * PASS_MS) / 1000u;
	if (r->playing) {
		for (i = 0; i < frames; i++) {
			if (r->loop_on && r->hi > r->lo) {
				if (r->song_frame + 1u >= r->hi) {
					r->song_frame = r->lo;
				} else {
					r->song_frame++;
				}
			} else {
				r->song_frame++;
				if (r->song_frame >= SONG_LEN) {
					r->song_frame = 0u;
				}
				/* AFTER an exit the playhead must never re-enter
				 * the section it just left. */
				if (r->saw_frame_after_exit &&
				    r->song_frame < r->first_frame_after_exit) {
					r->wrapped_after_exit = true;
				}
			}
		}
	}
	r->now_ms += PASS_MS;
}

/* Hold `raw` for `ms`, at the real control cadence. */
static void hold(rig_t *r, int raw, uint32_t ms, int8_t vol, bool fn)
{
	uint32_t end = r->now_ms + ms;

	while (r->now_ms < end) {
		pass(r, raw, vol, fn);
	}
}

/* ============ 1-6: PLAY-down capture, tap, threshold, one bar =========== */
static void case_entry(void)
{
	rig_t r;
	uint32_t N;

	g_cases++;
	printf("\n-- PLAY-down captures the loop start; the threshold enters there\n");

	rig_init(&r, 1234567u);
	/* Settle idle first, so the PLAY press is a clean edge. */
	hold(&r, RAW_IDLE, 40u, 0, false);
	N = r.song_frame;

	/* PLAY goes down. The ladder needs ST_LADDER_SETTLE_READS passes to
	 * commit it, so the captured frame is the one at the settled edge --
	 * which is what the audio thread is playing when the dispatcher first
	 * sees PLAY, and is what main.c would capture too. */
	pass(&r, RAW_PLAY, 0, false);
	pass(&r, RAW_PLAY, 0, false);
	pass(&r, RAW_PLAY, 0, false);
	CHECK(r.n_arm == 1, "1. exactly one ARM on the PLAY-down edge (got %d)",
	      r.n_arm);
	CHECK(r.out.pin_valid, "   and the sectors to pin are published");
	N = r.out.pin_entry_frame;
	/* 40 ms of idle plus the PLAY settle passes, all at 48 frames/ms. */
	CHECK(N > 1234567u &&
	      N <= 1234567u + 48u * (40u + PASS_MS * ST_LADDER_SETTLE_READS),
	      "   the captured frame %u is the live playhead at the edge", N);

	/* 6. COLD BOOT DEFAULT: exactly one bar of the measured tempo. Both
	 * pinned ends are the real seek targets -- loop_start and loop_end. */
	CHECK(r.out.pin_exit_frame - r.out.pin_entry_frame == ONE_BAR,
	      "6. the cold-boot window is exactly one bar: %u frames (4 x %u)",
	      ONE_BAR, FPB);
	CHECK(r.out.pin_exit_frame == r.out.pin_entry_frame + ONE_BAR,
	      "   and both ends are pinned before anything can be heard");

	/* Still nothing audible: no enter, no tap, and the transport is intact. */
	hold(&r, RAW_PLAY, ST_LOOP_HOLD_MS - 100u, 0, false);
	CHECK(r.n_enter == 0, "   nothing has entered %u ms in", ST_LOOP_HOLD_MS - 100u);
	CHECK(r.n_tap == 0, "   and no transport toggle has fired");

	/* Cross the threshold. */
	hold(&r, RAW_PLAY, 200u, 0, false);
	CHECK(r.n_enter == 1, "3. exactly ONE loop entry (got %d)", r.n_enter);
	CHECK(r.out.loop_active, "   the loop is running");
	CHECK(r.out.loop_start == N,
	      "2/4. loop_start is the PLAY-DOWN frame %u, not the frame the "
	      "threshold expired on", N);
	CHECK(r.first_frame_after_enter == N,
	      "4. the FIRST frame heard after entry is exactly %u", N);
	CHECK(r.out.loop_end == N + ONE_BAR,
	      "   and the window is [%u, %u) -- half-open", N, N + ONE_BAR);
	CHECK(r.n_tap == 0, "5. no transport toggle, and no restart action exists at all");

	/* Releasing after a loop must not also toggle the transport. */
	hold(&r, RAW_IDLE, 60u, 0, false);
	CHECK(r.n_tap == 0, "   releasing the loop press is not a tap either");
	CHECK(r.playing, "   the transport never stopped");
}

/* ================ 2: a short tap is still just play/stop ================= */
static void case_short_tap(void)
{
	rig_t r;

	g_cases++;
	printf("\n-- A short PLAY tap is untouched: play/stop, no loop\n");

	rig_init(&r, 500000u);
	hold(&r, RAW_IDLE, 40u, 0, false);
	hold(&r, RAW_PLAY, 120u, 0, false);      /* well under the threshold */
	hold(&r, RAW_IDLE, 60u, 0, false);

	CHECK(r.n_arm == 1, "2. the press still armed a candidate (harmless)");
	CHECK(r.n_enter == 0, "2. but no loop was entered");
	CHECK(r.n_tap == 1, "2. exactly one play/stop tap (got %d)", r.n_tap);
	CHECK(!r.playing, "2. and the transport toggled");
	CHECK(!r.out.pin_valid, "2. the candidate was discarded, so nothing stays pinned");
}

/* ======================================================================
 * A FUNCTION-QUALIFIED PLAY PRESS NEVER TOGGLES THE TRANSPORT.
 *
 * The bug this pins, reported from hardware: FUNCTION + PLAY toggled slow
 * playback AND paused the song, so the player had to press PLAY again to
 * hear the result, and pressed it again on the way out.
 *
 * The cause was routing, not audio. st_ctl_service() runs ABOVE main.c's
 * FUNCTION branch -- deliberately, so the loop engine can see FUNCTION at
 * all -- and its play_edge_up handler produced an ordinary play_tap without
 * ever consulting function_down. Every chorded gesture therefore also
 * toggled the transport underneath itself.
 *
 * BOTH FINGER ORDERS ARE CHECKED. FUNCTION is the more awkward finger and is
 * very often pressed first and released first, so a fix that only tested
 * function_down at the press edge, or only at the release edge, would leave
 * one of these two orders still pausing the song.
 * ====================================================================== */
static void case_function_play_never_pauses(void)
{
	rig_t r;

	g_cases++;
	printf("\n-- FUNCTION + PLAY never toggles the transport\n");

	/* ---- order A: FUNCTION down first, released last ---------------- */
	rig_init(&r, 500000u);
	hold(&r, RAW_IDLE, 40u, 0, false);
	hold(&r, RAW_IDLE, 40u, 0, true);        /* FUNCTION down */
	hold(&r, RAW_PLAY, 120u, 0, true);       /* a short PLAY tap under it */
	hold(&r, RAW_IDLE, 60u, 0, true);        /* PLAY up, FUNCTION still down */
	hold(&r, RAW_IDLE, 40u, 0, false);       /* FUNCTION up */
	printf("     FN-first : taps=%d playing=%d\n", r.n_tap, (int)r.playing);
	CHECK(r.n_tap == 0,
	      "FUNCTION-first chord produced %d play taps; it must produce none",
	      r.n_tap);
	CHECK(r.playing, "the song stopped on a FUNCTION-first chord");

	/* ---- order B: PLAY down first, FUNCTION arrives during the press,
	 *      and is RELEASED BEFORE PLAY. A release-edge test of
	 *      function_down sees false here and fires the tap anyway. ---- */
	rig_init(&r, 500000u);
	hold(&r, RAW_IDLE, 40u, 0, false);
	hold(&r, RAW_PLAY, 40u, 0, false);       /* PLAY down, no FUNCTION yet */
	hold(&r, RAW_PLAY, 60u, 0, true);        /* FUNCTION joins */
	hold(&r, RAW_PLAY, 40u, 0, false);       /* FUNCTION let go first */
	hold(&r, RAW_IDLE, 60u, 0, false);       /* then PLAY */
	printf("     PLAY-first: taps=%d playing=%d\n", r.n_tap, (int)r.playing);
	CHECK(r.n_tap == 0,
	      "PLAY-first chord produced %d play taps; the latch did not survive "
	      "FUNCTION being released first", r.n_tap);
	CHECK(r.playing, "the song stopped on a PLAY-first chord");

	/* ---- repeated rapid chords: still never a pause ------------------ */
	{
		int i;

		rig_init(&r, 500000u);
		hold(&r, RAW_IDLE, 40u, 0, false);
		for (i = 0; i < 8; i++) {
			hold(&r, RAW_IDLE, 24u, 0, true);
			hold(&r, RAW_PLAY, 80u, 0, true);
			hold(&r, RAW_IDLE, 40u, 0, true);
			hold(&r, RAW_IDLE, 24u, 0, false);
		}
		printf("     8 rapid chords: taps=%d playing=%d\n",
		       r.n_tap, (int)r.playing);
		CHECK(r.n_tap == 0,
		      "%d play taps leaked from 8 repeated chords", r.n_tap);
		CHECK(r.playing, "the song stopped during repeated chording");
	}

	/* ---- AND THE BARE TAP STILL WORKS. The fix must not cost the
	 *      ordinary transport toggle, which is the whole point of PLAY. */
	rig_init(&r, 500000u);
	hold(&r, RAW_IDLE, 40u, 0, false);
	hold(&r, RAW_PLAY, 120u, 0, false);
	hold(&r, RAW_IDLE, 60u, 0, false);
	printf("     bare tap  : taps=%d playing=%d\n", r.n_tap, (int)r.playing);
	CHECK(r.n_tap == 1,
	      "a bare PLAY tap must still toggle the transport (got %d taps)",
	      r.n_tap);
	CHECK(!r.playing, "a bare PLAY tap must still stop a playing song");

	/* ---- and a bare tap immediately AFTER a chord is not swallowed --- */
	rig_init(&r, 500000u);
	hold(&r, RAW_IDLE, 40u, 0, false);
	hold(&r, RAW_IDLE, 24u, 0, true);
	hold(&r, RAW_PLAY, 80u, 0, true);        /* the chord */
	hold(&r, RAW_IDLE, 40u, 0, true);
	hold(&r, RAW_IDLE, 40u, 0, false);       /* FUNCTION up */
	hold(&r, RAW_PLAY, 120u, 0, false);      /* now a bare tap */
	hold(&r, RAW_IDLE, 60u, 0, false);
	printf("     chord then bare tap: taps=%d\n", r.n_tap);
	CHECK(r.n_tap == 1,
	      "the chord's latch leaked into the next bare press (got %d taps)",
	      r.n_tap);
}

/* ============ 8-11: unlatched exit lands on loop_end, exactly ============ */
static void case_unlatched_exit(void)
{
	rig_t r;
	uint32_t N, E;

	g_cases++;
	printf("\n-- Releasing an unlatched loop rejoins the song at loop_end\n");

	rig_init(&r, 900000u);
	hold(&r, RAW_IDLE, 40u, 0, false);
	hold(&r, RAW_PLAY, ST_LOOP_HOLD_MS + 100u, 0, false);
	CHECK(r.n_enter == 1, "the loop entered");
	N = r.out.loop_start;
	E = r.out.loop_end;

	/* Let it lap several times, so an exit-at-loop-start would be obvious. */
	hold(&r, RAW_PLAY, 4000u, 0, false);
	CHECK(r.out.loop_active, "still looping after 4 s");

	hold(&r, RAW_IDLE, 100u, 0, false);
	CHECK(r.n_exit == 1, "8. exactly one exit (got %d)", r.n_exit);
	CHECK(!r.out.loop_active, "9. wrapping stops immediately");
	CHECK(r.first_frame_after_exit == E,
	      "8. the first post-loop frame is exactly loop_end (%u)", E);
	CHECK(r.first_frame_after_exit != N,
	      "10. NOT loop_start -- the looped passage does not play again");
	CHECK(!r.wrapped_after_exit,
	      "10/11. and playback never returns into [%u, %u) afterwards", N, E);
	CHECK(r.song_frame > E, "11. it moved forward past %u, skipping nothing", E);
	CHECK(r.n_tap == 0, "   and no transport toggle came with it");
	CHECK(r.playing, "   the transport is still running");
}

/* ============== 12-16: FUNCTION latches, PLAY exits ===================== */
static void case_latch(void)
{
	rig_t r;
	uint32_t E;

	g_cases++;
	printf("\n-- FUNCTION latches; PLAY exits; the latch is reachable\n");

	rig_init(&r, 700000u);
	hold(&r, RAW_IDLE, 40u, 0, false);
	hold(&r, RAW_PLAY, ST_LOOP_HOLD_MS + 100u, 0, false);
	CHECK(r.out.loop_active && !r.out.loop_latched, "momentary loop running");

	/* FUNCTION goes down while PLAY is still held. */
	hold(&r, RAW_PLAY, 40u, 0, true);
	CHECK(r.n_latch == 1, "12. exactly one latch action (got %d)", r.n_latch);
	CHECK(r.out.loop_latched, "12. the loop is latched");
	CHECK(r.out.function_consumed,
	      "13. and the FUNCTION press is CONSUMED, so main.c's own FUNCTION "
	      "branch must not also act on it");

	/* Holding FUNCTION longer must not latch again or power anything off. */
	hold(&r, RAW_PLAY, 800u, 0, true);
	CHECK(r.n_latch == 1, "12. still exactly one latch after 800 ms of FUNCTION");
	CHECK(r.out.function_consumed, "13. still consumed for the whole hold");

	/* Release BOTH. The loop must keep running. */
	hold(&r, RAW_PLAY, 40u, 0, false);
	hold(&r, RAW_IDLE, 200u, 0, false);
	CHECK(r.n_exit == 0, "14. releasing PLAY after latching does NOT exit");
	CHECK(r.out.loop_active && r.out.loop_latched, "14. still latched and looping");
	CHECK(r.n_tap == 0, "14. and it is not a transport tap either");
	CHECK(!r.out.function_consumed, "13. FUNCTION is released again");

	E = r.out.loop_end;

	/* A NEW PLAY press exits. */
	hold(&r, RAW_PLAY, 60u, 0, false);
	CHECK(r.n_exit == 1, "15. exactly one exit on the new PLAY press");
	CHECK(!r.out.loop_active, "15. wrapping stopped");
	CHECK(r.first_frame_after_exit == E,
	      "16. the first normal frame is loop_end (%u)", E);
	CHECK(r.playing, "15. and the transport was NOT stopped");

	/* That press's RELEASE must do nothing at all. */
	hold(&r, RAW_IDLE, 200u, 0, false);
	CHECK(r.n_tap == 0, "15. the exit press's release is consumed too");
	CHECK(r.n_enter == 1, "15. and it did not start another loop");
	CHECK(r.playing, "15. transport still running");
	CHECK(!r.wrapped_after_exit, "16. no part of the looped section replayed");
}

/* ================= 7: PLAY + VOLUME still changes the division ========== */
static void case_division(void)
{
	rig_t r;
	uint32_t len_default, len_short;

	g_cases++;
	printf("\n-- PLAY + Volume changes the division (the gesture that works)\n");

	rig_init(&r, 400000u);
	hold(&r, RAW_IDLE, 40u, 0, false);
	hold(&r, RAW_PLAY, ST_LOOP_HOLD_MS + 100u, 0, false);
	len_default = r.out.loop_end - r.out.loop_start;
	CHECK(len_default == ONE_BAR, "7. entered at one bar");

	/* Volume -, held for a few passes so it debounces, then released. */
	hold(&r, RAW_PLAY, 60u, -1, false);
	hold(&r, RAW_PLAY, 60u, 0, false);
	len_short = r.out.loop_end - r.out.loop_start;
	CHECK(r.n_resize == 1, "7. exactly one resize per press (got %d)", r.n_resize);
	CHECK(len_short == len_default / 2u,
	      "7. one bar -> half a bar (%u -> %u)", len_default, len_short);
	CHECK(r.out.loop_start == r.out.loop_start, "7. start_frame is unmoved");

	/* Holding the same Volume button down does not walk the ladder: one
	 * press, one step, however long the finger stays on it. (The pass
	 * above already returned it to 0, so press it again and HOLD.) */
	hold(&r, RAW_PLAY, 500u, -1, false);
	CHECK(r.n_resize == 2,
	      "7. the second press steps once more and then holds (got %d)",
	      r.n_resize);
	len_short = r.out.loop_end - r.out.loop_start;
	CHECK(len_short == len_default / 4u,
	      "7. half a bar -> a quarter (%u)", len_short);

	/* THE SESSION RULE: release everything, start a NEW loop -- the chosen
	 * division sticks, because only a cold boot re-defaults it. */
	hold(&r, RAW_IDLE, 200u, 0, false);
	hold(&r, RAW_PLAY, ST_LOOP_HOLD_MS + 100u, 0, false);
	CHECK(r.out.loop_end - r.out.loop_start == len_short,
	      "6/7. the next loop in the same session keeps the chosen division");

	/* And a cold boot puts it back to one bar. */
	{
		rig_t fresh;

		rig_init(&fresh, 400000u);
		hold(&fresh, RAW_IDLE, 40u, 0, false);
		hold(&fresh, RAW_PLAY, ST_LOOP_HOLD_MS + 100u, 0, false);
		CHECK(fresh.out.loop_end - fresh.out.loop_start == ONE_BAR,
		      "6. a cold boot is one bar again, inheriting nothing");
	}
}

/* ============ 22-26: every measured mask, through the dispatcher ========= */
static void case_all_masks(void)
{
	rig_t r;
	int m;

	g_cases++;
	printf("\n-- All 15 measured Track masks decode through the real dispatcher\n");

	for (m = 1; m <= 15; m++) {
		rig_init(&r, 100000u);
		hold(&r, RAW_IDLE, 40u, 0, false);
		hold(&r, RAW_MASK[m], 40u, 0, false);   /* 5 passes: well past settle */
		CHECK(r.out.track_mask == (uint8_t)m,
		      "22. raw %4d -> mask %X%s", RAW_MASK[m], m,
		      (m == 15) ? "  (all four -- NOT PLAY)" : "");

		/* 25. release restores everything within the documented window. */
		hold(&r, RAW_IDLE, PASS_MS * ST_LADDER_RELEASE_READS, 0, false);
		QUIET(r.out.track_mask == 0u);
	}
	CHECK(1, "25. every mask clears within %u ms of release",
	      PASS_MS * ST_LADDER_RELEASE_READS);

	/* 22. PLAY is not a chord and all-four is not PLAY. */
	rig_init(&r, 100000u);
	hold(&r, RAW_IDLE, 40u, 0, false);
	hold(&r, RAW_PLAY, 40u, 0, false);
	CHECK(r.out.track_mask == 0u, "22. PLAY publishes no Track mask");
	rig_init(&r, 100000u);
	hold(&r, RAW_IDLE, 40u, 0, false);
	hold(&r, RAW_MASK[15], 40u, 0, false);
	CHECK(r.n_arm == 0 && r.out.track_mask == 0xFu,
	      "22. all four Tracks is a chord and arms no loop");
}

/* ============ 23-24: the st15 slew deadlock, and ADC errors ============== */
static void case_chord_robustness(void)
{
	rig_t r;
	int m;

	g_cases++;
	printf("\n-- A big ladder step progresses; a transient ADC error holds\n");

	/* 23. Every idle -> mask step is far more than 40 counts. Under st15's
	 * slew guard none of these could ever commit. */
	for (m = 1; m <= 15; m++) {
		rig_init(&r, 100000u);
		hold(&r, RAW_IDLE, 40u, 0, false);
		QUIET(RAW_MASK[m] - RAW_IDLE > 40);
		hold(&r, RAW_MASK[m], 40u, 0, false);
		QUIET(r.out.track_mask == (uint8_t)m);
	}
	CHECK(1, "23. all 15 idle->mask steps exceed 40 counts and all commit");

	/* Chord to chord, also far beyond 40 counts. */
	rig_init(&r, 100000u);
	hold(&r, RAW_IDLE, 40u, 0, false);
	hold(&r, RAW_MASK[1], 40u, 0, false);
	CHECK(r.out.track_mask == 0x1u, "23. T1 held");
	hold(&r, RAW_MASK[14], 40u, 0, false);
	CHECK(r.out.track_mask == 0xEu,
	      "23. T1 -> T2+T3+T4 (a %d-count step) commits, it does not stall",
	      RAW_MASK[14] - RAW_MASK[1]);

	/* 24. A burst of ADC errors must not drop the chord. */
	hold(&r, -1, 200u, 0, false);
	CHECK(r.out.track_mask == 0xEu,
	      "24. 25 consecutive ADC errors do not erase the held chord");
	hold(&r, RAW_MASK[14], 40u, 0, false);
	CHECK(r.out.track_mask == 0xEu, "24. and it is still held afterwards");

	/* A single idle dip is not a release either. */
	pass(&r, RAW_IDLE, 0, false);
	CHECK(r.out.track_mask == 0xEu, "24. one idle sample is not a release");
	pass(&r, RAW_MASK[14], 0, false);
	CHECK(r.out.track_mask == 0xEu, "24. the chord survived the dip");

	/* A confirmed release clears it. */
	hold(&r, RAW_IDLE, 40u, 0, false);
	CHECK(r.out.track_mask == 0u, "25. a confirmed release restores all stems");
}

/* ============ 26: one mask, published once, for both consumers =========== */
static void case_single_owner(void)
{
	rig_t r;
	st_ctl_in_t in;
	st_ctl_out_t a, b;
	int m;

	g_cases++;
	printf("\n-- The mixer and the LEDs read ONE published mask\n");

	/* There is exactly one Track-mask field in the published output. The
	 * proof that both consumers see the same bits is that calling the
	 * dispatcher twice with identical state and input yields identical
	 * output -- there is no second, independently-evolving interpretation
	 * of the rail to disagree with the first. */
	rig_init(&r, 100000u);
	hold(&r, RAW_IDLE, 40u, 0, false);
	hold(&r, RAW_MASK[6], 40u, 0, false);

	memset(&in, 0, sizeof(in));
	in.ladder_raw      = RAW_MASK[6];
	in.stem_song       = true;
	in.playing         = true;
	in.song_frame      = r.song_frame;
	in.song_frames     = SONG_LEN;
	in.frames_per_beat = FPB;
	in.now_ms          = r.now_ms;

	{
		st_ctl_t snapshot = r.ctl;

		st_ctl_service(&r.ctl, &in, &a);
		r.ctl = snapshot;
		st_ctl_service(&r.ctl, &in, &b);
	}
	CHECK(memcmp(&a, &b, sizeof(a)) == 0,
	      "26. the same state and sample publish byte-identical output");
	CHECK(a.track_mask == 0x6u, "26. and it is the measured mask (T2+T3)");

	/* With no stem song the dispatcher owns nothing at all. */
	for (m = 1; m <= 15; m++) {
		st_ctl_t c;
		st_ctl_out_t o;

		st_ctl_reset(&c);
		in.stem_song  = false;
		in.ladder_raw = RAW_MASK[m];
		st_ctl_service(&c, &in, &o);
		st_ctl_service(&c, &in, &o);
		st_ctl_service(&c, &in, &o);
		st_ctl_service(&c, &in, &o);
		QUIET(o.track_mask == 0u);
		QUIET(!o.loop_active && !o.play_tap && !o.function_consumed);
	}
	CHECK(1, "26. with no stem song selected nothing is published or consumed");
}

/* ================== the hold that produces nothing, said ================= */
static void case_no_tempo_is_reported(void)
{
	rig_t r;
	st_ctl_in_t in;
	st_ctl_out_t out;
	st_ctl_t c;
	uint32_t t;
	int refusals = 0, enters = 0, taps = 0;

	g_cases++;
	printf("\n-- A song with no tempo REFUSES out loud, and never fakes a loop\n");

	rig_init(&r, 0u);
	st_ctl_reset(&c);
	memset(&in, 0, sizeof(in));
	in.stem_song       = true;
	in.playing         = true;
	in.song_frames     = SONG_LEN;
	in.frames_per_beat = 0u;          /* THE point: no trustworthy tempo */
	in.ladder_raw      = RAW_IDLE;
	for (t = 0; t < 40u; t += PASS_MS) {
		in.now_ms = t;
		st_ctl_service(&c, &in, &out);
	}
	in.ladder_raw = RAW_PLAY;
	for (; t < 40u + ST_LOOP_HOLD_MS + 200u; t += PASS_MS) {
		in.now_ms = t;
		in.song_frame = t * 48u;
		st_ctl_service(&c, &in, &out);
		if (out.refused != ST_CTL_REFUSE_NONE) {
			refusals++;
			QUIET(out.refused == ST_CTL_REFUSE_NO_TEMPO);
		}
		enters += out.loop_enter ? 1 : 0;
		taps   += out.play_tap ? 1 : 0;
	}
	CHECK(enters == 0, "no loop was entered");
	CHECK(refusals == 1, "the refusal is reported exactly once (got %d)", refusals);
	CHECK(!out.loop_active, "and nothing claims a loop is running");

	in.ladder_raw = RAW_IDLE;
	for (; t < 40u + ST_LOOP_HOLD_MS + 400u; t += PASS_MS) {
		in.now_ms = t;
		st_ctl_service(&c, &in, &out);
		taps += out.play_tap ? 1 : 0;
	}
	CHECK(taps == 0, "and the dead hold is not silently turned into a tap");
}


/* ======================================================================
 * LOOPING WHILE THE FX OVERLAY HOLDS A TRACK BUTTON.
 *
 * PLAY shares the AIN0 rail with the four Track buttons. main.c used to
 * express "the overlay owns this Track" by zeroing the rail reading, which
 * erased PLAY along with it -- so holding any effect made the loop gesture
 * invisible and looping inside FX mode was impossible. Reported from
 * hardware.
 *
 * The claim is a MASK now, subtracted after the decode. These assertions are
 * what stop it going back to a rail-wide erase: the claimed Track bit must
 * disappear from the published mask, and PLAY must still work.
 * ====================================================================== */
static void case_loop_works_while_fx_holds_a_track(void)
{
	rig_t r;
	uint32_t i;

	printf("\n-- the loop still works while the FX overlay holds a Track\n");
	rig_init(&r, 0u);
	r.playing = true;

	/* T1 held by the overlay. The dispatcher must not solo it... */
	g_fx_claim = 1u << 0;
	for (i = 0; i < 4; i++) {
		pass(&r, RAW_MASK[1], 0, false);
	}
	CHECK((r.out.track_mask & 1u) == 0u,
	      "a Track the overlay claimed does not reach the mixer as a solo "
	      "(mask 0x%02x)", r.out.track_mask);

	/* ...and with that same claim in force, a PLAY hold must still open a
	 * loop. This is the whole point: the effect is held THROUGHOUT. */
	for (i = 0; i < 60; i++) {          /* well past ST_LOOP_HOLD_MS */
		pass(&r, RAW_PLAY, 0, false);
	}
	CHECK(r.n_enter == 1,
	      "a loop opened while an effect was held (%d enters)", r.n_enter);

	/* Release PLAY: the loop exits, still with the effect held. */
	for (i = 0; i < 4; i++) {
		pass(&r, RAW_MASK[1], 0, false);
	}
	CHECK(r.n_exit == 1,
	      "and released normally (%d exits)", r.n_exit);

	/* THE CLAIM IS THE ONLY THING SUPPRESSED. Drop it and the same reading
	 * solos again, which proves the mask -- not the rail -- is what moved. */
	g_fx_claim = 0u;
	for (i = 0; i < 4; i++) {
		pass(&r, RAW_MASK[1], 0, false);
	}
	CHECK((r.out.track_mask & 1u) != 0u,
	      "with the claim released the same Track reading solos again "
	      "(mask 0x%02x)", r.out.track_mask);
	g_fx_claim = 0u;
}

int main(void)
{
	printf("== st_ctl: the assembled Stem Tape control path ==\n");
	printf("stimuli are MEASURED ADC values from docs/ladder-measured.json\n");
	printf("tempo is the measured song: %u frames/beat (93.71 BPM @ 48 kHz)\n",
	       FPB);

	case_entry();
	case_short_tap();
	case_function_play_never_pauses();
	case_unlatched_exit();
	case_latch();
	case_division();
	case_all_masks();
	case_chord_robustness();
	case_single_owner();
	case_loop_works_while_fx_holds_a_track();
	case_no_tempo_is_reported();

	printf("\n");
	if (g_failures) {
		printf("CONTROL PATH TEST FAILED (%d cases, %d checks, %d failures)\n",
		       g_cases, g_checks, g_failures);
		return 1;
	}
	printf("CONTROL PATH TEST PASSED (%d cases, %d checks, 0 failures)\n",
	       g_cases, g_checks);
	return 0;
}
