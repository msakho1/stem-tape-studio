/*
 * test_scratch_apply.c -- the audio thread's scratch application, compiled
 * from main.c's OWN SOURCE TEXT rather than reimplemented here.
 *
 * WHY THE BLOCK IS EXTRACTED INSTEAD OF COPIED. This is the one piece of the
 * scratch that only main.c contains, and it is the piece that decides which
 * heads move, in which direction, at what speed -- so a hand-written copy in a
 * test would be checking a second implementation and would go stale silently
 * the first time the real one changed. The harness lifts the exact lines
 * between the two markers below out of main.c at build time (see the CI step)
 * and compiles them against stubs for the two things a host cannot have: the
 * atomics and the four playheads.
 *
 * WHAT IT CANNOT PROVE: that the block sits in the right place in
 * stem_audio_block(), or that it runs once per block. Those are the Zephyr
 * build's and the wiring gate's business. What it proves is the arithmetic --
 * grab, drive, direction split, ownership, coast, handover -- which is where
 * the mistakes are.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "st_scratch.h"
#include "st_stem_stream.h"
#include "st_resample.h"

static int g_cases, g_checks, g_failures;
#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { printf("[OK  ] " __VA_ARGS__); printf("\n"); } \
		else { g_failures++; printf("[FAIL] " __VA_ARGS__); \
		       printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
	} while (0)
#define RUN(fn) do { g_cases++; printf("\n-- %s\n", #fn); fn(); } while (0)

/* ---- the stubs main.c's block needs ---------------------------------- */
typedef int32_t atomic_val_t;
typedef struct { atomic_val_t v; } atomic_t;
#define ATOMIC_INIT(x) { (x) }
static inline atomic_val_t atomic_get(const atomic_t *a) { return a->v; }
static inline void atomic_set(atomic_t *a, atomic_val_t v) { a->v = v; }

#define BLK_FRAMES 256
#define ST_SCR_T_MASTER 4u
#define ST_SCR_T_NONE   5u
#define ST_SCR_PACK(tgt, drive) \
	((atomic_val_t)(((uint32_t)(tgt) << 20) | ((uint32_t)(drive) & 0xFFFFFu)))
#define ST_SCR_TGT(v)   ((uint8_t)(((uint32_t)(v) >> 20) & 0xFu))
#define ST_SCR_DRIVE(v) ((int32_t)((uint32_t)(v) << 12) >> 12)
#define ST_SCR_BLOCK_US ((BLK_FRAMES * 1000000u) / ST11_SAMPLE_RATE_HZ)

static atomic_t g_stem_scratch_req =
	ATOMIC_INIT((atomic_val_t)(((uint32_t)ST_SCR_T_NONE << 20)));
static st_stream_t g_stem_stream[ST_PL_STEMS];
static uint8_t     s_stem_transport;

/* One block of the real application, lifted from main.c. */
static void apply_block(uint32_t rate_q16, uint32_t stem_rate_q16[ST_PL_STEMS])
{
	for (uint32_t rk = 0; rk < ST_PL_STEMS; rk++) {
		stem_rate_q16[rk] = rate_q16;
	}
#include "scratch_apply_block.inc"
}

/* ---- helpers ---------------------------------------------------------- */
#define SECTORS 600u
#define FRAMES  (SECTORS * ST11_FRAMES_PER_SECTOR)
#define BLOCKS  (SECTORS * ST11_BLOCKS_PER_SECTOR)

static void heads_init(void)
{
	uint32_t k;

	for (k = 0; k < ST_PL_STEMS; k++) {
		st_stream_init(&g_stem_stream[k], 0u, BLOCKS, FRAMES, SECTORS, false);
		st_stream_play(&g_stem_stream[k]);
		(void)st_stream_seek(&g_stem_stream[k], 300u * ST11_FRAMES_PER_SECTOR);
	}
	s_stem_transport = 0u;
	atomic_set(&g_stem_scratch_req, ST_SCR_PACK(ST_SCR_T_NONE, 0));
}

/* Run n blocks with a published gesture; return the last rate array. */
static void run_blocks(uint8_t tgt, int32_t drive, uint32_t n,
			uint32_t out[ST_PL_STEMS])
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		atomic_set(&g_stem_scratch_req, ST_SCR_PACK(tgt, drive));
		apply_block(ST_RS_ONE, out);
	}
}

/* The same, at an arbitrary transport rate -- the pitch rocker's domain. */
static void run_blocks_at(uint32_t rate, uint8_t tgt, int32_t drive, uint32_t n,
			   uint32_t out[ST_PL_STEMS])
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		atomic_set(&g_stem_scratch_req, ST_SCR_PACK(tgt, drive));
		apply_block(rate, out);
	}
}

/* ---- cases ------------------------------------------------------------ */

static void test_no_gesture_leaves_every_rate_alone(void)
{
	uint32_t r[ST_PL_STEMS];
	uint32_t k;

	heads_init();
	run_blocks(ST_SCR_T_NONE, 0, 8u, r);
	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(r[k] == ST_RS_ONE,
		      "A1. stem %u still runs at the transport rate (%u)", k, r[k]);
		CHECK(!g_stem_stream[k].reverse, "A1. and forward");
	}
}

static void test_master_moves_all_four_locked(void)
{
	uint32_t r[ST_PL_STEMS];
	uint32_t k;

	heads_init();
	run_blocks(ST_SCR_T_MASTER, ST_SCRATCH_DRIVE_FULL, 40u, r);

	for (k = 1; k < ST_PL_STEMS; k++) {
		CHECK(r[k] == r[0],
		      "A2. stem %u shares stem 0's rate exactly (%u vs %u) -- locked",
		      k, r[k], r[0]);
		CHECK(g_stem_stream[k].reverse == g_stem_stream[0].reverse,
		      "A2. and its direction");
	}
	CHECK(r[0] == ST_SCRATCH_MAX_RATE_MASTER_Q16,
	      "A2. a sustained forward hold shuttles to the master clamp (%u)", r[0]);
}

static void test_master_reverses_all_four_together(void)
{
	uint32_t r[ST_PL_STEMS];
	uint32_t k;

	heads_init();
	run_blocks(ST_SCR_T_MASTER, -ST_SCRATCH_DRIVE_FULL, 60u, r);

	for (k = 0; k < ST_PL_STEMS; k++) {
		CHECK(g_stem_stream[k].reverse,
		      "A3. stem %u is running backwards", k);
		CHECK(r[k] == ST_SCRATCH_MAX_RATE_MASTER_Q16,
		      "A3. at the clamp's MAGNITUDE (%u) -- the sign went to the head, "
		      "not to the rate", r[k]);
	}
}

static void test_one_stem_moves_and_three_do_not(void)
{
	uint32_t r[ST_PL_STEMS];
	uint32_t before[ST_PL_STEMS];
	uint32_t k;

	heads_init();
	for (k = 0; k < ST_PL_STEMS; k++) {
		before[k] = g_stem_stream[k].song_frame;
	}

	run_blocks(2u, -ST_SCRATCH_DRIVE_FULL, 40u, r);

	CHECK(g_stem_stream[2].reverse, "A4. stem 2 is reversed by its own gesture");
	CHECK(r[2] > ST_RS_ONE, "A4. and running faster than unity (%u)", r[2]);

	for (k = 0; k < ST_PL_STEMS; k++) {
		if (k == 2u) { continue; }
		CHECK(!g_stem_stream[k].reverse,
		      "A4. stem %u is untouched -- still forward", k);
		CHECK(r[k] == ST_RS_ONE,
		      "A4. and still at the transport rate (%u)", r[k]);
		CHECK(g_stem_stream[k].song_frame == before[k],
		      "A4. and this block moved it nowhere by itself");
	}
}

static void test_release_coasts_then_hands_back(void)
{
	uint32_t r[ST_PL_STEMS];
	uint32_t i;
	bool handed_back = false;

	heads_init();
	run_blocks(ST_SCR_T_MASTER, -ST_SCRATCH_DRIVE_FULL, 60u, r);
	CHECK(g_stem_stream[0].reverse && r[0] == ST_SCRATCH_MAX_RATE_MASTER_Q16,
	      "A5. shuttling in reverse at the clamp");

	/* The hand comes off: target NONE, and the coast runs. */
	for (i = 0; i < 200u; i++) {
		atomic_set(&g_stem_scratch_req, ST_SCR_PACK(ST_SCR_T_NONE, 0));
		apply_block(ST_RS_ONE, r);
		if (!g_stem_stream[0].reverse && r[0] == ST_RS_ONE) {
			handed_back = true;
			break;
		}
	}
	CHECK(handed_back,
	      "A5. the coast ends with every head forward at the transport rate again, "
	      "after %u blocks (%.0f ms)", i, i * ST_SCR_BLOCK_US / 1000.0);

	/* And it STAYS handed back -- the override must not reassert. */
	run_blocks(ST_SCR_T_NONE, 0, 20u, r);
	CHECK(r[0] == ST_RS_ONE && !g_stem_stream[0].reverse,
	      "A5. and stays there, so the transport owns its own rate again");
}

static void test_the_head_keeps_the_position_the_gesture_left(void)
{
	uint32_t r[ST_PL_STEMS];
	uint32_t i;

	heads_init();
	/* Reverse stem 1 a long way, then let go. */
	run_blocks(1u, -ST_SCRATCH_DRIVE_FULL, 30u, r);
	for (i = 0; i < 4000u; i++) {
		(void)st_stream_advance_frames(&g_stem_stream[1], 32u);
		st_stream_sector_ready(&g_stem_stream[1],
					st_stream_required_sector(&g_stem_stream[1]));
	}
	{
		const uint32_t left_at = g_stem_stream[1].song_frame;

		for (i = 0; i < 200u; i++) {
			atomic_set(&g_stem_scratch_req, ST_SCR_PACK(ST_SCR_T_NONE, 0));
			apply_block(ST_RS_ONE, r);
			if (!g_stem_stream[1].reverse && r[1] == ST_RS_ONE) { break; }
		}
		CHECK(g_stem_stream[1].song_frame == left_at,
		      "A6. releasing moved the head NOWHERE -- it resumes from where the "
		      "gesture left it (%u), not from where it started and not resynced",
		      g_stem_stream[1].song_frame);
	}
}

static void test_no_rate_ever_exceeds_the_renderer(void)
{
	uint32_t r[ST_PL_STEMS];
	uint32_t worst = 0u, k, i;

	heads_init();
	for (i = 0; i < 400u; i++) {
		const int32_t d = ((i / 7u) & 1u) ? -ST_SCRATCH_DRIVE_FULL
						   : ST_SCRATCH_DRIVE_FULL;

		atomic_set(&g_stem_scratch_req, ST_SCR_PACK(ST_SCR_T_MASTER, d));
		apply_block(ST_RS_ONE, r);
		for (k = 0; k < ST_PL_STEMS; k++) {
			if (r[k] > worst) { worst = r[k]; }
		}
	}
	CHECK(worst <= ST_RS_RATE_MAX,
	      "A7. 400 blocks of irregular scratching never hands the renderer more "
	      "than it can span (worst %u, max %u)", worst, (unsigned)ST_RS_RATE_MAX);
	CHECK(worst > 0u, "A7. and never hands it zero, which would stall the transport");
}

/*
 * SCENARIO 8: scratching from NON-UNITY VARISPEED.
 *
 * This is the case that found the bug. The coast walked to a hard-coded 1.0x,
 * so with the pitch rocker set -- the transport runs to 1.19x -- it finished
 * at unity, the override dropped, and the rate stepped 0.19x in a single
 * block. A 19% speed jump, audible as a pitch glitch, on every release while
 * pitched. The handover is seamless only if the coast ends on the number the
 * transport is about to use.
 */
static void test_scratch_from_pitched_playback_hands_back_exactly(void)
{
	const uint32_t pitched = 78000u;   /* ~1.19x, st_pitch's eMMC-capped max */
	uint32_t r[ST_PL_STEMS];
	uint32_t i;
	bool handed_back = false;

	heads_init();
	run_blocks_at(pitched, ST_SCR_T_MASTER, -ST_SCRATCH_DRIVE_FULL, 60u, r);
	CHECK(g_stem_stream[0].reverse,
	      "A8. a gesture started from pitched playback still reverses");

	for (i = 0; i < 300u; i++) {
		atomic_set(&g_stem_scratch_req, ST_SCR_PACK(ST_SCR_T_NONE, 0));
		apply_block(pitched, r);
		if (!g_stem_stream[0].reverse && r[0] == pitched) {
			handed_back = true;
			break;
		}
	}
	CHECK(handed_back,
	      "A8. and hands back at EXACTLY the transport's pitched rate (%u), not at "
	      "unity -- no step at the handover", pitched);

	/* The bug's signature, stated so a regression names itself: finishing at
	 * unity while the transport is pitched. */
	CHECK(r[0] != ST_RS_ONE || pitched == ST_RS_ONE,
	      "A8. specifically NOT 1.0x, which is where the first version stopped");
}

/*
 * SCENARIO: scratching a stem that was already REVERSE-TOGGLED.
 *
 * Reverse is a latch the player set deliberately. Coasting to a positive rate
 * would cancel it silently: reverse a stem, scratch it, let go, and find it
 * playing forward with nothing to explain it. Position is left where the
 * scratch put it; direction returns to the mode that was chosen.
 */
static void test_a_latched_reverse_survives_a_scratch(void)
{
	uint32_t r[ST_PL_STEMS];
	uint32_t i;

	heads_init();
	st_stream_set_reverse(&g_stem_stream[3], true);   /* the player's latch */

	run_blocks(3u, ST_SCRATCH_DRIVE_FULL, 40u, r);    /* scratch it FORWARD */
	CHECK(!g_stem_stream[3].reverse,
	      "A9. scratching forward really does drive it forward mid-gesture");

	/* The coast takes ~14 blocks; 100 is well past it, and a fixed count
	 * keeps the case from depending on state the block keeps to itself. */
	for (i = 0; i < 100u; i++) {
		atomic_set(&g_stem_scratch_req, ST_SCR_PACK(ST_SCR_T_NONE, 0));
		apply_block(ST_RS_ONE, r);
	}
	CHECK(g_stem_stream[3].reverse,
	      "A9. and letting go returns it to the REVERSE the player latched -- a "
	      "scratch moves the tape, it does not cancel a mode");
	CHECK(r[3] == ST_RS_ONE,
	      "A9. at the transport's own speed (%u)", r[3]);
}

/* SCENARIOS 2-5: every stem, not just the one that happened to be tested. */
static void test_each_stem_scratches_alone(void)
{
	static const char *NAMES[4] = { "vocal", "drums", "bass", "instrument" };
	uint32_t sk;

	for (sk = 0; sk < ST_PL_STEMS; sk++) {
		uint32_t r[ST_PL_STEMS];
		uint32_t k;
		bool others_clean = true;

		heads_init();
		run_blocks((uint8_t)sk, -ST_SCRATCH_DRIVE_FULL, 40u, r);

		for (k = 0; k < ST_PL_STEMS; k++) {
			if (k == sk) { continue; }
			if (g_stem_stream[k].reverse || r[k] != ST_RS_ONE) {
				others_clean = false;
			}
		}
		CHECK(g_stem_stream[sk].reverse && r[sk] > ST_RS_ONE && others_clean,
		      "A10. %s (stem %u) scratches alone; the other three are forward at "
		      "unity and undisturbed", NAMES[sk], sk);
	}
}

/* SCENARIO 11: repeated FUNCTION press/release, including re-grabbing before
 * the previous coast has finished -- which is what a player doing two scratches
 * in a row actually produces. */
static void test_repeated_press_release(void)
{
	uint32_t r[ST_PL_STEMS];
	uint32_t cycle;
	uint32_t worst = 0u;

	heads_init();
	for (cycle = 0; cycle < 12u; cycle++) {
		uint32_t i, k;

		run_blocks(ST_SCR_T_MASTER,
			    (cycle & 1u) ? -ST_SCRATCH_DRIVE_FULL : ST_SCRATCH_DRIVE_FULL,
			    9u, r);
		/* Let go for only THREE blocks -- far less than the coast needs. */
		for (i = 0; i < 3u; i++) {
			atomic_set(&g_stem_scratch_req, ST_SCR_PACK(ST_SCR_T_NONE, 0));
			apply_block(ST_RS_ONE, r);
		}
		for (k = 0; k < ST_PL_STEMS; k++) {
			if (r[k] > worst) { worst = r[k]; }
			if (r[k] != r[0]) {
				CHECK(false, "A11. heads diverged on cycle %u", cycle);
				return;
			}
		}
	}
	CHECK(worst <= ST_RS_RATE_MAX,
	      "A11. 12 press/release cycles with re-grabs mid-coast stay inside the "
	      "renderer's span (worst %u)", worst);
	CHECK(true, "A11. and all four heads stayed locked through every one");

	/* And a final full release still settles. */
	{
		uint32_t i;
		bool settled = false;

		for (i = 0; i < 300u; i++) {
			atomic_set(&g_stem_scratch_req, ST_SCR_PACK(ST_SCR_T_NONE, 0));
			apply_block(ST_RS_ONE, r);
			if (!g_stem_stream[0].reverse && r[0] == ST_RS_ONE) {
				settled = true;
				break;
			}
		}
		CHECK(settled, "A11. and the last release still settles back to normal play");
	}
}

int main(void)
{
	printf("== Stem Tape SCRATCH APPLIED (main.c's own block, on stubs) ==\n");
	RUN(test_no_gesture_leaves_every_rate_alone);
	RUN(test_master_moves_all_four_locked);
	RUN(test_master_reverses_all_four_together);
	RUN(test_one_stem_moves_and_three_do_not);
	RUN(test_release_coasts_then_hands_back);
	RUN(test_the_head_keeps_the_position_the_gesture_left);
	RUN(test_no_rate_ever_exceeds_the_renderer);
	RUN(test_scratch_from_pitched_playback_hands_back_exactly);
	RUN(test_a_latched_reverse_survives_a_scratch);
	RUN(test_each_stem_scratches_alone);
	RUN(test_repeated_press_release);

	printf("\n%d distinct test cases, %d assertion checks\n", g_cases, g_checks);
	if (g_failures) {
		printf("SCRATCH APPLY TEST FAILED (%d/%d checks failed)\n", g_failures, g_checks);
		return 1;
	}
	printf("SCRATCH APPLY TEST PASSED (%d test cases, %d checks)\n", g_cases, g_checks);
	return 0;
}
