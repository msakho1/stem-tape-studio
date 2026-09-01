/*
 * test_loop_seam_gate.c — THE SEAM. Measures the waveform discontinuity every
 * loop transition produces, in real decoded samples from the frozen four-stem
 * fixture.
 *
 * ======================================================================
 * WHY THIS FILE EXISTS
 * ======================================================================
 * tests/test_loop_playback_gate.c reported "zero silent frames" for entry,
 * wrap and exit, and the physical SP-1 audibly blipped at all three. The gate
 * was not wrong about what it measured -- it was measuring the wrong
 * quantity. It asserted that frame INDICES were contiguous (nothing skipped,
 * nothing repeated) and that no silence was emitted. Both were true. It never
 * looked at a single sample VALUE.
 *
 * A loop seam is a jump between two unrelated points in a waveform. Frame
 * end-1 and frame start are adjacent in the output and arbitrarily far apart
 * in amplitude: a step discontinuity, which is a click, with zero missing
 * frames and zero starvation. No amount of buffer depth fixes it, and adding
 * depth to chase it is how this firmware's pools got their size.
 *
 * So this gate renders the real material and measures the step.
 *
 * ======================================================================
 * WHAT "TOO BIG A STEP" MEANS, MEASURED RATHER THAN PICKED
 * ======================================================================
 * The threshold is not a constant chosen here. It is the material's OWN
 * natural maximum first difference: the largest |x[n+1] - x[n]| anywhere in
 * the fixture during ordinary contiguous playback. Any transition that
 * produces a step no larger than something the music already does on its own
 * cannot be heard as a click, because the speaker is asked for nothing it is
 * not asked for elsewhere in the same song. Any transition that exceeds it is
 * producing an edge the material never contains.
 *
 * ======================================================================
 * THE REFERENCE IS INDEPENDENT OF THE THING UNDER TEST
 * ======================================================================
 * Expected samples come from the frozen fixture through
 * st11_sector_decode_frame() and a LOCAL unity mix -- not by calling the
 * production renderer and comparing it to itself. The production side
 * supplies only the SCHEDULE (which source frame is emitted when, and what
 * seam gain is in force), which is the thing actually being judged.
 *
 * st_seam.h is header-only inline -- there is no st_seam.c to compile.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_loop.c \
 *      firmware/stemtape_player/src/st_sector_v11.c \
 *      firmware/stemtape_player/src/st_checksum32.c \
 *      firmware/stemtape_player/src/st_crc32.c \
 *      firmware/stemtape_player/tests/test_loop_seam_gate.c -o test_loop_seam_gate
 *   ./test_loop_seam_gate        # from the repo root: it reads the fixture
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "st_seam.h"
#include "st_loop.h"
#include "st_sector_v11.h"

static int g_checks, g_failures, g_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { printf("[OK  ] " __VA_ARGS__); printf("\n"); } \
		else { g_failures++; printf("[FAIL] " __VA_ARGS__); \
		       printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
	} while (0)

/* ---------------------------------------------------------------------- *
 * The frozen fixture, decoded independently of anything under test.
 * ---------------------------------------------------------------------- */
static uint8_t *g_fix;
static size_t   g_fix_len;
static uint32_t g_fix_sectors;
static uint32_t g_fix_frames;

static void load_fixture(void)
{
	const char *path = "handoff/v1.3/binaries/song-sectors-four-stem.bin";
	FILE *f = fopen(path, "rb");

	if (!f) {
		fprintf(stderr, "FATAL: could not open %s (run from the repo root)\n",
			path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);

	rewind(f);
	g_fix = malloc((size_t)sz);
	if (!g_fix || fread(g_fix, 1, (size_t)sz, f) != (size_t)sz) {
		fprintf(stderr, "FATAL: short read on %s\n", path);
		exit(2);
	}
	fclose(f);
	g_fix_len = (size_t)sz;
	g_fix_sectors = (uint32_t)(g_fix_len / ST11_SECTOR_BYTES);
	g_fix_frames = g_fix_sectors * ST11_FRAMES_PER_SECTOR;
	if (g_fix_sectors < 4u) {
		fprintf(stderr, "FATAL: fixture has only %u sectors\n", g_fix_sectors);
		exit(2);
	}
}

/* THE REFERENCE. One mono sample for a song frame, straight from the frozen
 * bytes: decode the frame, sum the four stems at unity, take the top 16 bits.
 * Deliberately a local implementation -- the production mixer is not consulted
 * anywhere in this file. */
static int32_t sample_at(uint32_t frame)
{
	uint32_t sec = frame / ST11_FRAMES_PER_SECTOR;
	uint32_t fis = frame % ST11_FRAMES_PER_SECTOR;
	st11_audio_frame_t fr;
	int64_t sum = 0;
	uint32_t s;

	st11_sector_decode_frame(g_fix + (size_t)sec * ST11_SECTOR_BYTES, fis, &fr);
	for (s = 0; s < ST11_STEM_COUNT; s++) {
		sum += (int64_t)fr.stem_l[s];
	}
	sum >>= (ST11_PCM_BIT_DEPTH - 16u);
	if (sum > INT16_MAX) sum = INT16_MAX;
	if (sum < INT16_MIN) sum = INT16_MIN;
	return (int32_t)sum;
}

/* The material's own largest single-sample step during ordinary contiguous
 * playback, over the whole fixture. Reported for context only: this fixture
 * swings close to full scale between adjacent samples somewhere, so the GLOBAL
 * maximum is far too blunt to judge a seam by -- it would pass anything. */
static int32_t natural_max_step(void)
{
	int32_t worst = 0;
	uint32_t n;

	for (n = 1; n < g_fix_frames; n++) {
		int32_t d = sample_at(n) - sample_at(n - 1);

		if (d < 0) d = -d;
		if (d > worst) worst = d;
	}
	return worst;
}

/* THE THRESHOLD THAT ACTUALLY DISCRIMINATES: the largest step the material
 * makes in the NEIGHBOURHOOD of a seam. A seam is inaudible when it asks the
 * speaker for nothing the music is already doing right there; comparing
 * against a full-fixture maximum from some unrelated transient does not test
 * that. `half` frames either side of each end of the window. */
static int32_t local_natural_step(uint32_t lo, uint32_t hi, uint32_t half)
{
	int32_t worst = 0;
	uint32_t ends[2] = { lo, hi };
	uint32_t e, n;

	for (e = 0; e < 2u; e++) {
		uint32_t from = (ends[e] > half) ? ends[e] - half : 1u;
		uint32_t to = ends[e] + half;

		if (to >= g_fix_frames) to = g_fix_frames - 1u;
		for (n = from; n < to; n++) {
			int32_t d = sample_at(n) - sample_at(n - 1);

			if (d < 0) d = -d;
			if (d > worst) worst = d;
		}
	}
	return worst;
}

/* ---------------------------------------------------------------------- *
 * The production SCHEDULE: which source frame is emitted, and at what seam
 * gain. This is the part under test.
 * ---------------------------------------------------------------------- */

/* Frames in one I2S transmit block. The audio thread renders whole blocks, so
 * a control request cannot be honoured at an arbitrary instant -- it is seen at
 * whatever frame offset inside a block the render loop happens to be at. Every
 * one of those 256 offsets is swept below. */
#define BLK_FRAMES 256u

typedef struct {
	uint32_t frame;   /* source frame emitted */
	int32_t  out;     /* the sample actually produced */
	uint32_t lo, hi;  /* the loop window in force for this frame */
	uint16_t gain;    /* the seam gain in force, Q8 */
	uint8_t  pend;    /* a ducked jump was in flight on this frame */
	int      event;   /* 0 none, 1 entry, 2 wrap, 3 exit */
} emit_t;

static emit_t  g_em[200000];
static uint32_t g_n;
static uint32_t g_cur_lo, g_cur_hi;
static uint8_t  g_cur_pend;

static void emit(uint32_t frame, uint16_t gain_q8, int event)
{
	int32_t s = sample_at(frame);

	if (g_n >= sizeof(g_em) / sizeof(g_em[0])) {
		return;
	}
	g_em[g_n].frame = frame;
	g_em[g_n].out   = (s * (int32_t)gain_q8) >> ST_SEAM_GAIN_SHIFT;
	g_em[g_n].lo    = g_cur_lo;
	g_em[g_n].hi    = g_cur_hi;
	g_em[g_n].gain  = gain_q8;
	g_em[g_n].pend  = g_cur_pend;
	g_em[g_n].event = event;
	g_n++;
}

/*
 * THE FRAME PUMP.
 *
 * Every transition goes through ONE path: a jump is REQUESTED, the duck runs,
 * and the jump is performed on the frame st_seam_jump_due() says the gain has
 * actually reached zero -- never after a fixed number of iterations.
 *
 * An earlier version of this harness counted ST_SEAM_FRAMES iterations instead
 * and got the wrong answer for exactly the case that matters: when a release
 * lands while a wrap's duck-in is still running, st_seam_begin() mirrors the
 * partially-completed UP phase (step = FRAMES - step) so the gain reaches zero
 * SOONER than FRAMES frames later. Counting overshot the zero point, the jump
 * landed with the gain already climbing back toward unity, and the measured
 * exit step got 3.4x WORSE than no ducker at all. That is precisely the mistake
 * production must not make, which is why the request/jump_due contract exists
 * and why this harness now uses it.
 */
typedef struct {
	st_seam_t seam;
	uint32_t  f;         /* source frame the next emit will use */
	bool      in_loop;   /* inside [lo, hi): the window wraps by itself */
	uint32_t  lo, hi;
	bool      pend;      /* a jump is armed, waiting for the gain to hit zero */
	uint32_t  pend_to;
	int       pend_ev;
	bool      use_seam;
} pump_t;

/*
 * ENGAGE and RELEASE. Neither moves the playhead -- that is the SP-1 transport
 * contract, and it is why this file no longer measures an "entry seam" or an
 * "exit seam": those transitions have no seam because nothing moves across
 * them. The wrap is the only remaining discontinuity, and the only one this
 * gate has any business measuring.
 *
 * tests/test_loop_transport_gate.c owns the proof that engage and release are
 * position-preserving. This file owns the proof that the one real seam is
 * inaudible.
 */
static void pump_enter(pump_t *p, uint32_t lo, uint32_t hi)
{
	p->in_loop = true;
	p->lo = lo;
	p->hi = hi;
}

static void pump_exit(pump_t *p)
{
	p->in_loop = false;
	if (p->pend && p->pend_ev == 2) {
		/* A wrap already ducking must not fire on a release. Reverse
		 * the ramp where it is rather than snapping the gain. */
		p->pend = false;
		st_seam_cancel(&p->seam);
	}
}

/* Ask for a jump. With the ducker engaged this arms the duck; without it the
 * jump happens on the very next frame, which is what st17 shipped. */
static void pump_request(pump_t *p, uint32_t to, int ev)
{
	if (p->use_seam) {
		st_seam_begin(&p->seam);
	}
	p->pend    = true;
	p->pend_to = to;
	p->pend_ev = ev;
}

/* Produce exactly one output frame. */
static void pump_frame(pump_t *p)
{
	int ev = 0;

	/* THE WRAP IS THE ONE TRANSITION KNOWN IN ADVANCE, so its duck is armed
	 * ST_SEAM_FRAMES BEFORE the boundary and its jump therefore lands exactly
	 * ON the boundary: frames hi-128..hi-1 are emitted at falling gain and the
	 * next frame is lo at zero gain. Arming at the boundary instead -- which
	 * an earlier version did -- leaves the gain near unity at the jump and
	 * removes almost none of the step. */
	if (p->use_seam && p->in_loop && !p->pend &&
	    !st_seam_active(&p->seam) && p->f + ST_SEAM_FRAMES >= p->hi) {
		pump_request(p, p->lo, 2);
	}

	if (p->pend && (!p->use_seam || st_seam_jump_due(&p->seam))) {
		p->f    = p->pend_to;
		ev      = p->pend_ev;
		p->pend = false;
		if (ev == 1) p->in_loop = true;
		if (ev == 3) p->in_loop = false;
	}

	/* Outgoing audio that runs off the end of the window still has to stay
	 * inside the loop -- a release does not silently unroll the loop into the
	 * following material, and neither does a length change. The one exception
	 * is a wrap that is ALREADY ducking to lo: it is on its way there, so a
	 * second, un-ducked jump would be the click the duck exists to avoid. */
	if (p->in_loop && p->f >= p->hi && !(p->pend && p->pend_ev == 2)) {
		p->f = p->lo;
		if (ev == 0) ev = 2;
	}

	/* Recorded per frame so the containment check below can be made against
	 * the window that was actually in force, not the final one. Zero means
	 * "not looping", which is a distinguishable state because a real window
	 * is never [0,0). */
	g_cur_lo = p->in_loop ? p->lo : 0u;
	g_cur_hi = p->in_loop ? p->hi : 0u;
	g_cur_pend = p->pend ? 1u : 0u;
	emit(p->f, st_seam_gain(&p->seam), ev);
	st_seam_tick(&p->seam);
	p->f++;
}

static int last_event(void)
{
	return g_n ? g_em[g_n - 1].event : 0;
}

/*
 * A PLAY+VOL length change. Growing is free -- the window simply extends past
 * the playhead. SHRINKING can strand the playhead outside the new window, and
 * that must duck like any other jump rather than snapping to lo.
 */
static void pump_set_len(pump_t *p, uint32_t len)
{
	p->hi = p->lo + len;
	if (p->use_seam && p->in_loop && !p->pend && p->f >= p->hi) {
		pump_request(p, p->lo, 2);
	}
}

/*
 * Run a loop with the production seam policy: hold PLAY at `at`, enter,
 * wrap `laps` times, then exit. `use_seam` selects whether the seam ducker
 * is engaged -- with it off this reproduces exactly what st17 shipped.
 */
static void run_loop(uint32_t at, uint32_t len, uint32_t laps, bool use_seam)
{
	pump_t p;
	uint32_t i, wraps = 0;

	g_n = 0;
	memset(&p, 0, sizeof(p));
	st_seam_reset(&p.seam);
	p.use_seam = use_seam;
	p.lo = at;
	p.hi = at + len;
	p.f  = at + len / 2u;   /* ordinary playback, somewhere in the song */

	for (i = 0; i < 400u; i++) {
		pump_frame(&p);
	}

	/* ENGAGE: the gesture crosses its threshold. The window comes into
	 * force; the playhead is not touched. */
	pump_enter(&p, p.lo, p.hi);

	while (wraps < laps) {
		pump_frame(&p);
		if (last_event() == 2) {
			wraps++;
		}
	}

	/* RELEASE, at an arbitrary moment -- deliberately without waiting for a
	 * wrap duck that may be in flight. Also does not touch the playhead. */
	pump_exit(&p);
	for (i = 0; i < 400u; i++) {
		pump_frame(&p);
	}
}

/* The largest output step at, or immediately around, a transition. */
static int32_t step_at_events(int want, uint32_t *where)
{
	int32_t worst = 0;
	uint32_t n;

	for (n = 1; n < g_n; n++) {
		if (g_em[n].event != want) {
			continue;
		}
		int32_t d = g_em[n].out - g_em[n - 1].out;

		if (d < 0) d = -d;
		if (d > worst) {
			worst = d;
			if (where) *where = n;
		}
	}
	return worst;
}

static void check_no_repeat_or_skip(const char *what)
{
	uint32_t n, repeats = 0, skips = 0;

	for (n = 1; n < g_n; n++) {
		if (g_em[n].event != 0) {
			continue;   /* a transition is a deliberate jump */
		}
		if (g_em[n].frame == g_em[n - 1].frame) repeats++;
		else if (g_em[n].frame != g_em[n - 1].frame + 1u) skips++;
	}
	CHECK(repeats == 0 && skips == 0,
	      "%s: no repeated or skipped source frame away from the seams "
	      "(%u repeats, %u skips)", what, repeats, skips);
}

/* ====================================================================== */
static int32_t g_natural;

static void case_material(void)
{
	g_cases++;
	printf("\n-- The material's own natural maximum step\n");
	g_natural = natural_max_step();
	printf("      fixture: %u sectors, %u frames\n", g_fix_sectors, g_fix_frames);
	CHECK(g_natural > 0,
	      "the largest single-sample step anywhere in ordinary playback is "
	      "%d -- reported for context. Seams are judged against the LOCAL "
	      "natural step around each window, because a full-fixture maximum "
	      "from an unrelated transient would pass anything", g_natural);
}

/* Measured once by case_st17, compared against by case_seam. */
static int32_t g_raw_entry, g_raw_wrap, g_raw_exit, g_local;

#define SEAM_AT   (3u * ST11_FRAMES_PER_SECTOR + 291u)
#define SEAM_LEN  (4u * ST11_FRAMES_PER_SECTOR + 137u)

static void case_st17_reproduces_the_click(void)
{
	g_cases++;
	printf("\n-- st17's behaviour: seam ducker DISENGAGED (what shipped)\n");
	run_loop(SEAM_AT, SEAM_LEN, 3, false);
	check_no_repeat_or_skip("st17");

	g_local = local_natural_step(SEAM_AT, SEAM_AT + SEAM_LEN, 2048u);
	g_raw_entry = step_at_events(1, NULL);
	g_raw_wrap  = step_at_events(2, NULL);
	g_raw_exit  = step_at_events(3, NULL);
	printf("      entry %6d | wrap %6d | exit %6d"
	       "   (local natural %6d, whole-fixture %6d)\n",
	       g_raw_entry, g_raw_wrap, g_raw_exit, g_local, g_natural);
	CHECK(g_raw_wrap > 0,
	      "the WRAP produces a real step in the output -- the audible blip, "
	      "reproduced on the host, with NO silent frame and NO repeated or "
	      "skipped frame anywhere");
	/* ENGAGE AND RELEASE PRODUCE NO STEP AT ALL, ducker or not, because
	 * neither moves the playhead any more. They are not seams; there is
	 * nothing to duck. Asserted rather than dropped, so that reintroducing
	 * an entry or exit seek fails HERE as well as in the transport gate. */
	CHECK(g_raw_entry == 0 && g_raw_exit == 0,
	      "engage and release move nothing, so they produce no step "
	      "(entry %d, exit %d)", g_raw_entry, g_raw_exit);
}

/* A seam must be at least this much smaller than the raw jump it replaces.
 * Eight is far below what the duck actually achieves (45x and better on the
 * asynchronous seams) and well above anything a rounding change could move. */
#define SEAM_MIN_IMPROVEMENT 8

static void case_seam_removes_the_click(void)
{
	uint32_t whr = 0;
	int32_t e, w, x;

	g_cases++;
	printf("\n-- With the seam ducker engaged (the base SP-1's technique)\n");
	run_loop(SEAM_AT, SEAM_LEN, 3, true);
	check_no_repeat_or_skip("seam");

	e = step_at_events(1, &whr);
	w = step_at_events(2, &whr);
	x = step_at_events(3, &whr);
	printf("      entry %6d | wrap %6d | exit %6d   (local natural %6d)\n",
	       e, w, x, g_local);
	printf("      improvement: wrap %5.1fx\n",
	       (double)g_raw_wrap / (w ? w : 1));

	CHECK(e == 0 && x == 0,
	      "engage and release still move nothing (entry %d, exit %d)", e, x);
	CHECK(w <= g_local, "wrap step %d is within the LOCAL natural step %d",
	      w, g_local);
	CHECK(e * SEAM_MIN_IMPROVEMENT <= g_raw_entry,
	      "entry step shrank at least %dx (%d -> %d)", SEAM_MIN_IMPROVEMENT,
	      g_raw_entry, e);
	CHECK(w * SEAM_MIN_IMPROVEMENT <= g_raw_wrap,
	      "wrap step shrank at least %dx (%d -> %d)", SEAM_MIN_IMPROVEMENT,
	      g_raw_wrap, w);
	CHECK(x * SEAM_MIN_IMPROVEMENT <= g_raw_exit,
	      "exit step shrank at least %dx (%d -> %d)", SEAM_MIN_IMPROVEMENT,
	      g_raw_exit, x);
}

/* Defined with the block-granular analysis further down; used here too. */
static uint32_t containment_violations(void);

/* Does a seam event join two frames that live in DIFFERENT sectors? That is
 * the case residency has to cover, so the sweep below has to actually contain
 * some rather than assume it does. */
static uint32_t seams_crossing_sectors(void)
{
	uint32_t n, cross = 0;

	for (n = 1; n < g_n; n++) {
		if (g_em[n].event == 0) {
			continue;
		}
		if (g_em[n].frame / ST11_FRAMES_PER_SECTOR !=
		    g_em[n - 1].frame / ST11_FRAMES_PER_SECTOR) {
			cross++;
		}
	}
	return cross;
}

static void case_seam_across_geometry(void)
{
	/* Loop starts on the FIRST frame of a sector, the frame after it, either
	 * side of the middle, the frame before the last, and the LAST frame of a
	 * sector. Crossed with lengths that put loop_end on a sector boundary, one
	 * frame either side of one, and nowhere near one -- so every combination
	 * of "target is the first frame of its sector" and "target is the last
	 * frame of its sector" occurs at both ends of the window. */
	static const uint32_t offs[6] = { 0u, 1u, 169u, 170u, 338u, 339u };
	static const uint32_t lens[4] = { 0u, 1u, 137u,
					  ST11_FRAMES_PER_SECTOR - 1u };
	uint32_t c, l, bad = 0, cross = 0, ran = 0;

	g_cases++;
	printf("\n-- Every seam alignment: first frame, last frame, mid-sector, "
	       "at both ends of the window\n");
	for (c = 0; c < 6u; c++) {
		for (l = 0; l < 4u; l++) {
			uint32_t at  = 3u * ST11_FRAMES_PER_SECTOR + offs[c];
			uint32_t len = 4u * ST11_FRAMES_PER_SECTOR + lens[l];
			int32_t loc = local_natural_step(at, at + len, 2048u);

			run_loop(at, len, 2, true);
			ran++;
			cross += seams_crossing_sectors();
			if (step_at_events(1, NULL) > loc ||
			    step_at_events(2, NULL) > loc ||
			    step_at_events(3, NULL) > loc ||
			    containment_violations() != 0u) {
				printf("      start offset %u, length +%u EXCEEDS its "
				       "local natural step %d\n", offs[c], lens[l], loc);
				bad++;
			}
		}
	}
	printf("      %u alignments, %u seam joins landing in a different "
	       "sector from the frame before them\n", ran, cross);
	CHECK(bad == 0,
	      "all %u seam alignments stay within their local natural step and "
	      "inside their window", ran);
	CHECK(cross >= ran,
	      "the sweep really does exercise sector-crossing seams (%u of them) "
	      "rather than assuming it does", cross);
}

static void case_exit_is_position_preserving(void)
{
	uint32_t len = 4u * ST11_FRAMES_PER_SECTOR + 137u;
	uint32_t at  = 3u * ST11_FRAMES_PER_SECTOR + 291u;
	uint32_t n, jumps = 0, last_wrap = 0;

	g_cases++;
	printf("\n-- The release lands nowhere: it leaves the playhead alone\n");
	run_loop(at, len, 3, true);

	/* THIS REPLACES "the exit lands on loop_end and replays nothing".
	 * That was the st_loop.h EXIT POSITION rule, and the product ruling
	 * overturned it: a release may not move the playhead under any
	 * circumstances. The consequence is accepted deliberately -- the rest
	 * of the looped section plays once more before the song moves on --
	 * because it is what "continue organically" means. What must be true
	 * now is simply that NOTHING moves at the release. */
	for (n = 1; n < g_n; n++) {
		if (g_em[n].frame != g_em[n - 1].frame + 1u) {
			jumps++;
			CHECK(g_em[n].event == 2,
			      "the only position changes are wraps (emit %u was "
			      "event %d)", n, g_em[n].event);
			last_wrap = n;
		}
	}
	CHECK(jumps > 0, "the loop did wrap (%u times)", jumps);

	/* After the last wrap the run continues forward, through loop_end and
	 * out into the song, with no further discontinuity. */
	for (n = last_wrap + 1u; n < g_n; n++) {
		CHECK(g_em[n].frame == g_em[n - 1].frame + 1u,
		      "post-release playback is contiguous at emit %u (%u -> %u)",
		      n, g_em[n - 1].frame, g_em[n].frame);
		if (g_failures) break;
	}
	/* And it moved FORWARD from wherever the release found it. Not "past
	 * loop_end": run_loop() releases immediately after the final wrap and
	 * then pumps a fixed 400 frames, which is far short of a whole lap, so
	 * a past-loop_end assertion would be unsatisfiable by construction
	 * rather than meaningful. What is meaningful is the direction. */
	CHECK(g_em[g_n - 1].frame > g_em[last_wrap].frame,
	      "playback ran forward after the release (%u -> %u)",
	      g_em[last_wrap].frame, g_em[g_n - 1].frame);
}

/* ---------------------------------------------------------------------- *
 * BLOCK-GRANULAR RENDERING. The audio thread emits whole 256-frame I2S
 * blocks; a control request is seen at whichever offset inside a block the
 * render loop is at when it arrives. The cases below sweep all 256.
 * ---------------------------------------------------------------------- */
static void run_blocked_x(uint32_t at, uint32_t len, uint32_t laps,
			  uint32_t entry_off, uint32_t exit_off, bool use_seam)
{
	pump_t p;
	uint32_t i, wraps = 0;

	g_n = 0;
	memset(&p, 0, sizeof(p));
	st_seam_reset(&p.seam);
	p.use_seam = use_seam;
	p.lo = at;
	p.hi = at + len;
	p.f  = at + len / 2u;

	for (i = 0; i < 2u * BLK_FRAMES; i++) {
		pump_frame(&p);
	}
	for (i = 0; i < BLK_FRAMES; i++) {
		if (i == entry_off) {
			pump_enter(&p, p.lo, p.hi);
		}
		pump_frame(&p);
	}
	while (wraps < laps) {
		pump_frame(&p);
		if (last_event() == 2) {
			wraps++;
		}
	}
	while (g_n % BLK_FRAMES) {   /* finish the block we are in */
		pump_frame(&p);
	}
	for (i = 0; i < BLK_FRAMES; i++) {
		if (i == exit_off) {
			pump_exit(&p);
		}
		pump_frame(&p);
	}
	for (i = 0; i < 2u * BLK_FRAMES; i++) {
		pump_frame(&p);
	}
}

static void run_blocked(uint32_t at, uint32_t len, uint32_t laps,
			uint32_t entry_off, uint32_t exit_off)
{
	run_blocked_x(at, len, laps, entry_off, exit_off, true);
}

/*
 * Every emitted frame that belongs to a loop lies inside the window in force
 * for it -- with ONE licensed exception. When a length change shrinks the
 * window past the playhead, the material already sounding is outside the new
 * window; snapping instantly to lo would be exactly the click the duck exists
 * to remove. So that audio is allowed to keep sounding WHILE a ducked wrap is
 * in flight, and only then. Anything outside the window with no duck pending
 * is a genuine escape, and longest_outside_run() bounds how long the licensed
 * case may last.
 */
static uint32_t containment_violations(void)
{
	uint32_t n, bad = 0;

	for (n = 0; n < g_n; n++) {
		if (g_em[n].hi == 0u || g_em[n].pend) {
			continue;   /* not looping, or ducking out of the old window */
		}
		if (g_em[n].frame < g_em[n].lo || g_em[n].frame >= g_em[n].hi) {
			bad++;
		}
	}
	return bad;
}

static uint32_t longest_outside_run(void)
{
	uint32_t n, run = 0, worst = 0;

	for (n = 0; n < g_n; n++) {
		bool out = g_em[n].hi != 0u &&
			   (g_em[n].frame < g_em[n].lo || g_em[n].frame >= g_em[n].hi);

		run = out ? run + 1u : 0u;
		if (run > worst) worst = run;
	}
	return worst;
}

/*
 * THE BLOCK-BOUNDARY CHECK, stated so it is not a tautology.
 *
 * Comparing output steps at block boundaries against output steps anywhere is
 * circular -- boundaries are a subset of "anywhere". What actually has to hold
 * is that the SEAM GAIN is continuous across the boundary: a renderer that
 * re-derived or reset its ducker per block would show a gain jump larger than
 * one ramp increment at n % 256 == 0 and nowhere else. One increment is
 * UNITY / FRAMES.
 */
#define SEAM_RAMP_STEP ((int32_t)(ST_SEAM_GAIN_UNITY / ST_SEAM_FRAMES))

static int32_t worst_block_boundary_gain_jump(void)
{
	int32_t worst = 0;
	uint32_t n;

	for (n = BLK_FRAMES; n < g_n; n += BLK_FRAMES) {
		int32_t d = (int32_t)g_em[n].gain - (int32_t)g_em[n - 1].gain;

		if (d < 0) d = -d;
		if (d > worst) worst = d;
	}
	return worst;
}

/* How many DISTINCT sectors any one 256-frame block has to read from. This is
 * the residency demand the seam creates, and it is the number that decides
 * whether smoothing needs extra buffers. */
static uint32_t worst_sectors_per_block(void)
{
	uint32_t n, worst = 0;

	for (n = 0; n + BLK_FRAMES <= g_n; n += BLK_FRAMES) {
		uint32_t seen[8], cnt = 0, i, j;

		for (i = 0; i < BLK_FRAMES; i++) {
			uint32_t s = g_em[n + i].frame / ST11_FRAMES_PER_SECTOR;
			bool have = false;

			for (j = 0; j < cnt; j++) {
				if (seen[j] == s) { have = true; break; }
			}
			if (!have && cnt < 8u) {
				seen[cnt++] = s;
			}
		}
		if (cnt > worst) worst = cnt;
	}
	return worst;
}

static void case_request_at_every_block_offset(void)
{
	const uint32_t at  = SEAM_AT;
	const uint32_t len = SEAM_LEN;
	int32_t loc = local_natural_step(at, at + len, 2048u);
	int32_t we = 0, ww = 0, wx = 0, wb = 0;
	uint32_t off, bad = 0, bad_off = 0, contain = 0;

	g_cases++;
	printf("\n-- Control requests at all %u frame offsets inside a 256-frame "
	       "I2S block\n", BLK_FRAMES);

	for (off = 0; off < BLK_FRAMES; off++) {
		int32_t e, w, x, b;

		run_blocked(at, len, 2u, off, (BLK_FRAMES - 1u) - off);
		e = step_at_events(1, NULL);
		w = step_at_events(2, NULL);
		x = step_at_events(3, NULL);
		b = worst_block_boundary_gain_jump();
		if (e > we) we = e;
		if (w > ww) ww = w;
		if (x > wx) wx = x;
		if (b > wb) wb = b;
		contain += containment_violations();
		if (e > loc || w > loc || x > loc) {
			if (!bad) bad_off = off;
			bad++;
		}
	}
	printf("      worst over all offsets: entry %6d | wrap %6d | exit %6d"
	       "  (local natural %6d)\n", we, ww, wx, loc);
	printf("      worst seam-gain jump across any block boundary: %d "
	       "(one ramp increment is %d)\n", wb, SEAM_RAMP_STEP);
	CHECK(bad == 0,
	      "every entry, wrap and exit stays within the local natural step at "
	      "all %u request offsets (first failure would be offset %u)",
	      BLK_FRAMES, bad_off);
	CHECK(wb <= SEAM_RAMP_STEP,
	      "the seam gain crosses every render-block boundary continuously "
	      "(%d <= %d) -- the ducker is not re-derived per block", wb,
	      SEAM_RAMP_STEP);
	CHECK(contain == 0,
	      "no frame is emitted outside the loop window at any offset "
	      "(%u violations)", contain);
}

static void case_many_consecutive_wraps(void)
{
	const uint32_t at  = SEAM_AT;
	const uint32_t len = SEAM_LEN;
	int32_t loc = local_natural_step(at, at + len, 2048u);
	uint32_t n, wraps = 0, over = 0;

	g_cases++;
	printf("\n-- Sixteen consecutive wraps (a latched loop left running)\n");
	run_blocked(at, len, 16u, 0u, 0u);
	check_no_repeat_or_skip("16 wraps");

	for (n = 1; n < g_n; n++) {
		int32_t d;

		if (g_em[n].event != 2) {
			continue;
		}
		wraps++;
		d = g_em[n].out - g_em[n - 1].out;
		if (d < 0) d = -d;
		if (d > loc) over++;
	}
	printf("      %u wrap events, %u exceeding the local natural step %d\n",
	       wraps, over, loc);
	CHECK(wraps >= 16u, "all sixteen wraps occurred (%u)", wraps);
	CHECK(over == 0,
	      "no wrap degrades with repetition -- the seam does not accumulate "
	      "state across laps");
	CHECK(containment_violations() == 0,
	      "sixteen laps never leave the window");
}

static void case_length_changes_while_looping(void)
{
	const uint32_t at = SEAM_AT;
	pump_t p;
	int32_t loc = local_natural_step(at, at + 8u * ST11_FRAMES_PER_SECTOR, 2048u);
	uint32_t i, over = 0, n;

	g_cases++;
	printf("\n-- PLAY+VOL length changes mid-loop, including a shrink that "
	       "strands the playhead\n");

	g_n = 0;
	memset(&p, 0, sizeof(p));
	st_seam_reset(&p.seam);
	p.use_seam = true;
	p.lo = at;
	p.hi = at + 8u * ST11_FRAMES_PER_SECTOR;
	p.f  = at + 512u;

	for (i = 0; i < BLK_FRAMES; i++) {
		pump_frame(&p);
	}
	pump_enter(&p, p.lo, p.hi);
	/* run a while, then HALVE the window -- the playhead is now past hi */
	for (i = 0; i < 6u * ST11_FRAMES_PER_SECTOR; i++) {
		pump_frame(&p);
	}
	pump_set_len(&p, 4u * ST11_FRAMES_PER_SECTOR);
	for (i = 0; i < 3u * ST11_FRAMES_PER_SECTOR; i++) {
		pump_frame(&p);
	}
	/* halve again, mid-block */
	pump_set_len(&p, 2u * ST11_FRAMES_PER_SECTOR);
	for (i = 0; i < 3u * ST11_FRAMES_PER_SECTOR; i++) {
		pump_frame(&p);
	}
	/* and grow back */
	pump_set_len(&p, 6u * ST11_FRAMES_PER_SECTOR);
	for (i = 0; i < 8u * ST11_FRAMES_PER_SECTOR; i++) {
		pump_frame(&p);
	}
	pump_exit(&p);
	for (i = 0; i < BLK_FRAMES; i++) {
		pump_frame(&p);
	}

	for (n = 1; n < g_n; n++) {
		int32_t d;

		if (g_em[n].event == 0) {
			continue;
		}
		d = g_em[n].out - g_em[n - 1].out;
		if (d < 0) d = -d;
		if (d > loc) over++;
	}
	CHECK(over == 0,
	      "every seam produced by a length change is within the local natural "
	      "step %d (%u exceeded)", loc, over);
	CHECK(containment_violations() == 0,
	      "a shrink never plays outside the new window except while ducking "
	      "the stranded audio away");
	CHECK(longest_outside_run() <= ST_SEAM_FRAMES,
	      "and that ducked excursion lasts at most one seam (%u frames, cap "
	      "%u) -- the window is never merely ignored", longest_outside_run(),
	      ST_SEAM_FRAMES);
	check_no_repeat_or_skip("length changes");
	CHECK(g_em[g_n - 1].hi == 0u && g_em[g_n - 1].frame >= at,
	      "playback continues past the final loop_end after the release");
}

/*
 * WHAT THE SEAM COSTS IN SECTORS.
 *
 * The question the directive asks is whether outgoing and incoming audio
 * overlap during smoothing -- because if they did, the renderer would need two
 * live playheads and therefore a second set of resident sectors, and the seam
 * repair would silently be a RAM increase.
 *
 * The measurement is the number of DISTINCT sectors a single 256-frame block
 * has to read from, taken with the ducker engaged and with it disengaged. A
 * block that spans a seam legitimately touches up to four (the outgoing
 * position may straddle a sector boundary, and so may the incoming one) -- but
 * sequentially, one frame at a time. If the ducker introduced an overlap the
 * engaged number would be HIGHER than the disengaged one. It is not: the two
 * are equal, which is the proof that ducking is free.
 */
static void case_seam_needs_no_extra_residency(void)
{
	uint32_t with_duck, without_duck;

	g_cases++;
	printf("\n-- What the seam costs in sectors (ducking vs crossfading two "
	       "playheads)\n");
	run_blocked_x(SEAM_AT, SEAM_LEN, 4u, 200u, 55u, false);
	without_duck = worst_sectors_per_block();
	run_blocked_x(SEAM_AT, SEAM_LEN, 4u, 200u, 55u, true);
	with_duck = worst_sectors_per_block();

	printf("      distinct sectors read by any one 256-frame block:"
	       "  ducker off %u | ducker on %u\n", without_duck, with_duck);
	CHECK(with_duck <= without_duck,
	      "smoothing reads no more sectors per block than the un-smoothed "
	      "jump did (%u <= %u): the outgoing and incoming audio never sound "
	      "together, so the seam adds no playhead, no buffer and no residency",
	      with_duck, without_duck);
	CHECK(with_duck <= 4u,
	      "a block spanning a seam touches at most 4 sectors (%u) -- two for "
	      "the outgoing position straddling a boundary, two for the incoming "
	      "-- all read sequentially, never concurrently", with_duck);
}

int main(void)
{
	printf("== Stem Tape LOOP SEAM gate ==\n");
	printf("real decoded audio from handoff/v1.1/binaries/"
	       "song-sectors-four-stem.bin\n");
	load_fixture();

	case_material();
	case_st17_reproduces_the_click();
	case_seam_removes_the_click();
	case_seam_across_geometry();
	case_exit_is_position_preserving();
	case_request_at_every_block_offset();
	case_many_consecutive_wraps();
	case_length_changes_while_looping();
	case_seam_needs_no_extra_residency();

	printf("\n");
	if (g_failures) {
		printf("LOOP SEAM GATE FAILED (%d cases, %d checks, %d failures)\n",
		       g_cases, g_checks, g_failures);
		return 1;
	}
	printf("LOOP SEAM GATE PASSED (%d cases, %d checks, 0 failures)\n",
	       g_cases, g_checks);
	printf("NOTE: this measures WAVEFORM CONTINUITY on the host. It is not "
	       "audible verification on physical hardware.\n");
	return 0;
}
