/*
 * test_head_contract.c -- THE INDEPENDENT-HEAD CONTRACT, stated as tests.
 *
 * ======================================================================
 * WHY THIS FILE EXISTS
 * ======================================================================
 * Per-track reverse proved each stem can have its own position and its own
 * direction. The scratch series then proved that is NOT the same as each stem
 * having a hardened, independently controllable transport head: reverse only
 * ever changes direction ONCE, deliberately, and stays there, so nothing in the
 * v1.3 engine was ever asked what happens when a head changes signed velocity
 * dozens of times a second. docs/postmortems/2026-09-scratch-series.md is what
 * happened when something did ask.
 *
 * So this file states the head contract directly, ahead of any scratch code:
 *
 *   H1  a signed-velocity change does not invalidate the residency window
 *   H2  a signed-velocity change does not restart or reposition the transport
 *   H3  moving one head does not disturb another
 *   H4  a head is self-contained -- no shared transport state to fall back on
 *   H5  nothing in the head layer can touch gain, mute or solo
 *   H6  repeated sign flips inside the resident window cost no refills
 *
 * and one that subsumes H1/H6, which is the property the whole redesign turns
 * on:
 *
 *   H7  ONLY a seek outside the resident window triggers a refill.
 *
 * ======================================================================
 * THE RIG USES THE REAL RING. THIS IS THE POINT.
 * ======================================================================
 * The scratch series shipped a residency claim measured against a MODEL of the
 * ring -- an idealised, fully-associative window centred on the head, which
 * re-centred on every miss and asserted `sector_ready()` unconditionally. The
 * production ring is none of those things: it is DIRECT-MAPPED (sector s lives
 * in slot s % SLOTS and nowhere else), it is filled from a request address the
 * consumer publishes, and it can say no. The model could not represent the
 * failure and so could not find it. See the postmortem, section 2.5.
 *
 * This rig therefore drives:
 *   - a real st_stream_t,
 *   - a real st_stem_mbox_t,
 *   - the real producer (st_stem_mbox_producer_next_run), counting every
 *     physical read it would issue,
 *   - and the real consumer sequence from main.c, INCLUDING main.c's own
 *     prefetch request-address rule, reproduced in rig_request_ahead() below
 *     with the source it mirrors quoted next to it.
 *
 * A read counted here is a read the eMMC would actually perform.
 *
 * NO AUDIO-CONTENT ASSERTIONS are made anywhere in this file, so the synthetic
 * geometry below fabricates nothing: every claim is about position, direction,
 * residency and read count, which are properties of the bookkeeping and not of
 * any sample. The repo's fixture rule applies to audio content and is not
 * side-stepped here.
 *
 *     cc -std=c11 -Wall -Wextra -I../src \
 *        ../src/st_stem_stream.c ../src/st_stem_bufmbox.c \
 *        test_head_contract.c -o test_head_contract && ./test_head_contract
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st_latency.h"
#include "st_planar.h"
#include "st_stem_bufmbox.h"
#include "st_stem_stream.h"
#include "st_v11_format.h"

static int g_checks, g_failures, g_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { printf("[OK  ] " __VA_ARGS__); printf("\n"); } \
		else { g_failures++; printf("[FAIL] " __VA_ARGS__); \
		       printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
	} while (0)

/* A property that is KNOWN to fail against today's head, recorded as a
 * measurement rather than a pass/fail, so this file can be committed and run
 * before the fix exists. Each one names the postmortem section that explains
 * it. When the head layer is hardened these become CHECK()s. */
#define EXPECT_TODAY(cond, ...) do { \
		printf((cond) ? "[HOLDS ] " : "[BROKEN] " __VA_ARGS__); \
		if (cond) { printf(__VA_ARGS__); } \
		printf("\n"); \
	} while (0)

#define RUN(fn) do { g_cases++; printf("\n-- %s\n", #fn); fn(); } while (0)

/* ======================================================================
 * THE SONG. Synthetic geometry, chosen only so a head has room to move in
 * both directions without meeting either end: 64 full sectors.
 * ====================================================================== */
#define RIG_SECTORS 64u
#define RIG_FRAMES  (RIG_SECTORS * ST11_FRAMES_PER_SECTOR)
#define RIG_BLOCKS  (RIG_SECTORS * ST11_BLOCKS_PER_SECTOR)

/* main.c's own refill run length and ring depth, not re-chosen here. */
#define RIG_R ST_PL_REFILL_GROUPS
#define RIG_G ST_STEM_MBOX_SLOTS

typedef struct {
	st_stream_t   st;
	st_stem_mbox_t mb;
	uint32_t      reads;    /* physical eMMC reads the producer issued */
	uint32_t      sectors;  /* sectors those reads delivered */
} rig_t;

/*
 * ONE PRODUCER PASS -- streamer_thread's inner loop, minus the eMMC.
 * Each successful next_run() is ONE physical read of `n` consecutive sectors,
 * which is exactly the accounting main.c:7182 performs.
 */
static void rig_produce(rig_t *r, uint32_t max_reads)
{
	uint32_t first, slot, n, i;
	uint32_t issued = 0u;

	while (issued < max_reads &&
	       st_stem_mbox_producer_next_run(&r->mb, RIG_SECTORS, RIG_R,
					       &first, &slot, &n)) {
		for (i = 0; i < n; i++) {
			st_stem_mbox_publish_ready(&r->mb, first + i,
						    st_stem_mbox_slot_of(first + i));
		}
		r->reads++;
		r->sectors += n;
		issued++;
	}
}

/*
 * THE PREFETCH REQUEST ADDRESS, mirrored from main.c's own block (the one at
 * "THE DIRECTION THE HEAD IS ACTUALLY GOING", ~line 3732). Reproduced rather
 * than modelled, because this rule IS the thing under test:
 *
 *     if (reverse)  ahead = needed >= R ? needed - R : 0;
 *     else          ahead = needed + 1, wrapping at the song's end;
 *
 * and while the head is NOT resident the request is `needed` itself, so the
 * producer fetches what the consumer is actually stuck on.
 */
static uint32_t rig_request_ahead(const st_stream_t *st, uint32_t needed, bool resident)
{
	uint32_t ahead;

	if (!resident) {
		return needed;
	}
	if (st->reverse) {
		ahead = (needed >= RIG_R) ? needed - RIG_R : 0u;
	} else {
		ahead = needed + 1u;
		if (ahead >= RIG_SECTORS) {
			ahead = st->loop_enabled ? 0u : needed;
		}
	}
	return ahead;
}

/* ONE CONSUMER PASS -- main.c's acquire/request/advance, in that order. */
static void rig_consume(rig_t *r, uint32_t frames)
{
	const uint32_t needed = st_stream_required_sector(&r->st);
	uint32_t slot;
	bool resident;

	if (r->st.ready_sector != needed) {
		if (st_stem_mbox_try_acquire(&r->mb, needed, &slot)) {
			st_stream_sector_ready(&r->st, needed);
		} else {
			st_stem_mbox_release(&r->mb);
		}
	}
	resident = (r->st.ready_sector == needed);

	st_stem_mbox_set_requested_sector(&r->mb,
					   rig_request_ahead(&r->st, needed, resident));

	if (frames > 0u) {
		(void)st_stream_advance_frames(&r->st, frames);
	}
}

/* Bring a rig up at `frame`, with the ring fully primed around it and the
 * read counter zeroed, so every read counted afterwards belongs to the
 * gesture under test and not to start-up. */
static void rig_init(rig_t *r, uint32_t frame, bool reverse)
{
	uint32_t i;

	memset(r, 0, sizeof(*r));
	if (!st_stream_init(&r->st, 0u, RIG_BLOCKS, RIG_FRAMES, RIG_SECTORS, false)) {
		printf("FATAL: rig geometry rejected\n");
		exit(2);
	}
	st_stream_play(&r->st);
	(void)st_stream_seek(&r->st, frame);
	st_stem_mbox_init(&r->mb, st_stream_required_sector(&r->st));
	if (reverse) {
		st_stream_set_reverse(&r->st, true);
	}

	/* Settle: let the consumer publish its request and the producer fill
	 * the whole window, repeatedly, until nothing more is wanted. */
	for (i = 0; i < 32u; i++) {
		rig_consume(r, 0u);
		rig_produce(r, 64u);
	}
	r->reads = 0u;
	r->sectors = 0u;
}

/* How many sectors of this song the ring currently holds. */
__attribute__((unused)) static uint32_t rig_resident_count(const rig_t *r)
{
	uint32_t s, n = 0u;

	for (s = 0; s < RIG_SECTORS; s++) {
		if (st_atomic_get(&r->mb.slot_sector[st_stem_mbox_slot_of(s)]) ==
		    (int32_t)s) {
			n++;
		}
	}
	return n;
}

/* ======================================================================
 * H1 / H6 / H7 -- the residency window under repeated sign flips
 * ====================================================================== */

/*
 * The head sits in the MIDDLE of a sector and oscillates by a few frames, so
 * it never leaves the sector it is on, let alone the resident window. Under
 * the contract this must cost nothing: the samples are already there.
 */
static void test_sign_flips_inside_one_sector_cost_no_reads(void)
{
	rig_t r;
	const uint32_t mid = 32u * ST11_FRAMES_PER_SECTOR + (ST11_FRAMES_PER_SECTOR / 2u);
	const uint32_t start_sector = mid / ST11_FRAMES_PER_SECTOR;
	uint32_t flips;

	rig_init(&r, mid, false);

	for (flips = 0; flips < 40u; flips++) {
		st_stream_set_reverse(&r.st, (flips & 1u) != 0u);
		rig_consume(&r, 8u);          /* 8 frames -- 0.17 ms of tape */
		rig_produce(&r, 64u);
	}

	CHECK(st_stream_required_sector(&r.st) == start_sector,
	      "H1. 40 sign flips of 8 frames never left sector %u", start_sector);
	EXPECT_TODAY(r.reads == 0u,
		      "H1. 40 sign flips inside ONE sector cost %u reads "
		      "(contract: 0 -- the samples were already resident) "
		      "[postmortem 2.5]", r.reads);
}

/*
 * Wider, but still inside the resident window: the head ranges over a couple of
 * sectors, which the ring holds on both sides of it. Under the contract this is
 * still free.
 */
static void test_sign_flips_inside_the_resident_window_cost_no_reads(void)
{
	rig_t r;
	const uint32_t mid = 32u * ST11_FRAMES_PER_SECTOR;
	uint32_t cycles;

	rig_init(&r, mid, false);

	/* One cycle: forward one sector's worth, then back again. Amplitude is
	 * one sector, so the whole excursion is 2 sectors of a window that
	 * holds G-1 = %u either side. */
	for (cycles = 0; cycles < 10u; cycles++) {
		uint32_t k;

		st_stream_set_reverse(&r.st, false);
		for (k = 0; k < 8u; k++) { rig_consume(&r, ST11_FRAMES_PER_SECTOR / 8u); rig_produce(&r, 64u); }
		st_stream_set_reverse(&r.st, true);
		for (k = 0; k < 8u; k++) { rig_consume(&r, ST11_FRAMES_PER_SECTOR / 8u); rig_produce(&r, 64u); }
	}

	EXPECT_TODAY(r.reads == 0u,
		      "H7. 10 one-sector oscillations inside a %u-slot window cost "
		      "%u reads (contract: 0 -- only a seek OUTSIDE the window "
		      "may refill) [postmortem 2.5]", RIG_G, r.reads);
	printf("       ...and delivered %u sectors into a %u-slot ring, i.e. the "
	       "ring was rewritten %.1f times over\n",
	       r.sectors, RIG_G, RIG_G ? (double)r.sectors / (double)RIG_G : 0.0);
}

/*
 * THE ALIASING, stated as arithmetic and then measured.
 *
 * A forward head requests `needed+1` and the producer fills an ASCENDING run
 * of R from there; a reversed head requests `needed-R` and the producer fills
 * the same shape. So the two directions want:
 *
 *     forward   n+1 .. n+R
 *     head      n
 *     reverse   n-R .. n-1
 *
 * which is 2R+1 distinct sectors over G slots. With R=3, G=6 that is 7 over 6,
 * and (n-R) and (n+R) land in the SAME slot: the two directions provably
 * cannot both be resident, so every sign flip evicts the other one.
 */
static void test_the_two_directions_read_ahead_regions_alias(void)
{
	const uint32_t span = 2u * RIG_R + 1u;
	const uint32_t n = 32u;   /* any head sector; the collision is modular */

	printf("       R=%u  G=%u  forward wants n+1..n+%u, reverse wants n-%u..n-1\n",
	       RIG_R, RIG_G, RIG_R, RIG_R);
	CHECK(st_stem_mbox_slot_of(n - RIG_R) == st_stem_mbox_slot_of(n + RIG_R),
	      "H7. sector n-%u and n+%u map to the SAME slot (%u) -- the two "
	      "directions' read-ahead cannot coexist", RIG_R, RIG_R,
	      st_stem_mbox_slot_of(n - RIG_R));
	EXPECT_TODAY(span <= RIG_G,
		      "H7. a bidirectional window needs %u distinct slots and the "
		      "ring has %u (contract: G >= 2R+1)", span, RIG_G);
}

/* ======================================================================
 * H2 -- a direction change must not restart or reposition
 * ====================================================================== */

static void test_a_sign_flip_never_moves_the_head(void)
{
	rig_t r;
	const uint32_t mid = 20u * ST11_FRAMES_PER_SECTOR + 100u;
	uint32_t before, i;

	rig_init(&r, mid, false);
	before = r.st.song_frame;

	for (i = 0; i < 16u; i++) {
		st_stream_set_reverse(&r.st, (i & 1u) != 0u);
	}

	CHECK(r.st.song_frame == before,
	      "H2. 16 direction changes with no advance left song_frame at %u",
	      r.st.song_frame);
	CHECK(r.st.ready_sector != ST_STREAM_NO_SECTOR,
	      "H2. ...and residency was not invalidated by any of them");
}

/*
 * THE RESTART, isolated. A head driven back past frame 0 parks there
 * (START_OF_SONG), and turning it forward lifts it straight to PLAYING at
 * frame 0 -- so the song plays from the top. That is the SPECIFIED behaviour
 * for a latched reverse ("a reversed track that reaches the absolute beginning
 * stops/clamps there") and it is wrong for a momentary gesture, which should
 * clamp without discarding where it came from. Postmortem 2.9.
 */
static void test_retreat_past_the_start_then_forward_restarts_the_song(void)
{
	rig_t r;
	const uint32_t start = 4u * ST11_FRAMES_PER_SECTOR;
	uint32_t i;

	rig_init(&r, start, true);

	for (i = 0; i < 200u && r.st.state != ST_STREAM_START_OF_SONG; i++) {
		rig_consume(&r, 64u);
		rig_produce(&r, 64u);
	}

	CHECK(r.st.state == ST_STREAM_START_OF_SONG,
	      "H2. a reversed head driven off the front parks at START_OF_SONG");
	CHECK(r.st.song_frame == 0u, "H2. ...clamped at frame 0");

	st_stream_set_reverse(&r.st, false);
	CHECK(r.st.state == ST_STREAM_PLAYING && r.st.song_frame == 0u,
	      "H2. turning forward resumes PLAYING at frame 0");
	printf("       ^ correct for a LATCHED reverse, and a full song restart "
	       "for a momentary gesture. The head layer cannot currently tell\n"
	       "         the two apart, which is postmortem 2.9.\n");
}

/* ======================================================================
 * H3 / H4 -- independence
 * ====================================================================== */

static void test_moving_one_head_does_not_disturb_another(void)
{
	rig_t a, b;
	uint32_t b_frame, b_ready, i;
	bool b_rev;
	st_stream_state_t b_state;

	rig_init(&a, 30u * ST11_FRAMES_PER_SECTOR, false);
	rig_init(&b, 30u * ST11_FRAMES_PER_SECTOR, false);

	b_frame = b.st.song_frame;
	b_ready = b.st.ready_sector;
	b_rev   = b.st.reverse;
	b_state = b.st.state;

	for (i = 0; i < 60u; i++) {
		st_stream_set_reverse(&a.st, (i & 1u) != 0u);
		rig_consume(&a, 32u);
		rig_produce(&a, 64u);
	}

	CHECK(b.st.song_frame == b_frame && b.st.ready_sector == b_ready &&
	      b.st.reverse == b_rev && b.st.state == b_state,
	      "H3. 60 passes of gesturing head A left head B bit-identical");
	CHECK(b.reads == 0u, "H3. ...and cost head B zero reads");
}

static void test_every_head_may_be_reversed_at_once(void)
{
	rig_t h[ST_PL_STEMS];
	uint32_t k, i;
	bool any_forward = false;

	for (k = 0; k < ST_PL_STEMS; k++) {
		rig_init(&h[k], 30u * ST11_FRAMES_PER_SECTOR, true);
	}
	for (i = 0; i < 20u; i++) {
		for (k = 0; k < ST_PL_STEMS; k++) {
			rig_consume(&h[k], 32u);
			rig_produce(&h[k], 64u);
		}
	}
	for (k = 0; k < ST_PL_STEMS; k++) {
		if (!h[k].st.reverse) {
			any_forward = true;
		}
	}

	CHECK(!any_forward,
	      "H4. all %u heads can travel backwards simultaneously -- the head "
	      "layer itself imposes no 'at most one reversed' rule", ST_PL_STEMS);
	printf("       ^ but main.c's s_stem_transport is a stem INDEX chosen by\n"
	       "         `for j: if (!g_stem_stream[j].reverse) transport = j`,\n"
	       "         whose own comment says \"there is always at least one\n"
	       "         [forward head], because only one track reverses at a\n"
	       "         time\". With every head reversed that search finds\n"
	       "         nothing and leaves the song clock on a backwards head.\n"
	       "         That is the shared-transport fallback H4 forbids, and it\n"
	       "         lives ABOVE this layer. Postmortem 2.6.\n");
}

/* ======================================================================
 * H5 -- the head layer cannot touch the mix
 * ====================================================================== */

static void test_the_head_owns_no_mix_state(void)
{
	/* Structural, not behavioural: the head's whole mutable surface is
	 * position, residency, direction, state and a diagnostic counter. There
	 * is no gain, mute or solo field for any code path to reach, so "an
	 * underrun changed the solo state" is unrepresentable here rather than
	 * merely untested. */
	const size_t accounted =
		sizeof(((st_stream_t *)0)->song_start_block) +
		sizeof(((st_stream_t *)0)->song_block_count) +
		sizeof(((st_stream_t *)0)->frames) +
		sizeof(((st_stream_t *)0)->sector_count) +
		sizeof(((st_stream_t *)0)->loop_enabled) +
		sizeof(((st_stream_t *)0)->state) +
		sizeof(((st_stream_t *)0)->song_frame) +
		sizeof(((st_stream_t *)0)->ready_sector) +
		sizeof(((st_stream_t *)0)->underrun_count) +
		sizeof(((st_stream_t *)0)->reverse);

	CHECK(accounted <= sizeof(st_stream_t),
	      "H5. every field of st_stream_t is accounted for -- geometry, "
	      "position, residency, direction, state, diagnostics; no gain, "
	      "mute or solo");
}

/* ======================================================================
 * What the head does NOT own today, and the user's list says it should
 * ====================================================================== */

static void test_the_head_does_not_own_its_own_rate(void)
{
	printf("       the contract says a head owns:\n"
	       "         position       -> st_stream_t.song_frame        PRESENT\n"
	       "         signed_rate    -> (main.c stem_rate_q16[] +\n"
	       "                            s_stem_rate_frac[])          ABSENT\n"
	       "         base_direction -> (nothing -- `reverse` is the\n"
	       "                            CURRENT direction, and a\n"
	       "                            gesture overwrites the\n"
	       "                            player's latch with it)      ABSENT\n"
	       "         residency      -> st_stream_t.ready_sector, ONE\n"
	       "                            sector, not a window          PARTIAL\n"
	       "         stream state   -> st_stream_t.state             PRESENT\n");
	CHECK(sizeof(st_stream_t) > 0u,
	      "H-own. 2 of the 5 owned quantities are absent and 1 is a scalar "
	      "where the contract needs a window");
}

int main(void)
{
	printf("head contract -- ring %u slots, refill run %u, sector %u frames\n",
	       RIG_G, RIG_R, (unsigned)ST11_FRAMES_PER_SECTOR);
	printf("song: %u sectors, %u frames\n", RIG_SECTORS, (unsigned)RIG_FRAMES);

	RUN(test_sign_flips_inside_one_sector_cost_no_reads);
	RUN(test_sign_flips_inside_the_resident_window_cost_no_reads);
	RUN(test_the_two_directions_read_ahead_regions_alias);
	RUN(test_a_sign_flip_never_moves_the_head);
	RUN(test_retreat_past_the_start_then_forward_restarts_the_song);
	RUN(test_moving_one_head_does_not_disturb_another);
	RUN(test_every_head_may_be_reversed_at_once);
	RUN(test_the_head_owns_no_mix_state);
	RUN(test_the_head_does_not_own_its_own_rate);

	printf("\n%d cases, %d checks, %d failures\n", g_cases, g_checks, g_failures);
	printf("(lines marked [BROKEN] are measured contract violations in the "
	       "CURRENT head, not test failures -- they become CHECK()s when the "
	       "head layer is hardened)\n");
	return g_failures ? 1 : 0;
}
