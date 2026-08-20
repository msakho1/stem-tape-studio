/*
 * test_stem_bufmbox.c — st_stem_bufmbox.c: the lock-free SPSC N-slot
 * sector ring, host-tested.
 *
 * Two layers:
 *   1. Single-threaded protocol tests: the sector->slot mapping, what the
 *      producer is and is not allowed to fill, and the two ways the ring
 *      recovers from a discontinuity (seek, loop wrap).
 *   2. REAL concurrent pthread producer/consumer tests: a producer
 *      thread and a consumer thread run the actual protocol against each
 *      other for thousands of sectors, with every buffer filled with a
 *      pattern that DETERMINISTICALLY encodes the sector index, and the
 *      consumer verifying EVERY byte matches the exact pattern for the
 *      sector it just acquired. That is what actually exercises the
 *      release/acquire visibility guarantee -- a stale, torn, or
 *      wrong-buffer read cannot pass by accident.
 *
 * The host build uses the C11 <stdatomic.h> backend with explicit
 * acquire/release -- the MINIMAL ordering the protocol claims to need --
 * so a failure here is a genuine violation of the documented requirement
 * rather than an artifact of a stronger default. The firmware build's
 * Zephyr atomic_t backend is unconditionally sequentially consistent,
 * i.e. strictly stronger, so anything passing here also holds there.
 *
 *     cc -std=c11 -Wall -Wextra -pthread \
 *        ../src/st_stem_bufmbox.c test_stem_bufmbox.c -o test_stem_bufmbox && \
 *        ./test_stem_bufmbox
 *
 * Supplemental TSan run:
 *     cc -std=c11 -Wall -Wextra -pthread -fsanitize=thread -g \
 *        ../src/st_stem_bufmbox.c test_stem_bufmbox.c -o test_stem_bufmbox_tsan && \
 *        ./test_stem_bufmbox_tsan
 */

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st_stem_bufmbox.h"

static int g_checks;
static int g_failures;
static int g_test_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { \
			printf("[OK  ] " __VA_ARGS__); \
			printf("\n"); \
		} else { \
			g_failures++; \
			printf("[FAIL] " __VA_ARGS__); \
			printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)
#define RUN(fn) do { g_test_cases++; fn(); } while (0)

#define SLOTS ST_STEM_MBOX_SLOTS
#define BIG_COUNT 10000u

/* Sector s lives in slot (s % SLOTS) -- the mapping both sides and every
 * test agree on, and the reason the consumer never searches. */
static void test_slot_mapping_is_modulo(void)
{
	CHECK(st_stem_mbox_slot_of(0u) == 0u, "sector 0 maps to slot 0");
	CHECK(st_stem_mbox_slot_of(SLOTS) == 0u, "sector SLOTS wraps back to slot 0");
	CHECK(st_stem_mbox_slot_of(SLOTS + 1u) == 1u, "sector SLOTS+1 maps to slot 1");
	CHECK(st_stem_mbox_slot_of(7u * SLOTS + 3u) == 3u % SLOTS,
	      "the mapping stays pure modulo far into the song");
}

/* init() makes exactly one sector resident, in its OWN mapped slot, and
 * marks it held -- matching a real boot where sector 0 is read
 * synchronously before either thread starts. */
static void test_init_publishes_only_the_initial_sector(void)
{
	st_stem_mbox_t mb;
	uint32_t slot = 0xffffu;

	st_stem_mbox_init(&mb, 0u);
	CHECK(st_stem_mbox_try_acquire(&mb, 0u, &slot), "the initial sector is immediately acquirable");
	CHECK(slot == 0u, "and it is in its own mapped slot");
	CHECK(!st_stem_mbox_try_acquire(&mb, 1u, &slot), "no other sector is resident yet");
	CHECK(st_stem_mbox_producer_requested_sector(&mb) == 0u,
	      "requested_sector starts at the initial sector");
}

/* init() at a non-zero sector (a real reload can start anywhere) must put
 * it in ITS OWN slot, not slot 0. */
static void test_init_at_nonzero_sector_uses_its_own_slot(void)
{
	st_stem_mbox_t mb;
	uint32_t slot = 0xffffu;
	uint32_t s = SLOTS + 1u;

	st_stem_mbox_init(&mb, s);
	CHECK(st_stem_mbox_try_acquire(&mb, s, &slot), "a non-zero initial sector is acquirable");
	CHECK(slot == st_stem_mbox_slot_of(s), "in its own mapped slot, not slot 0");
}

/* NEAREST GAP FIRST: a fill must always be spent on the sector the
 * consumer will reach soonest, never on speculative depth past a hole. */
static void test_producer_fills_nearest_gap_first(void)
{
	st_stem_mbox_t mb;
	uint32_t sec = 0xffffu, slot = 0xffffu;

	st_stem_mbox_init(&mb, 0u);
	CHECK(st_stem_mbox_producer_next_fill(&mb, BIG_COUNT, &sec, &slot),
	      "with only sector 0 resident there is work to do");
	CHECK(sec == 1u, "the first gap is sector 1, not something further ahead");
	CHECK(slot == st_stem_mbox_slot_of(1u), "and it is targeted at sector 1's own slot");

	st_stem_mbox_publish_ready(&mb, 1u, slot);
	CHECK(st_stem_mbox_producer_next_fill(&mb, BIG_COUNT, &sec, &slot) && sec == 2u,
	      "after sector 1 lands the next gap is sector 2");
}

/* THE WINDOW IS FULL: once every legal slot holds its sector, the
 * producer must report nothing to do rather than spin or re-fetch. */
static void test_producer_stops_when_window_is_full(void)
{
	st_stem_mbox_t mb;
	uint32_t sec, slot;
	uint32_t fills = 0;

	st_stem_mbox_init(&mb, 0u);
	while (st_stem_mbox_producer_next_fill(&mb, BIG_COUNT, &sec, &slot)) {
		st_stem_mbox_publish_ready(&mb, sec, slot);
		if (++fills > SLOTS * 4u) {
			break;   /* guard: a non-terminating producer is itself the bug */
		}
	}
	CHECK(fills == SLOTS - 1u,
	      "read-ahead saturates at exactly SLOTS-1 sectors (got %u, expected %u)",
	      fills, SLOTS - 1u);
	CHECK(!st_stem_mbox_producer_next_fill(&mb, BIG_COUNT, &sec, &slot),
	      "and then reports no work, rather than looping forever");
}

/* THE BUFFER BEING READ IS NEVER A TARGET. The consumer holds sector 0;
 * sector SLOTS maps to the same slot, so it must never be chosen even
 * once every other slot is full and it is the only "gap" left. */
static void test_producer_never_targets_the_held_slot(void)
{
	st_stem_mbox_t mb;
	uint32_t sec, slot;
	uint32_t held_slot;
	int i;

	st_stem_mbox_init(&mb, 0u);
	held_slot = st_stem_mbox_slot_of(0u);

	for (i = 0; i < 64; i++) {
		if (!st_stem_mbox_producer_next_fill(&mb, BIG_COUNT, &sec, &slot)) {
			break;
		}
		CHECK(slot != held_slot,
		      "fill %d targets slot %u, never the held slot %u", i, slot, held_slot);
		st_stem_mbox_publish_ready(&mb, sec, slot);
	}
}

/* ADVANCING FREES THE OLD BUFFER: acquiring sector 1 releases sector 0's
 * slot, which the producer may then reuse for sector SLOTS. */
static void test_acquire_releases_the_previous_slot(void)
{
	st_stem_mbox_t mb;
	uint32_t sec, slot, got;

	st_stem_mbox_init(&mb, 0u);
	while (st_stem_mbox_producer_next_fill(&mb, BIG_COUNT, &sec, &slot)) {
		st_stem_mbox_publish_ready(&mb, sec, slot);
	}
	CHECK(st_stem_mbox_try_acquire(&mb, 1u, &got), "the consumer advances to sector 1");

	st_stem_mbox_set_requested_sector(&mb, 2u);
	CHECK(st_stem_mbox_producer_next_fill(&mb, BIG_COUNT, &sec, &slot),
	      "sector 0's slot is now free, so there is work again");
	CHECK(sec == SLOTS, "and it is filled with sector SLOTS (%u)", SLOTS);
	CHECK(slot == st_stem_mbox_slot_of(0u), "reusing exactly the slot just released");
}

/* A SEEK IS SELF-CORRECTING: nothing detects it. The slot for the newly
 * needed sector simply holds something else, so acquire fails and the
 * producer refills it -- with no reset call anywhere. */
static void test_seek_needs_no_reset(void)
{
	st_stem_mbox_t mb;
	uint32_t sec, slot, got;
	uint32_t far = 5000u;

	st_stem_mbox_init(&mb, 0u);
	while (st_stem_mbox_producer_next_fill(&mb, BIG_COUNT, &sec, &slot)) {
		st_stem_mbox_publish_ready(&mb, sec, slot);
	}

	/* Consumer jumps far away and is now reading nothing. */
	st_stem_mbox_set_requested_sector(&mb, far);
	st_stem_mbox_release(&mb);
	CHECK(!st_stem_mbox_try_acquire(&mb, far, &got),
	      "the far sector is not resident, so acquire correctly fails");
	CHECK(st_stem_mbox_producer_next_fill(&mb, BIG_COUNT, &sec, &slot) && sec == far,
	      "and the producer's very next fill is that exact sector -- no reset needed");

	st_stem_mbox_publish_ready(&mb, far, slot);
	CHECK(st_stem_mbox_try_acquire(&mb, far, &got), "which the consumer then acquires");
}

/* READ-AHEAD CROSSES THE LOOP SEAM: at the end of a looping song the
 * window must wrap to sector 0 rather than stalling until the wrap
 * actually happens. */
static void test_readahead_wraps_at_the_loop_seam(void)
{
	st_stem_mbox_t mb;
	uint32_t count = SLOTS * 3u;      /* short song, so the seam is reachable */
	uint32_t sec, slot;
	bool saw_wrap = false;
	int i;

	st_stem_mbox_init(&mb, count - 1u);   /* consumer on the LAST sector */
	st_stem_mbox_set_requested_sector(&mb, count - 1u);

	for (i = 0; i < (int)SLOTS; i++) {
		if (!st_stem_mbox_producer_next_fill(&mb, count, &sec, &slot)) {
			break;
		}
		CHECK(sec < count, "a wrapped read-ahead sector (%u) stays inside the song", sec);
		if (sec == 0u) {
			saw_wrap = true;
		}
		st_stem_mbox_publish_ready(&mb, sec, slot);
	}
	CHECK(saw_wrap, "read-ahead reaches sector 0 across the loop seam");
}

/*
 * THE LOOP-WRAP STALL REGRESSION.
 *
 * When (sector_count % SLOTS) == 1, the song's LAST sector and sector 0
 * map to the SAME slot. At the wrap the consumer holds the last sector
 * and needs sector 0, so the producer -- which must never write the held
 * slot -- would refuse forever to fill the one sector the consumer is
 * waiting for, and the consumer would never acquire anything and so never
 * update held: a permanent stall, for those song lengths only.
 *
 * st_stem_mbox_release() is what breaks it: a consumer with nothing to
 * read holds nothing.
 */
static void test_loop_wrap_stall_is_broken_by_release(void)
{
	st_stem_mbox_t mb;
	uint32_t count = SLOTS * 4u + 1u;    /* count % SLOTS == 1: the pathological case */
	uint32_t last = count - 1u;
	uint32_t sec, slot, got;
	int i;

	CHECK(st_stem_mbox_slot_of(last) == st_stem_mbox_slot_of(0u),
	      "this song length really does collide the last sector with sector 0");

	st_stem_mbox_init(&mb, last);
	st_stem_mbox_set_requested_sector(&mb, 0u);

	/* Still holding `last`: sector 0's slot is off-limits, so the
	 * producer genuinely cannot supply it. */
	for (i = 0; i < (int)SLOTS * 2; i++) {
		if (!st_stem_mbox_producer_next_fill(&mb, count, &sec, &slot)) {
			break;
		}
		CHECK(sec != 0u, "while the colliding sector is held, sector 0 is not fillable");
		st_stem_mbox_publish_ready(&mb, sec, slot);
	}
	CHECK(!st_stem_mbox_try_acquire(&mb, 0u, &got), "so the consumer is stalled -- the bug");

	/* The consumer is reading nothing, and says so. */
	st_stem_mbox_release(&mb);
	CHECK(st_stem_mbox_producer_next_fill(&mb, count, &sec, &slot) && sec == 0u,
	      "after release the producer immediately picks sector 0");
	st_stem_mbox_publish_ready(&mb, sec, slot);
	CHECK(st_stem_mbox_try_acquire(&mb, 0u, &got), "and the consumer gets past the seam");
}

/* Defensive: a song of zero sectors, and a requested sector outside the
 * song, must both be refused rather than producing a wild slot index. */
static void test_out_of_range_inputs_are_refused(void)
{
	st_stem_mbox_t mb;
	uint32_t sec, slot;

	st_stem_mbox_init(&mb, 0u);
	CHECK(!st_stem_mbox_producer_next_fill(&mb, 0u, &sec, &slot),
	      "a zero-sector song yields no fill target");

	st_stem_mbox_set_requested_sector(&mb, 999u);
	CHECK(!st_stem_mbox_producer_next_fill(&mb, 10u, &sec, &slot),
	      "a requested sector past the end of the song yields no fill target");
}

static void test_requested_sector_roundtrip(void)
{
	st_stem_mbox_t mb;

	st_stem_mbox_init(&mb, 0u);
	CHECK(st_stem_mbox_producer_requested_sector(&mb) == 0u, "requested starts at 0");
	st_stem_mbox_set_requested_sector(&mb, 77u);
	CHECK(st_stem_mbox_producer_requested_sector(&mb) == 77u, "and round-trips exactly");
}

/* ========================================================================
 * Real concurrent pthread producer/consumer test.
 * ======================================================================== */

#define STREAM_SECTOR_COUNT 20000u
#define BUF_BYTES 64u

static uint8_t g_bufs[SLOTS][BUF_BYTES];
static st_stem_mbox_t g_mb;
static volatile int g_consumer_mismatch_at = -1;
static volatile uint32_t g_spins;

/* Deterministic per-sector fill: every byte depends on both the sector
 * index and its own position, so a stale/torn/wrong-buffer read is
 * overwhelmingly unlikely to accidentally match. */
static void fill_pattern(uint8_t *buf, uint32_t sector)
{
	for (uint32_t i = 0; i < BUF_BYTES; i++) {
		buf[i] = (uint8_t)((sector * 131u + i * 17u + 7u) & 0xffu);
	}
}

static bool pattern_matches(const uint8_t *buf, uint32_t sector)
{
	for (uint32_t i = 0; i < BUF_BYTES; i++) {
		uint8_t expect = (uint8_t)((sector * 131u + i * 17u + 7u) & 0xffu);

		if (buf[i] != expect) {
			return false;
		}
	}
	return true;
}

static void *producer_main(void *arg)
{
	(void)arg;
	for (;;) {
		uint32_t sec, slot;
		uint32_t spins = 0;

		/* This spin lives in the TEST DRIVER, standing in for
		 * streamer_thread's own per-pass polling; the mailbox API
		 * itself never loops. */
		while (!st_stem_mbox_producer_next_fill(&g_mb, STREAM_SECTOR_COUNT, &sec, &slot)) {
			if (__atomic_load_n(&g_consumer_mismatch_at, __ATOMIC_RELAXED) != -1) {
				return NULL;
			}
			if (st_stem_mbox_producer_requested_sector(&g_mb) >= STREAM_SECTOR_COUNT - 1u) {
				return NULL;
			}
			if ((++spins & 0xfffu) == 0u) {
				sched_yield();
			}
		}
		__atomic_add_fetch(&g_spins, spins, __ATOMIC_RELAXED);

		/* Plain (non-atomic) buffer writes BEFORE the release store
		 * -- exactly the ordering the visibility guarantee claims. */
		fill_pattern(g_bufs[slot], sec);
		st_stem_mbox_publish_ready(&g_mb, sec, slot);
	}
}

static void *consumer_main(void *arg)
{
	(void)arg;
	for (uint32_t sector = 1; sector < STREAM_SECTOR_COUNT; sector++) {
		uint32_t slot = 0xffffu;
		uint32_t spins = 0;

		st_stem_mbox_set_requested_sector(&g_mb, sector);
		while (!st_stem_mbox_try_acquire(&g_mb, sector, &slot)) {
			st_stem_mbox_release(&g_mb);
			if ((++spins & 0xfffu) == 0u) {
				sched_yield();
			}
		}
		__atomic_add_fetch(&g_spins, spins, __ATOMIC_RELAXED);

		if (!pattern_matches(g_bufs[slot], sector)) {
			__atomic_store_n(&g_consumer_mismatch_at, (int)sector, __ATOMIC_RELAXED);
			return NULL;
		}
	}
	return NULL;
}

static void test_concurrent_producer_consumer_real_threads(void)
{
	pthread_t producer, consumer;
	int rc_p, rc_c;
	uint32_t slot = 0xffffu;
	bool ok0;

	fill_pattern(g_bufs[0], 0u);
	st_stem_mbox_init(&g_mb, 0u);
	g_consumer_mismatch_at = -1;
	g_spins = 0;

	ok0 = st_stem_mbox_try_acquire(&g_mb, 0u, &slot);
	CHECK(ok0 && slot == 0u && pattern_matches(g_bufs[0], 0u),
	      "the ring's own initial sector 0 is immediately acquirable and content-correct");

	rc_p = pthread_create(&producer, NULL, producer_main, NULL);
	rc_c = pthread_create(&consumer, NULL, consumer_main, NULL);
	CHECK(rc_p == 0 && rc_c == 0, "both producer and consumer threads started");

	pthread_join(consumer, NULL);
	pthread_join(producer, NULL);

	CHECK(g_consumer_mismatch_at == -1,
	      "across %u real concurrent producer/consumer sectors the consumer NEVER observed content that "
	      "did not exactly match the sector it just acquired (mismatch_at=%d) -- proving buffer bytes are "
	      "genuinely visible before the slot publication is observed, under real thread interleaving",
	      STREAM_SECTOR_COUNT, g_consumer_mismatch_at);
	printf("      (driver-loop spin iterations across both threads: %u -- benign; the mailbox API itself "
	       "is wait-free)\n", (unsigned)g_spins);
}

int main(void)
{
	printf("ST_STEM_MBOX_SLOTS = %u (%u sectors of read-ahead)\n\n",
	       (unsigned)SLOTS, (unsigned)(SLOTS - 1u));

	RUN(test_slot_mapping_is_modulo);
	RUN(test_init_publishes_only_the_initial_sector);
	RUN(test_init_at_nonzero_sector_uses_its_own_slot);
	RUN(test_producer_fills_nearest_gap_first);
	RUN(test_producer_stops_when_window_is_full);
	RUN(test_producer_never_targets_the_held_slot);
	RUN(test_acquire_releases_the_previous_slot);
	RUN(test_seek_needs_no_reset);
	RUN(test_readahead_wraps_at_the_loop_seam);
	RUN(test_loop_wrap_stall_is_broken_by_release);
	RUN(test_out_of_range_inputs_are_refused);
	RUN(test_requested_sector_roundtrip);
	RUN(test_concurrent_producer_consumer_real_threads);

	printf("\nSTEM BUFMBOX TEST %s (%d test cases, %d checks, %d failures)\n",
	       g_failures == 0 ? "PASSED" : "FAILED", g_test_cases, g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
