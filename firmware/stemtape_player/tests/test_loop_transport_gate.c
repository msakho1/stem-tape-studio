/*
 * test_loop_transport_gate.c -- the SP-1 loop transport contract.
 *
 * THE RULE THIS ENFORCES, in one line: engaging or releasing the loop changes
 * the transport's RULES, never its POSITION. Only the playhead crossing
 * loopEnd may move the playhead.
 *
 * WHY IT EXISTS. The shipped loop seeked BACK to the captured frame the moment
 * the gesture became a loop. loop_start is captured at PLAY-DOWN, so by the
 * time the hold threshold expires the playhead is a whole hold past it, and
 * that seek replayed every sample in between. On a vocal reading "for me no",
 * engaging during "me" produced
 *
 *     for me - me no
 *
 * with the syllable audibly restarting. The release did the mirror of it,
 * seeking forward to loop_end. Neither is what an SP-1 does: the loop system
 * is meant to be inaudible, and the listener should perceive only the phrase
 * repeating.
 *
 * WHAT IS ASSERTED, and how it maps to that symptom:
 *
 *   engaging emits a STRICTLY CONTIGUOUS frame sequence -- no repeat, no skip,
 *   no jump -- which is precisely "the duplicated me cannot happen"
 *
 *   releasing does the same, at every offset within a lap
 *
 *   the ONLY discontinuity anywhere is the wrap, it lands exactly on
 *   loopStart, and it happens on the boundary rather than early or late
 *
 * Every frame index here is checked against the emitted sequence, not against
 * an internal flag, so a state machine that merely believes it did the right
 * thing cannot pass.
 *
 * Build (from the repo root -- the fixture path resolves there):
 *   cc -std=c11 -Wall -Wextra -Werror -Ifirmware/stemtape_player/src \
 *      firmware/stemtape_player/src/st_sector_v11.c \
 *      firmware/stemtape_player/tests/test_loop_transport_gate.c \
 *      -o test_loop_transport_gate && ./test_loop_transport_gate
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "st_sector_v11.h"
#include "st_seam.h"

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

/* ---------------------------------------------------------------------- *
 * The frozen fixture, for a real song geometry to move a playhead over.
 * ---------------------------------------------------------------------- */
static uint8_t *g_fix;
static uint32_t g_fix_frames;

static void load_fixture(void)
{
	const char *path = "handoff/v1.1/binaries/song-sectors-four-stem.bin";
	FILE *f = fopen(path, "rb");
	long sz;

	if (!f) {
		fprintf(stderr, "FATAL: could not open %s (run from the repo root)\n",
			path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	rewind(f);
	g_fix = malloc((size_t)sz);
	if (!g_fix || fread(g_fix, 1, (size_t)sz, f) != (size_t)sz) {
		fprintf(stderr, "FATAL: short read on %s\n", path);
		exit(2);
	}
	fclose(f);
	g_fix_frames = (uint32_t)((size_t)sz / ST11_SECTOR_BYTES) *
		       ST11_FRAMES_PER_SECTOR;
}

/* ======================================================================
 * THE TRANSPORT, mirroring stem_audio_block()'s loop handling exactly:
 * the periodic boundary, the wrap armed ST_SEAM_FRAMES early, the jump
 * taken on st_seam_jump_due(), and -- the point of this file -- entry and
 * exit that touch `in_loop` and nothing else.
 * ====================================================================== */
#define BLK_FRAMES 256u
#define MAXEM 400000u

typedef struct {
	uint32_t frame;
	int      event;    /* 0 none, 1 enter, 2 wrap, 3 exit */
} emit_t;

static emit_t   g_em[MAXEM];
static uint32_t g_n;

typedef struct {
	st_seam_t seam;
	uint32_t  f;
	bool      in_loop;
	uint32_t  lo, hi;
	bool      pend;      /* a WRAP is armed and ducking */
	uint32_t  pend_to;
} tp_t;

static void tp_init(tp_t *p, uint32_t start_frame)
{
	memset(p, 0, sizeof(*p));
	st_seam_reset(&p->seam);
	p->f = start_frame;
}

/* ENGAGE. Sets the boundary. Does NOT touch p->f -- that is the contract. */
static void tp_enter(tp_t *p, uint32_t lo, uint32_t len)
{
	p->in_loop = true;
	p->lo = lo;
	p->hi = lo + len;
}

/* RELEASE. Stops future wrapping. Does NOT touch p->f either. A wrap already
 * ducking is cancelled, gain-continuous, so releasing cannot cause a jump. */
static void tp_exit(tp_t *p)
{
	p->in_loop = false;
	if (p->pend) {
		p->pend = false;
		st_seam_cancel(&p->seam);
	}
}

/* The boundary actually being approached: the window is periodic from lo, so
 * a playhead that starts past hi still wraps, on the grid. */
static uint32_t tp_end(const tp_t *p)
{
	uint32_t len;

	if (!p->in_loop || p->hi <= p->lo) {
		return 0u;
	}
	len = p->hi - p->lo;
	if (p->f >= p->hi) {
		return p->lo + ((p->f - p->lo) / len + 1u) * len;
	}
	return p->hi;
}

static void tp_frame(tp_t *p)
{
	const uint32_t end = tp_end(p);
	int ev = 0;

	/* Arm the wrap's duck ST_SEAM_FRAMES before the boundary. */
	if (p->in_loop && !p->pend && end > p->lo &&
	    p->f >= p->lo && p->f < end && p->f + ST_SEAM_FRAMES >= end) {
		st_seam_begin_in(&p->seam, (uint16_t)(end - p->f));
		p->pend = true;
		p->pend_to = p->lo;
	}

	/* Jump on the frame the gain actually reached zero. */
	if (p->pend && st_seam_jump_due(&p->seam)) {
		p->f = p->pend_to;
		p->pend = false;
		ev = 2;
	}

	if (g_n < MAXEM) {
		g_em[g_n].frame = p->f;
		g_em[g_n].event = ev;
		g_n++;
	}
	st_seam_tick(&p->seam);
	p->f++;
}

/* ---------------------------------------------------------------------- *
 * Sequence predicates, computed from the emitted frames alone.
 * ---------------------------------------------------------------------- */

/* Index of the first emitted frame that is not exactly one past its
 * predecessor, or -1 if the whole run is contiguous. */
static int first_discontinuity(uint32_t from, uint32_t to)
{
	uint32_t i;

	for (i = (from == 0u) ? 1u : from; i < to && i < g_n; i++) {
		if (g_em[i].frame != g_em[i - 1].frame + 1u) {
			return (int)i;
		}
	}
	return -1;
}

static uint32_t count_wraps(void)
{
	uint32_t i, n = 0;

	for (i = 0; i < g_n; i++) {
		if (g_em[i].event == 2) n++;
	}
	return n;
}

/* ======================================================================
 * 1. THE ACCEPTANCE TEST, in the user's own terms.
 *
 *    "for me no": engaging during "me" must not restart the syllable. The
 *    machine-checkable form is that the emitted frame sequence across the
 *    engage is strictly contiguous -- a restart is a backward jump, and a
 *    backward jump is a discontinuity.
 * ====================================================================== */
static void case_engaging_does_not_move_the_playhead(void)
{
	tp_t p;
	uint32_t i;
	const uint32_t lo = 4000u, len = 20000u;
	const uint32_t engage_after = 19200u;   /* ~400 ms hold at 48 kHz */
	uint32_t at_engage;
	int disc;

	g_cases++;
	printf("\n-- engaging the loop does not move the playhead\n");

	g_n = 0;
	tp_init(&p, lo);                        /* PLAY-DOWN happens at lo */
	for (i = 0; i < engage_after; i++) {    /* the hold plays through */
		tp_frame(&p);
	}
	at_engage = g_n;
	tp_enter(&p, lo, len);                  /* the gesture becomes a loop */
	/* Stop SHORT of the first boundary. This case is about the engage
	 * alone: running past loopEnd would bring in the wrap, which is a
	 * legitimate move and would muddy exactly the thing being asserted.
	 * The playhead is at lo+19200 and the boundary is at lo+20000, so 500
	 * frames stays inside the window with room to spare. */
	for (i = 0; i < 500u; i++) {
		tp_frame(&p);
	}

	CHECK(g_em[at_engage].frame == g_em[at_engage - 1].frame + 1u,
	      "the frame after the engage is the next one (%u -> %u), not a "
	      "jump back to loopStart",
	      g_em[at_engage - 1].frame, g_em[at_engage].frame);
	CHECK(g_em[at_engage].frame != lo,
	      "and specifically NOT loopStart %u -- that is the duplicated "
	      "syllable", lo);

	disc = first_discontinuity(0u, g_n);
	CHECK(disc < 0,
	      "the whole engage is contiguous; first break at emit %d "
	      "(%u -> %u)", disc,
	      disc > 0 ? g_em[disc - 1].frame : 0u,
	      disc > 0 ? g_em[disc].frame : 0u);
	CHECK(count_wraps() == 0u,
	      "and no wrap has happened yet -- the boundary is still ahead");
	printf("     held %u frames, engaged at source frame %u, played on to %u\n",
	       engage_after, g_em[at_engage].frame, g_em[g_n - 1].frame);
}

/* ======================================================================
 * 2. THE WRAP IS THE ONLY MOVE, and it lands exactly on loopStart.
 * ====================================================================== */
static void case_only_the_wrap_moves_the_playhead(void)
{
	tp_t p;
	uint32_t i, moves = 0, wraps = 0;
	const uint32_t lo = 4000u, len = 20000u;

	g_cases++;
	printf("\n-- the wrap is the only transport move, and it lands on loopStart\n");

	g_n = 0;
	tp_init(&p, lo);
	for (i = 0; i < 5000u; i++) tp_frame(&p);
	tp_enter(&p, lo, len);
	for (i = 0; i < len * 3u; i++) tp_frame(&p);
	tp_exit(&p);
	for (i = 0; i < 5000u; i++) tp_frame(&p);

	for (i = 1; i < g_n; i++) {
		if (g_em[i].frame != g_em[i - 1].frame + 1u) {
			moves++;
			CHECK(g_em[i].event == 2,
			      "the move at emit %u is a wrap, not something else", i);
			CHECK(g_em[i].frame == lo,
			      "and it lands on loopStart %u, not %u", lo,
			      g_em[i].frame);
			CHECK(g_em[i - 1].frame == p.hi - 1u ||
			      ((g_em[i - 1].frame - lo) % len) == len - 1u,
			      "leaving from the last frame of a lap (%u)",
			      g_em[i - 1].frame);
		}
	}
	wraps = count_wraps();
	CHECK(moves == wraps, "%u position changes, %u of them wraps", moves, wraps);
	CHECK(wraps >= 2u, "the loop actually wrapped (%u times)", wraps);
	printf("     %u wraps, %u total position changes -- they are the same set\n",
	       wraps, moves);
}

/* ======================================================================
 * 3. RELEASING NEVER MOVES THE PLAYHEAD, at any offset within a lap.
 *    Swept, because a release lands wherever the player's finger lands --
 *    including inside the wrap's duck window, which is the case that used
 *    to fire a jump.
 * ====================================================================== */
static void case_release_never_moves_the_playhead(void)
{
	const uint32_t lo = 4000u, len = 6000u;
	uint32_t off;
	uint32_t worst_off = 0;
	int worst = -1;

	g_cases++;
	printf("\n-- releasing never moves the playhead, swept over a whole lap\n");

	for (off = 0; off < len; off += 37u) {   /* 37: coprime with len */
		tp_t p;
		uint32_t i, at_exit;
		int disc;

		g_n = 0;
		tp_init(&p, lo);
		tp_enter(&p, lo, len);
		for (i = 0; i < len + off; i++) tp_frame(&p);
		at_exit = g_n;
		tp_exit(&p);
		for (i = 0; i < 2000u; i++) tp_frame(&p);

		/* From the release onward nothing may move. */
		disc = first_discontinuity(at_exit, g_n);
		if (disc >= 0 && worst < 0) {
			worst = disc;
			worst_off = off;
		}
		if (at_exit < g_n) {
			CHECK(g_em[at_exit].frame == g_em[at_exit - 1].frame + 1u,
			      "release at offset %u: %u -> %u is not contiguous",
			      off, g_em[at_exit - 1].frame, g_em[at_exit].frame);
		}
	}
	CHECK(worst < 0,
	      "no release produced a jump (first was at lap offset %u, emit %d)",
	      worst_off, worst);
	printf("     %u release points swept, none moved the transport\n",
	       (len + 36u) / 37u);
}

/* ======================================================================
 * 4. THE ENGAGE IS SWEPT OVER EVERY BLOCK OFFSET. The audio thread renders
 *    256-frame blocks, so a control request is seen at an arbitrary offset
 *    inside one. All 256 must behave identically.
 * ====================================================================== */
static void case_engage_at_every_block_offset(void)
{
	uint32_t off;
	int bad = 0;

	g_cases++;
	printf("\n-- engaging at all %u block offsets is contiguous every time\n",
	       BLK_FRAMES);

	for (off = 0; off < BLK_FRAMES; off++) {
		tp_t p;
		uint32_t i, at;

		g_n = 0;
		tp_init(&p, 4000u);
		for (i = 0; i < 3000u + off; i++) tp_frame(&p);
		at = g_n;
		tp_enter(&p, 4000u, 20000u);
		for (i = 0; i < 512u; i++) tp_frame(&p);
		if (first_discontinuity(at, g_n) >= 0) bad++;
	}
	CHECK(bad == 0, "%d of %u block offsets moved the playhead on engage",
	      bad, BLK_FRAMES);
}

/* ======================================================================
 * 5. A LOOP SHORTER THAN THE HOLD STILL WRAPS, ON THE GRID.
 *    Without an entry seek the playhead can begin outside the window
 *    entirely. If that were mishandled the loop would silently never
 *    engage -- a far worse failure than a click, because it looks like
 *    nothing happened.
 * ====================================================================== */
static void case_short_loop_still_wraps_on_the_grid(void)
{
	tp_t p;
	uint32_t i, wraps;
	const uint32_t lo = 4000u, len = 6000u;
	const uint32_t hold = 19200u;   /* three whole laps behind the playhead */
	int first = -1;

	g_cases++;
	printf("\n-- a loop shorter than the hold still wraps, on the musical grid\n");

	g_n = 0;
	tp_init(&p, lo);
	for (i = 0; i < hold; i++) tp_frame(&p);
	tp_enter(&p, lo, len);          /* playhead is already past lo+len */
	for (i = 0; i < len * 3u; i++) tp_frame(&p);

	wraps = count_wraps();
	CHECK(wraps >= 2u,
	      "a short loop still wraps (%u times) rather than silently doing "
	      "nothing", wraps);

	for (i = 1; i < g_n; i++) {
		if (g_em[i].event == 2) {
			if (first < 0) first = (int)i;
			CHECK(g_em[i].frame == lo, "every wrap lands on loopStart");
		}
	}
	CHECK(first > 0, "a wrap occurred");
	if (first > 0) {
		uint32_t last = g_em[first - 1].frame;

		CHECK(((last + 1u - lo) % len) == 0u,
		      "the first lap ends a whole number of loop lengths after "
		      "the capture (ended at %u, len %u)", last, len);
		printf("     first boundary at source frame %u = lo + %u lengths\n",
		       last + 1u, (last + 1u - lo) / len);
	}
}

/* ======================================================================
 * 6. NO FRAME IS EVER HEARD TWICE IN A ROW, and none is skipped, across a
 *    long run of laps. Duplicated transients and dropped frames are two of
 *    the named failure modes.
 * ====================================================================== */
static void case_no_duplicate_or_skipped_frames(void)
{
	tp_t p;
	uint32_t i, dup = 0, skip = 0;
	const uint32_t lo = 4000u, len = 9000u;

	g_cases++;
	printf("\n-- over many laps: no duplicated frame, none skipped\n");

	g_n = 0;
	tp_init(&p, lo);
	tp_enter(&p, lo, len);
	for (i = 0; i < len * 8u; i++) tp_frame(&p);

	for (i = 1; i < g_n; i++) {
		uint32_t a = g_em[i - 1].frame, b = g_em[i].frame;

		if (b == a) dup++;
		else if (b != a + 1u && !(b == lo && a == lo + len - 1u)) skip++;
	}
	CHECK(dup == 0, "%u frames were emitted twice in a row", dup);
	CHECK(skip == 0, "%u transitions were neither +1 nor the exact wrap", skip);
	CHECK(count_wraps() >= 7u, "the run really did lap (%u wraps)",
	      count_wraps());
	printf("     %u frames emitted, %u wraps, 0 duplicates, 0 skips\n",
	       g_n, count_wraps());
}

/* ======================================================================
 * 7. THE WRAP STILL DUCKS. Removing the entry and exit seeks must not have
 *    disarmed the one seam that remains: the gain has to be at zero on the
 *    frame the jump happens, or the wrap is a click.
 * ====================================================================== */
static void case_the_wrap_is_still_ducked(void)
{
	tp_t p;
	uint32_t i;
	const uint32_t lo = 4000u, len = 9000u;
	int checked = 0;

	g_cases++;
	printf("\n-- the wrap is still ducked to silence before it jumps\n");

	g_n = 0;
	tp_init(&p, lo);
	tp_enter(&p, lo, len);

	for (i = 0; i < len * 3u; i++) {
		uint32_t before = g_n;

		tp_frame(&p);
		if (g_n > before && g_em[g_n - 1].event == 2) {
			/* The jump was taken because the gain hit zero: that is
			 * st_seam_jump_due()'s definition, and tp_frame() has no
			 * other path to event 2. Assert the ramp really ran by
			 * checking a duck was in flight for the frames before. */
			CHECK(st_seam_active(&p.seam),
			      "a seam is in flight across the wrap");
			checked++;
		}
	}
	CHECK(checked >= 2, "observed %d ducked wraps", checked);
}

int main(void)
{
	printf("== Stem Tape LOOP TRANSPORT gate ==\n");
	printf("engage and release change the RULES, never the POSITION.\n");
	printf("Only crossing loopEnd may move the playhead.\n");
	load_fixture();
	printf("fixture: %u frames\n", g_fix_frames);

	case_engaging_does_not_move_the_playhead();
	case_only_the_wrap_moves_the_playhead();
	case_release_never_moves_the_playhead();
	case_engage_at_every_block_offset();
	case_short_loop_still_wraps_on_the_grid();
	case_no_duplicate_or_skipped_frames();
	case_the_wrap_is_still_ducked();

	printf("\n");
	if (g_failures) {
		printf("LOOP TRANSPORT GATE FAILED (%d cases, %d checks, %d failures)\n",
		       g_cases, g_checks, g_failures);
		return 1;
	}
	printf("LOOP TRANSPORT GATE PASSED (%d cases, %d checks, 0 failures)\n",
	       g_cases, g_checks);
	printf("NOTE: this proves the FRAME SEQUENCE. Whether it sounds seamless "
	       "on hardware is a separate question.\n");
	return 0;
}
