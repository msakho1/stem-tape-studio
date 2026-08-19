/*
 * test_stem_bufmbox.c — st_stem_bufmbox.c: lock-free SPSC double-buffer
 * mailbox, host-tested (STEM TAPE Phase 2 continuous streaming, Slice
 * 3B.1 concurrency correction).
 *
 * Two kinds of coverage, deliberately kept separate:
 *   1. Single-threaded, deterministic tests of the protocol's own
 *      invariants (publish/consume, buffer-slot alternation, mismatch/
 *      underrun, and the explicit "attempted producer reuse before
 *      release" adversarial scenario) -- no raced timing to depend on,
 *      exact outcomes asserted directly.
 *   2. REAL concurrent pthread producer/consumer tests: a producer
 *      thread streams a long deterministic sequence of "sectors" into
 *      two small synthetic buffers (NOT claimed to be real audio
 *      content -- the same "hand-picked/synthetic values to hit a
 *      specific protocol case" allowance test_stem_mix.c's own header
 *      comment already establishes for pure numeric/protocol testing,
 *      as opposed to fabricating song content), each sector's buffer
 *      filled with a pattern that DETERMINISTICALLY encodes the sector
 *      index; a consumer thread acquires each published sector in turn
 *      and verifies EVERY byte matches the exact pattern for the sector
 *      it just acquired. Any visibility-ordering bug (buffer bytes not
 *      actually guaranteed visible before the ready flag, or a stale/
 *      torn read of the packed ready word) would show up here as a
 *      content mismatch or a hang -- not merely "the test finishes
 *      without crashing".
 *
 * Run twice by CI: once as a plain build (the primary proof: this
 * module's own algorithm, correct under the documented ordering
 * requirements), and once built with -fsanitize=thread (supplemental
 * evidence only, not the sole proof -- see this repo's own note on why:
 * TSan validates the C11 <stdatomic.h> HOST BACKEND's actual acquire/
 * release usage under real pthreads, which is a faithful stand-in for
 * the protocol's correctness, but does not itself exercise Zephyr's
 * atomic_t implementation, which the real firmware build uses instead
 * and which is unconditionally sequentially consistent -- strictly
 * stronger than what this protocol requires).
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

/* ========================================================================
 * Single-threaded, deterministic protocol tests.
 * ======================================================================== */

static void test_init_and_basic_publish_consume(void)
{
	st_stem_mbox_t mb;

	st_stem_mbox_init(&mb, 0u, 0u);

	CHECK(st_stem_mbox_producer_requested_sector(&mb) == 0u,
	      "init publishes requested_sector == the initial sector");
	CHECK(st_stem_mbox_producer_target_slot(&mb) == 1u,
	      "init leaves the complement buffer (1) as the producer's target slot");

	uint8_t got_buf = 0xffu;

	CHECK(st_stem_mbox_try_acquire(&mb, 0u, &got_buf) && got_buf == 0u,
	      "consumer can immediately acquire the initial published sector/buffer");

	/* Producer fills slot 1 with sector 1, publishes. */
	st_stem_mbox_publish_ready(&mb, 1u, 1u);
	got_buf = 0xffu;
	CHECK(st_stem_mbox_try_acquire(&mb, 1u, &got_buf) && got_buf == 1u,
	      "consumer acquires the freshly published sector 1 in buffer 1");
	CHECK(st_stem_mbox_producer_target_slot(&mb) == 0u,
	      "after the consumer adopts buffer 1, the producer's new target is the released buffer 0");
}

static void test_mismatch_returns_false_and_touches_nothing(void)
{
	st_stem_mbox_t mb;

	st_stem_mbox_init(&mb, 5u, 0u);

	uint8_t got_buf = 0xffu;
	bool ok = st_stem_mbox_try_acquire(&mb, 6u, &got_buf);

	CHECK(!ok, "try_acquire for a sector that was never published returns false");
	CHECK(got_buf == 0xffu, "try_acquire leaves *out_buf_idx untouched on failure");
	CHECK(st_stem_mbox_producer_target_slot(&mb) == 1u,
	      "a failed acquire does not move the consumer's slot -- target is still the complement of buf 0");
}

static void test_slot_alternation_over_many_publishes(void)
{
	st_stem_mbox_t mb;

	st_stem_mbox_init(&mb, 0u, 0u);

	uint8_t got_buf;
	bool ok = st_stem_mbox_try_acquire(&mb, 0u, &got_buf);

	CHECK(ok && got_buf == 0u, "adopts the initial sector 0 / buffer 0");

	bool alternates = true;

	for (uint32_t sector = 1u; sector <= 50u; sector++) {
		uint8_t target = st_stem_mbox_producer_target_slot(&mb);
		uint8_t expect = (uint8_t)(sector & 1u); /* strictly alternates 1,0,1,0,... from buf 0 */

		if (target != expect) {
			alternates = false;
		}
		st_stem_mbox_publish_ready(&mb, sector, target);
		got_buf = 0xffu;
		ok = st_stem_mbox_try_acquire(&mb, sector, &got_buf);
		if (!ok || got_buf != target) {
			alternates = false;
		}
	}
	CHECK(alternates, "across 50 sequential publish/acquire rounds, the producer's target slot strictly "
			   "alternates and always matches what the consumer actually acquires");
}

/*
 * Adversarial scenario the user explicitly asked for: the producer
 * publishes AGAIN before the consumer has ever acquired the first
 * publish. Proves the mailbox's own safety invariant holds regardless:
 * the producer's target slot can NEVER become the slot the consumer is
 * actually using, even under a (hypothetically buggy) over-eager
 * producer that ignores its own "is there already a matching publish"
 * check -- a second premature publish only ever overwrites the
 * producer's OWN still-unconsumed prior publish, never the consumer's
 * active buffer.
 */
static void test_attempted_producer_reuse_before_release(void)
{
	st_stem_mbox_t mb;

	st_stem_mbox_init(&mb, 0u, 0u); /* consumer implicitly "at" sector 0, buffer 0 */

	uint8_t target1 = st_stem_mbox_producer_target_slot(&mb);

	CHECK(target1 == 1u, "first target slot is the complement of the initial buffer (1)");

	/* Producer fills+publishes sector 1 into buffer 1 -- consumer has
	 * NOT acquired it yet. */
	st_stem_mbox_publish_ready(&mb, 1u, target1);

	/* Adversarial: producer (bug, or a legitimate re-check that finds
	 * nothing new to do) asks for its target slot AGAIN before any
	 * consumer acquire happened. */
	uint8_t target2 = st_stem_mbox_producer_target_slot(&mb);

	CHECK(target2 == target1,
	      "asking for the target slot again before the consumer has acquired anything returns the SAME "
	      "slot (1) -- never the consumer's actual active buffer (0)");

	/* If the producer were to publish AGAIN right now (a second,
	 * hypothetically premature fill), it can only overwrite its own
	 * unconsumed sector-1 publish -- prove that specifically: */
	st_stem_mbox_publish_ready(&mb, 2u, target2);
	uint8_t got_buf = 0xffu;
	bool ok_old = st_stem_mbox_try_acquire(&mb, 1u, &got_buf);

	CHECK(!ok_old, "the SUPERSEDED sector 1 publish is gone (overwritten by the adversarial re-publish) "
		       "-- expected, since nothing ever consumed it; this is a wasted-work outcome, not a "
		       "corruption outcome, because buffer 0 (the consumer's real active buffer) was never "
		       "touched by either premature publish");

	ok_old = st_stem_mbox_try_acquire(&mb, 2u, &got_buf);
	CHECK(ok_old && got_buf == target2, "the latest (sector 2) publish IS acquirable, in the same buffer");
	CHECK(st_stem_mbox_producer_target_slot(&mb) == 0u,
	      "only NOW, after the consumer's first-ever acquire, does the producer's target move to the "
	      "buffer (0) the consumer actually released");
}

static void test_requested_sector_roundtrip(void)
{
	st_stem_mbox_t mb;

	st_stem_mbox_init(&mb, 0u, 0u);
	st_stem_mbox_set_requested_sector(&mb, 77u);
	CHECK(st_stem_mbox_producer_requested_sector(&mb) == 77u,
	      "producer observes exactly the sector index the consumer published as requested");
}

/* ========================================================================
 * Real concurrent pthread producer/consumer test.
 * ======================================================================== */

#define STREAM_SECTOR_COUNT 20000u
#define BUF_BYTES 64u

static uint8_t g_bufs[2][BUF_BYTES];
static st_stem_mbox_t g_mb;
static volatile int g_consumer_mismatch_at = -1; /* set by the consumer thread on any content mismatch */
static volatile uint32_t g_underrun_spins;        /* diagnostic: total spin iterations either thread performed */

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
	for (uint32_t sector = 1; sector < STREAM_SECTOR_COUNT; sector++) {
		/* Wait (wait-free from the MAILBOX's own perspective -- this
		 * spin lives in the TEST DRIVER, standing in for streamer_
		 * thread's own real per-pass polling loop, not inside the
		 * mailbox API itself) until the consumer actually wants this
		 * sector -- mirrors the real production ordering constraint
		 * (song_frame frozen until the needed sector is ready, so the
		 * producer is never asked to race ahead of demand). */
		uint32_t spins = 0;

		while (st_stem_mbox_producer_requested_sector(&g_mb) != sector) {
			spins++;
			if ((spins & 0xfffu) == 0u) {
				sched_yield(); /* adversarial: maximize interleaving opportunities */
			}
		}
		__atomic_add_fetch(&g_underrun_spins, spins, __ATOMIC_RELAXED);

		uint8_t target = st_stem_mbox_producer_target_slot(&g_mb);

		fill_pattern(g_bufs[target], sector);
		st_stem_mbox_publish_ready(&g_mb, sector, target);
	}
	return NULL;
}

static void *consumer_main(void *arg)
{
	(void)arg;
	for (uint32_t sector = 1; sector < STREAM_SECTOR_COUNT; sector++) {
		st_stem_mbox_set_requested_sector(&g_mb, sector);

		uint8_t buf_idx = 0xffu;
		uint32_t spins = 0;

		while (!st_stem_mbox_try_acquire(&g_mb, sector, &buf_idx)) {
			spins++;
			if ((spins & 0xfffu) == 0u) {
				sched_yield();
			}
		}
		__atomic_add_fetch(&g_underrun_spins, spins, __ATOMIC_RELAXED);

		if (!pattern_matches(g_bufs[buf_idx], sector)) {
			g_consumer_mismatch_at = (int)sector;
			return NULL;
		}
	}
	return NULL;
}

static void test_concurrent_producer_consumer_real_threads(void)
{
	/* Sector 0 is the mailbox's own initial state -- fill buffer 0
	 * with sector 0's own pattern first so the consumer's very first
	 * (synchronous, pre-thread-start) read is also verifiable. */
	fill_pattern(g_bufs[0], 0u);
	st_stem_mbox_init(&g_mb, 0u, 0u);
	g_consumer_mismatch_at = -1;
	g_underrun_spins = 0;

	uint8_t buf_idx = 0xffu;
	bool ok0 = st_stem_mbox_try_acquire(&g_mb, 0u, &buf_idx);

	CHECK(ok0 && buf_idx == 0u && pattern_matches(g_bufs[0], 0u),
	      "the mailbox's own initial sector 0 is immediately acquirable and content-correct");

	pthread_t producer, consumer;
	int rc_p = pthread_create(&producer, NULL, producer_main, NULL);
	int rc_c = pthread_create(&consumer, NULL, consumer_main, NULL);

	CHECK(rc_p == 0 && rc_c == 0, "both producer and consumer threads started");

	pthread_join(producer, NULL);
	pthread_join(consumer, NULL);

	CHECK(g_consumer_mismatch_at == -1,
	      "across %u real concurrent producer/consumer sectors, the consumer NEVER observed content that "
	      "did not exactly match the sector it just acquired (mismatch_at=%d) -- proves buffer bytes are "
	      "genuinely visible before the ready flag is observed, under real thread interleaving",
	      STREAM_SECTOR_COUNT, g_consumer_mismatch_at);
	printf("      (spin iterations across both threads while polling: %u -- expected/benign, this test's "
	       "own driver loop, not the mailbox itself, which is wait-free)\n",
	       (unsigned)g_underrun_spins);
}

int main(void)
{
	RUN(test_init_and_basic_publish_consume);
	RUN(test_mismatch_returns_false_and_touches_nothing);
	RUN(test_slot_alternation_over_many_publishes);
	RUN(test_attempted_producer_reuse_before_release);
	RUN(test_requested_sector_roundtrip);
	RUN(test_concurrent_producer_consumer_real_threads);

	printf("\nSTEM BUFMBOX TEST %s (%d test cases, %d checks, %d failures)\n",
	       g_failures == 0 ? "PASSED" : "FAILED", g_test_cases, g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
