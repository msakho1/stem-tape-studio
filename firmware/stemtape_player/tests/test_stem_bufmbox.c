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
	if (SLOTS >= 3u) {
		CHECK(st_stem_mbox_producer_next_fill(&mb, BIG_COUNT, &sec, &slot) && sec == 2u,
		      "after sector 1 lands the next gap is sector 2");
	} else {
		/* At SLOTS==2 the ring holds exactly one sector of read-ahead,
		 * so publishing sector 1 already fills the window -- there is
		 * no sector 2 slot to put anything in. Asserting sector 2 here
		 * would be asserting a particular DEPTH rather than the
		 * nearest-gap-first RULE this case exists to pin. */
		CHECK(!st_stem_mbox_producer_next_fill(&mb, BIG_COUNT, &sec, &slot),
		      "at SLOTS==2 the single read-ahead slot is now full");
	}
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


/* ===================================================================
 * BATCHED FILL (song-planar v1.2): st_stem_mbox_producer_next_run().
 *
 * Song-planar gives every stem its own ring, so a span costs four fills
 * rather than one. Four separate reads per span is 5147 us against a 7083 us
 * span -- unaffordable -- so runs of consecutive groups are fetched in one
 * read. That is only ONE read while the run's slots are also consecutive,
 * which is the property these cases exist to pin down. Everything else about
 * the handoff is unchanged, and the cases above still cover it.
 * =================================================================== */

/* THE INVARIANT THE WHOLE BATCHING RESTS ON. A run is a single
 * emmc_read_blocks() only if its slots are consecutive with no wrap --
 * emmc_read_blocks() fills one contiguous buffer. Asserted DIRECTLY on the
 * slots rather than inferred from the alignment rule, because it is the
 * alignment rule that could be wrong. */
static void check_run_is_one_read(uint32_t first, uint32_t slot, uint32_t n,
				   const char *where)
{
	uint32_t k;

	CHECK(n >= 1u, "%s: a returned run is never empty (n=%u)", where, n);
	CHECK(st_stem_mbox_slot_of(first) == slot,
	      "%s: the reported slot is the first sector's own slot", where);
	for (k = 0; k < n; k++) {
		CHECK(st_stem_mbox_slot_of(first + k) == slot + k,
		      "%s: sector %u of the run sits at slot %u+%u -- consecutive, "
		      "so the run is ONE read (got %u)",
		      where, first + k, slot, k, st_stem_mbox_slot_of(first + k));
	}
	CHECK(slot + n <= SLOTS,
	      "%s: the run ends at or before the end of the ring (slot %u + n %u)",
	      where, slot, n);
}

/* A run that does not divide the ring is REFUSED, not quietly shortened.
 * At G=7/R=3 the batch sizes cycle 3,3,1 and one refill in three costs an
 * extra read -- a cost model silently different from the intended one, which
 * is exactly the bug that made this rule explicit. */
static void test_run_must_divide_the_ring(void)
{
	st_stem_mbox_t mb;
	uint32_t first, slot, n;
	uint32_t r;

	st_stem_mbox_init(&mb, 0u);

	CHECK(!st_stem_mbox_producer_next_run(&mb, 64u, 0u, &first, &slot, &n),
	      "a run of 0 is refused");
	for (r = 1u; r <= SLOTS + 2u; r++) {
		const bool divides = (SLOTS % r) == 0u;
		const bool got = st_stem_mbox_producer_next_run(&mb, 64u, r,
								 &first, &slot, &n);
		CHECK(got == divides,
		      "run=%u %s (SLOTS=%u %% %u = %u)",
		      r, divides ? "divides the ring and is accepted"
				  : "does not divide the ring and is REFUSED",
		      SLOTS, r, SLOTS % r);
		if (got) {
			check_run_is_one_read(first, slot, n, "divisor sweep");
			CHECK(n <= r, "run=%u never returns more than asked (n=%u)", r, n);
		}
	}
}

/* Steady state: once aligned, every run is the full R and lands in one read.
 * Driven through a whole ring's worth of refills so the wrap is crossed. */
static void test_runs_stay_aligned_and_never_wrap(void)
{
	const uint32_t R = 2u;                 /* the adopted refill size */
	const uint32_t COUNT = 64u;
	st_stem_mbox_t mb;
	uint32_t consumed = 0u;
	uint32_t guard = 0u;

	if ((SLOTS % R) != 0u) {
		CHECK(true, "ring depth %u is not a multiple of %u -- case skipped",
		      SLOTS, R);
		return;
	}

	st_stem_mbox_init(&mb, 0u);

	/* Walk the consumer forward, refilling in runs, for several laps of the
	 * ring. Every run must be one read, and no run may name a sector past
	 * the song's end. */
	while (consumed < COUNT - SLOTS && guard++ < 1000u) {
		uint32_t first, slot, n, k;

		while (st_stem_mbox_producer_next_run(&mb, COUNT, R,
						       &first, &slot, &n)) {
			check_run_is_one_read(first, slot, n, "steady state");
			CHECK(first + n <= COUNT,
			      "a run never names a sector past the song end "
			      "(first=%u n=%u count=%u)", first, n, COUNT);
			for (k = 0; k < n; k++) {
				st_stem_mbox_publish_ready(&mb, first + k,
							    st_stem_mbox_slot_of(first + k));
			}
		}

		{
			uint32_t got_slot;

			CHECK(st_stem_mbox_try_acquire(&mb, consumed, &got_slot),
			      "sector %u is resident when the consumer reaches it",
			      consumed);
		}
		consumed++;
		st_stem_mbox_set_requested_sector(&mb, consumed);
	}
	CHECK(guard < 1000u, "the refill/consume loop terminated");
}

/* A seek landing mid-block produces ONE short run to get aligned, and full
 * runs from then on. The short run is the price of alignment, paid once. */
static void test_a_seek_realigns_with_one_short_run(void)
{
	const uint32_t R = 2u;
	st_stem_mbox_t mb;
	uint32_t first, slot, n;

	if ((SLOTS % R) != 0u || SLOTS < 4u) {
		CHECK(true, "ring depth %u unsuitable for this case -- skipped", SLOTS);
		return;
	}

	st_stem_mbox_init(&mb, 0u);
	/* Land the consumer on an ODD sector: not a multiple of R. */
	st_stem_mbox_set_requested_sector(&mb, 11u);

	CHECK(st_stem_mbox_producer_next_run(&mb, 64u, R, &first, &slot, &n),
	      "there is something to fetch after a seek");
	CHECK(first == 11u, "the first fetch is the sector the consumer needs NOW");
	CHECK(n == 1u,
	      "an unaligned start yields a SHORT run (n=%u), so the next one is "
	      "aligned", n);
	check_run_is_one_read(first, slot, n, "post-seek short run");
	st_stem_mbox_publish_ready(&mb, first, slot);

	/* THE CONSUMER THEN ACQUIRES IT, which is the part that makes this a
	 * real seek rather than a frozen one. init() leaves sector 0 held, so
	 * without this the slot 0 stays forbidden forever and the sector that
	 * maps to it is skipped -- the producer would look wrong when in fact
	 * the scenario was. */
	{
		uint32_t acquired;

		CHECK(st_stem_mbox_try_acquire(&mb, 11u, &acquired),
		      "the consumer acquires the sector it seeked to");
	}

	CHECK(st_stem_mbox_producer_next_run(&mb, 64u, R, &first, &slot, &n),
	      "the next run follows");
	CHECK(first == 12u && (first % R) == 0u,
	      "and it starts aligned (first=%u)", first);
	CHECK(n == R, "so it is a FULL run (n=%u, R=%u)", n, R);
	check_run_is_one_read(first, slot, n, "first aligned run");
}

/* The run stops at the consumer's held slot, exactly as a single fill does.
 * A batch that overran it would refill the buffer being read. */
static void test_a_run_never_crosses_the_held_slot(void)
{
	const uint32_t R = 2u;
	const uint32_t COUNT = 64u;
	st_stem_mbox_t mb;
	uint32_t first, slot, n, k;
	uint32_t held_slot;
	uint32_t guard = 0u;

	if ((SLOTS % R) != 0u) {
		CHECK(true, "ring depth %u is not a multiple of %u -- skipped", SLOTS, R);
		return;
	}

	st_stem_mbox_init(&mb, 0u);
	CHECK(st_stem_mbox_try_acquire(&mb, 0u, &held_slot),
	      "the consumer acquires sector 0, so slot %u is now off-limits",
	      held_slot);

	while (st_stem_mbox_producer_next_run(&mb, COUNT, R, &first, &slot, &n) &&
	       guard++ < 100u) {
		check_run_is_one_read(first, slot, n, "with a slot held");
		for (k = 0; k < n; k++) {
			CHECK(st_stem_mbox_slot_of(first + k) != held_slot,
			      "no sector of any run targets the held slot "
			      "(sector %u -> slot %u, held %u)",
			      first + k, st_stem_mbox_slot_of(first + k), held_slot);
			st_stem_mbox_publish_ready(&mb, first + k,
						    st_stem_mbox_slot_of(first + k));
		}
	}
	CHECK(guard < 100u, "the refill loop terminated with a slot held");
}

/* A song shorter than one run, and a song shorter than the ring. Neither may
 * produce a run naming a sector that does not exist. */
static void test_short_songs_are_clamped(void)
{
	const uint32_t R = 2u;
	st_stem_mbox_t mb;
	uint32_t first, slot, n;
	uint32_t count;

	if ((SLOTS % R) != 0u) {
		CHECK(true, "ring depth %u is not a multiple of %u -- skipped", SLOTS, R);
		return;
	}

	for (count = 1u; count <= SLOTS + 1u; count++) {
		uint32_t guard = 0u;

		st_stem_mbox_init(&mb, 0u);
		while (st_stem_mbox_producer_next_run(&mb, count, R,
						       &first, &slot, &n) &&
		       guard++ < 100u) {
			uint32_t k;

			CHECK(first + n <= count,
			      "song of %u: run [%u,%u) stays inside the song",
			      count, first, first + n);
			check_run_is_one_read(first, slot, n, "short song");
			for (k = 0; k < n; k++) {
				st_stem_mbox_publish_ready(&mb, first + k,
							    st_stem_mbox_slot_of(first + k));
			}
		}
		CHECK(guard < 100u, "song of %u sectors: refill terminated", count);
	}

	CHECK(!st_stem_mbox_producer_next_run(&mb, 0u, R, &first, &slot, &n),
	      "a song of zero sectors has nothing to fetch");
}

/* The economy this exists for, as an executable claim rather than a comment:
 * batching really does cut the number of reads per span, and by the factor
 * the format document quotes. */
static void test_batching_cuts_reads_by_the_run_factor(void)
{
	const uint32_t COUNT = 96u;
	uint32_t reads[3];
	uint32_t idx = 0u;
	uint32_t R;

	for (R = 1u; R <= 2u; R++) {
		st_stem_mbox_t mb;
		uint32_t consumed = 0u;
		uint32_t nreads = 0u;
		uint32_t guard = 0u;

		if ((SLOTS % R) != 0u) {
			continue;
		}
		st_stem_mbox_init(&mb, 0u);
		while (consumed < COUNT - SLOTS && guard++ < 10000u) {
			uint32_t first, slot, n, k;

			while (st_stem_mbox_producer_next_run(&mb, COUNT, R,
							       &first, &slot, &n)) {
				nreads++;              /* ONE emmc_read_blocks() */
				for (k = 0; k < n; k++) {
					st_stem_mbox_publish_ready(
						&mb, first + k,
						st_stem_mbox_slot_of(first + k));
				}
			}
			consumed++;
			st_stem_mbox_set_requested_sector(&mb, consumed);
		}
		reads[idx++] = nreads;
		printf("      R=%u -> %u reads for %u sectors\n",
		       (unsigned)R, (unsigned)nreads, (unsigned)(COUNT - SLOTS));
	}

	if (idx == 2u) {
		CHECK(reads[1] < reads[0],
		      "R=2 needs FEWER reads than R=1 (%u vs %u) -- the whole "
		      "reason batching exists", reads[1], reads[0]);
		/* Not exactly half: the first and last runs of a song are
		 * short, and the window edges cost a few single fills. The
		 * claim worth pinning is the ORDER of the saving, which is
		 * what the us/span table in the format document rests on. */
		CHECK(reads[1] * 3u <= reads[0] * 2u,
		      "and at least a third fewer (%u vs %u) -- the saving the "
		      "us/span model rests on", reads[1], reads[0]);
	}
}


/* ---- The four cases below exist because mutation testing found the ones
 * above did not reach them. Each pins ONE guard in next_run(), in the exact
 * configuration where that guard is the only thing standing between the
 * producer and a wrong read. ---- */

/* SONG END. The alignment rule wants a full run; the last sector of an
 * ODD-length song cannot supply one. Without the song-end clamp the run names
 * a group past the end of the song region. Reached only once the consumer is
 * far enough in that the song end is nearer than the window end -- which is
 * why walking to the end matters, not just starting short. */
static void test_no_run_ever_names_a_sector_past_the_song(void)
{
	const uint32_t R = 2u;
	uint32_t count;
	uint32_t saw_clamped = 0u;

	if ((SLOTS % R) != 0u) {
		CHECK(true, "ring depth %u is not a multiple of %u -- skipped", SLOTS, R);
		return;
	}

	/* SWEPT, not a single length. Whether the song-end clamp is the guard
	 * that actually binds depends on where the last sector falls relative
	 * to the held slot and to what is already resident -- at some lengths
	 * another guard masks it entirely. One hand-picked length therefore
	 * proves nothing about the clamp; a sweep reaches the lengths where it
	 * is the only thing standing between the run and a group that does not
	 * exist. */
	for (count = SLOTS + 1u; count <= SLOTS * 3u; count++) {
		st_stem_mbox_t mb;
		uint32_t consumed = 0u;
		uint32_t guard = 0u;

		st_stem_mbox_init(&mb, 0u);
		while (consumed < count && guard++ < 500u) {
			uint32_t first, slot, n, k;

			while (st_stem_mbox_producer_next_run(&mb, count, R,
							       &first, &slot, &n)) {
				CHECK(first + n <= count,
				      "song of %u: run [%u,%u) stays inside it",
				      count, first, first + n);
				if (first + n == count && n < R) {
					saw_clamped++;
				}
				for (k = 0; k < n; k++) {
					st_stem_mbox_publish_ready(
						&mb, first + k,
						st_stem_mbox_slot_of(first + k));
				}
			}
			{
				uint32_t got;

				(void)st_stem_mbox_try_acquire(&mb, consumed, &got);
			}
			consumed++;
			st_stem_mbox_set_requested_sector(&mb, consumed);
		}
		CHECK(guard < 500u, "song of %u: the walk terminated", count);
	}
	CHECK(saw_clamped > 0u,
	      "the song's last run really was clamped short (%u times across the "
	      "sweep) -- so the clamp was exercised, not merely present",
	      saw_clamped);
}

/* HELD SLOT IN THE MIDDLE OF A RUN. Inside the window the held slot normally
 * holds the held sector, so the already-resident check masks the held-slot
 * check. They come apart at exactly one position: when the consumer has moved
 * its request on to h+1, sector h+SLOTS enters the far end of the window and
 * maps to the SAME slot as the sector still being read -- while holding a
 * different index, so it reads as fillable. Only the forbidden-slot test
 * stops the run there. */
static void test_a_run_stops_at_the_held_slot_even_when_it_looks_fillable(void)
{
	const uint32_t R = 2u;
	st_stem_mbox_t mb;
	uint32_t first, slot, n, k;
	uint32_t held_slot;
	uint32_t s;

	if ((SLOTS % R) != 0u || SLOTS < 4u || (SLOTS % 2u) != 0u) {
		CHECK(true, "ring depth %u unsuitable for this case -- skipped", SLOTS);
		return;
	}

	/* Hold an ODD sector, so that h+SLOTS-1 (the run's start) is even and
	 * the run's SECOND sector is the one landing on the held slot. */
	st_stem_mbox_init(&mb, 0u);
	st_stem_mbox_publish_ready(&mb, 1u, st_stem_mbox_slot_of(1u));
	CHECK(st_stem_mbox_try_acquire(&mb, 1u, &held_slot),
	      "the consumer is reading sector 1, in slot %u", held_slot);
	st_stem_mbox_set_requested_sector(&mb, 2u);
	for (s = 2u; s < SLOTS; s++) {
		st_stem_mbox_publish_ready(&mb, s, st_stem_mbox_slot_of(s));
	}

	/* The only gap left is sector SLOTS, whose run would reach SLOTS+1 --
	 * the same slot as the held sector 1, holding a different index. */
	if (st_stem_mbox_producer_next_run(&mb, 64u, R, &first, &slot, &n)) {
		for (k = 0; k < n; k++) {
			CHECK(st_stem_mbox_slot_of(first + k) != held_slot,
			      "run [%u,%u): sector %u does NOT land on the held "
			      "slot %u", first, first + n, first + k, held_slot);
		}
	} else {
		CHECK(true,
		      "the run is deferred rather than allowed to reach the held "
		      "slot -- also correct");
	}
}

/* ALREADY RESIDENT. A run must not re-cover a sector it already has: the read
 * is pure waste, and the whole point of batching is fewer reads. */
static void test_a_run_never_refetches_what_it_already_has(void)
{
	const uint32_t R = 2u;
	const uint32_t COUNT = 64u;
	st_stem_mbox_t mb;
	uint32_t first, slot, n, k;
	uint32_t guard = 0u;
	bool published[64] = { false };

	if ((SLOTS % R) != 0u || SLOTS < 4u) {
		CHECK(true, "ring depth %u unsuitable -- skipped", SLOTS);
		return;
	}

	st_stem_mbox_init(&mb, 0u);
	published[0] = true;
	/* Punch a hole: publish an ODD sector, leaving the even one before it
	 * missing, so a full run would straddle the resident one. */
	st_stem_mbox_publish_ready(&mb, 3u, st_stem_mbox_slot_of(3u));
	published[3] = true;

	while (st_stem_mbox_producer_next_run(&mb, COUNT, R, &first, &slot, &n) &&
	       guard++ < 50u) {
		for (k = 0; k < n; k++) {
			CHECK(!published[first + k],
			      "run [%u,%u) does not re-cover sector %u, which is "
			      "already resident", first, first + n, first + k);
			published[first + k] = true;
			st_stem_mbox_publish_ready(&mb, first + k,
						    st_stem_mbox_slot_of(first + k));
		}
	}
	CHECK(guard < 50u, "the hole-filling loop terminated");
}

/* URGENCY BEATS BATCHING. The sector the consumer needs RIGHT NOW is fetched
 * whatever its run length. Without that exemption a truncated urgent run is
 * deferred for a batch that will never form, and the consumer stalls on a
 * sector it is waiting for -- which is audible, where a short read is not. */
static void test_the_urgent_sector_is_never_deferred_for_a_batch(void)
{
	const uint32_t R = 2u;
	st_stem_mbox_t mb;
	uint32_t first, slot, n;

	if ((SLOTS % R) != 0u || SLOTS < 4u) {
		CHECK(true, "ring depth %u unsuitable -- skipped", SLOTS);
		return;
	}

	st_stem_mbox_init(&mb, 0u);
	/* The consumer needs an EVEN sector, and the odd one after it -- the
	 * rest of its would-be run -- is already resident. So the run truncates
	 * to a single group and does NOT end on a run boundary. */
	st_stem_mbox_publish_ready(&mb, 11u, st_stem_mbox_slot_of(11u));
	st_stem_mbox_set_requested_sector(&mb, 10u);

	CHECK(st_stem_mbox_producer_next_run(&mb, 64u, R, &first, &slot, &n),
	      "the sector the consumer needs NOW is fetched even though its run "
	      "is short and unaligned");
	CHECK(first == 10u, "and it is that exact sector (got %u)", first);
	CHECK(n == 1u, "as a single group (n=%u), because 11 is already resident", n);
	CHECK(((first + n) % R) != 0u,
	      "and the run genuinely does NOT end on a run boundary (%u %% %u = "
	      "%u) -- so the urgency exemption is what allowed it, not the "
	      "alignment rule", first + n, R, (first + n) % R);
}


/* AND THE SAME CLAMP, REACHED THE ONLY WAY IT CAN BE. Walking a song from the
 * start never exposes it: the producer runs far enough ahead that the last
 * sector is already resident by the time the consumer is close enough for the
 * song's end to be nearer than the window's. A SEEK is what creates the
 * configuration -- the consumer lands near the end with nothing prefetched, so
 * the final aligned run is the one that would overrun.
 *
 * Verified against the real mutant: with the clamp removed this yields
 * [8,10) on a 9-sector song, i.e. a read of a group that does not exist. */
static void test_a_seek_near_the_end_cannot_overrun_the_song(void)
{
	const uint32_t R = 2u;
	const uint32_t COUNT = 9u;             /* odd, so the last run is short */
	st_stem_mbox_t mb;
	uint32_t first, slot, n, k;
	uint32_t guard = 0u;
	uint32_t saw_last = 0u;

	if ((SLOTS % R) != 0u || SLOTS < 4u) {
		CHECK(true, "ring depth %u unsuitable -- skipped", SLOTS);
		return;
	}

	st_stem_mbox_init(&mb, 0u);
	st_stem_mbox_set_requested_sector(&mb, COUNT - 4u);   /* seek near the end */

	while (st_stem_mbox_producer_next_run(&mb, COUNT, R, &first, &slot, &n) &&
	       guard++ < 50u) {
		CHECK(first + n <= COUNT,
		      "after a seek to %u in a %u-sector song, run [%u,%u) stays "
		      "inside it", COUNT - 4u, COUNT, first, first + n);
		if (first + n == COUNT) {
			saw_last++;
		}
		for (k = 0; k < n; k++) {
			st_stem_mbox_publish_ready(&mb, first + k,
						    st_stem_mbox_slot_of(first + k));
		}
	}
	CHECK(guard < 50u, "the post-seek refill terminated");
	CHECK(saw_last > 0u,
	      "and the run that ends exactly at the song's last sector was "
	      "actually reached -- otherwise this case proves nothing");
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

	RUN(test_run_must_divide_the_ring);
	RUN(test_runs_stay_aligned_and_never_wrap);
	RUN(test_a_seek_realigns_with_one_short_run);
	RUN(test_a_run_never_crosses_the_held_slot);
	RUN(test_short_songs_are_clamped);
	RUN(test_batching_cuts_reads_by_the_run_factor);
	RUN(test_no_run_ever_names_a_sector_past_the_song);
	RUN(test_a_seek_near_the_end_cannot_overrun_the_song);
	RUN(test_a_run_stops_at_the_held_slot_even_when_it_looks_fillable);
	RUN(test_a_run_never_refetches_what_it_already_has);
	RUN(test_the_urgent_sector_is_never_deferred_for_a_batch);

	printf("\nSTEM BUFMBOX TEST %s (%d test cases, %d checks, %d failures)\n",
	       g_failures == 0 ? "PASSED" : "FAILED", g_test_cases, g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
