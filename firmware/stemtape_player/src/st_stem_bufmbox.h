/*
 * st_stem_bufmbox.h — lock-free single-producer/single-consumer double-
 * buffer publication mailbox (STEM TAPE Phase 2 continuous streaming,
 * Slice 3B.1 concurrency correction).
 *
 * WHY THIS EXISTS: Slice 3B wired st_stem_stream.c's pure state machine
 * into two real threads by having BOTH of them mutate one shared
 * st_stream_t instance directly, relying on `volatile` fields plus a
 * DOCUMENTED write order (buffer index published before the ready flag)
 * for correctness. That is not a formally safe synchronization
 * primitive -- `volatile` only tells the compiler not to cache a value
 * across a sequence point; it says nothing about cross-thread visibility
 * ordering, and a "we always write A before B" convention has no
 * mechanism enforcing that the other thread cannot observe B before A
 * on hardware where a plain store is not immediately globally visible.
 * This module replaces that with a real, formally specified handoff:
 * every value that crosses the thread boundary is an atomic object, and
 * every ordering requirement is expressed as acquire/release pairing
 * (Zephyr's own atomic_t operations, used in the real firmware build,
 * are unconditionally sequentially consistent -- strictly STRONGER than
 * acquire/release, so they satisfy every ordering requirement this
 * module states; the host test build below uses C11 <stdatomic.h> with
 * EXPLICIT memory_order_acquire/memory_order_release, the minimal
 * ordering the protocol actually needs, so the tests exercise the real
 * requirement, not a stronger one that would hide a weaker bug).
 *
 * ROLES (exactly one thread each, for the whole lifetime of a mailbox):
 *   PRODUCER (real firmware: streamer_thread) -- the ONLY thread that
 *     ever fills or validates the free physical buffer, and the only
 *     thread that calls st_stem_mbox_publish_ready().
 *   CONSUMER (real firmware: audio_thread) -- the ONLY thread that ever
 *     decodes/reads buffer bytes for playback, and the only thread that
 *     calls st_stem_mbox_try_acquire() / st_stem_mbox_set_requested_
 *     sector().
 * Calling a "wrong side" function from the other role is a caller bug
 * this module does not defend against (matches every other single-
 * writer-per-field convention already used throughout this codebase).
 *
 * THE HANDOFF, exactly:
 *   - `ready_word` (producer writes, consumer reads): packs BOTH the
 *     ready sector index and which physical buffer slot (0/1) holds it
 *     into ONE atomic word, so the pair is always observed together,
 *     never torn -- there is no possible interleaving where a consumer
 *     could see a NEW sector index paired with an OLD buffer slot (or
 *     vice versa), because there is only ever one atomic load involved.
 *   - `requested_sector` (consumer writes, producer reads): which
 *     sector the consumer currently needs -- the producer's own
 *     "what should I fetch" input. Only the consumer can know this (it
 *     owns song position), so it must publish it.
 *   - `consumer_slot` (consumer writes, producer reads): which physical
 *     buffer slot the consumer is CURRENTLY reading from. This doubles
 *     as the release signal: the producer's only safe fill target is
 *     ALWAYS `1 - consumer_slot`, recomputed FRESH (never cached) every
 *     time it considers starting a fill -- so "the producer must not
 *     refill a buffer until the consumer has atomically released it" is
 *     satisfied structurally, not by a wait/poll loop: the producer can
 *     never even compute the wrong target, because the buffer the
 *     consumer is using can never be misidentified once consumer_slot is
 *     read. The consumer updates consumer_slot to the NEW slot the
 *     instant it adopts a freshly-published buffer (inside try_acquire,
 *     see below) -- which is exactly the same instant the OLD slot
 *     becomes safe for the producer to reuse, because a sequential
 *     single-position stream never needs to re-read a sector once it
 *     has moved past it (st_stream_advance_frame() freezes song_frame
 *     while the needed sector isn't ready, so the consumer can never
 *     race ahead of what the producer has actually published).
 *
 * VISIBILITY GUARANTEE ("buffer bytes and their metadata must become
 * visible before the ready flag is published"): the producer writes the
 * plain (non-atomic) buffer bytes and header fields BEFORE calling
 * st_stem_mbox_publish_ready(), which performs a RELEASE store of
 * `ready_word`. The consumer's st_stem_mbox_try_acquire() performs an
 * ACQUIRE load of the SAME `ready_word`. A release store
 * synchronizes-with an acquire load of the same atomic object that
 * observes the stored value (C11 5.1.2.4/Zephyr's own stronger SC
 * guarantee), which means every plain write the producer made before
 * its release store is guaranteed visible to the consumer after its
 * acquire load succeeds -- this is the formal mechanism, not merely a
 * convention, and it is exactly what the adversarial host tests in
 * tests/test_stem_bufmbox.c exercise under real concurrent pthreads
 * (writing a deterministic, per-sector byte pattern into the buffer and
 * verifying the consumer never observes anything other than the exact
 * pattern for the sector index it just acquired).
 *
 * NON-BLOCKING: every function here is wait-free (bounded number of
 * atomic operations, no loops that can spin on another thread, no
 * mutex, no allocation) -- the consumer side in particular must never
 * lock, block, or allocate, since it runs on the real-time audio thread.
 *
 * BACKEND: st_atomic32_t / st_atomic_get() / st_atomic_set() below map
 * to Zephyr's real atomic_t when ST_STEM_BUFMBOX_ZEPHYR is defined (set
 * by CMakeLists.txt for the real firmware target only), and to C11
 * <stdatomic.h> atomic_int with explicit acquire/release ordering
 * otherwise (the host test build, which has no Zephyr headers
 * available) -- SAME protocol, SAME ordering requirements, two
 * interchangeable backends for the same four primitive operations.
 */

#ifndef STEMTAPE_PLAYER_STEM_BUFMBOX_H_
#define STEMTAPE_PLAYER_STEM_BUFMBOX_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef ST_STEM_BUFMBOX_ZEPHYR
#include <zephyr/sys/atomic.h>

typedef atomic_t st_atomic32_t;
#define ST_ATOMIC32_INIT(v) ATOMIC_INIT(v)

/* Zephyr's atomic_get()/atomic_set() are unconditionally sequentially
 * consistent on every architecture Zephyr supports -- strictly stronger
 * than the acquire/release this module requires, so using them directly
 * satisfies every ordering requirement documented above. */
static inline int32_t st_atomic_get(const st_atomic32_t *a)
{
	return (int32_t)atomic_get(a);
}
static inline void st_atomic_set(st_atomic32_t *a, int32_t v)
{
	atomic_set(a, (atomic_val_t)v);
}
#else
#include <stdatomic.h>

typedef _Atomic int32_t st_atomic32_t;
#define ST_ATOMIC32_INIT(v) (v)

/* Host test backend: explicit acquire/release, the MINIMAL ordering the
 * protocol actually needs -- deliberately not seq_cst, so a test failure
 * here reflects a genuine violation of the documented requirement, not
 * an artifact of an over-strong default. */
static inline int32_t st_atomic_get(const st_atomic32_t *a)
{
	return atomic_load_explicit(a, memory_order_acquire);
}
static inline void st_atomic_set(st_atomic32_t *a, int32_t v)
{
	atomic_store_explicit(a, v, memory_order_release);
}
#endif

/* ready_word's value when no sector has ever been published (mailbox
 * freshly initialized only -- st_stem_mbox_init() always publishes an
 * initial ready sector, so in practice this sentinel is only ever
 * observed by tests exercising the primitive directly). */
#define ST_STEM_MBOX_NO_SECTOR (-1)

typedef struct {
	st_atomic32_t ready_word;        /* producer publishes: (sector_index << 1) | buf_idx */
	st_atomic32_t requested_sector;  /* consumer publishes: sector index it currently needs */
	st_atomic32_t consumer_slot;     /* consumer publishes: buf idx (0/1) it currently reads from */
} st_stem_mbox_t;

/*
 * Initializes the mailbox with an already-resident, already-adopted
 * initial sector -- matches a real boot sequence where the first
 * sector is read synchronously, before either thread's steady-state
 * loop starts, so there is no concurrent access to guard against here.
 */
void st_stem_mbox_init(st_stem_mbox_t *mb, uint32_t initial_sector, uint8_t initial_buf_idx);

/* ---- PRODUCER-side API (streamer_thread only) ---- */

/*
 * The ONLY buffer slot index (0 or 1) the producer may write into right
 * now. Always the complement of the consumer's own current slot,
 * recomputed fresh from a single acquire load -- see this header's own
 * doc comment for why this alone is sufficient to guarantee the
 * producer never refills a buffer the consumer has not yet released.
 */
uint8_t st_stem_mbox_producer_target_slot(const st_stem_mbox_t *mb);

/* Which sector index the consumer currently needs (its own last
 * published requested_sector). */
uint32_t st_stem_mbox_producer_requested_sector(const st_stem_mbox_t *mb);

/*
 * Publishes `buf_idx` (which MUST be st_stem_mbox_producer_target_
 * slot()'s own return value at the time the producer started filling
 * it -- see this header's own doc comment for why that remains valid
 * for the whole duration of one fill) as holding `sector_index`, fully
 * written and validated. Callers MUST perform every buffer-byte and
 * metadata write BEFORE calling this -- see this header's own
 * "VISIBILITY GUARANTEE" section for exactly what that buys.
 */
void st_stem_mbox_publish_ready(st_stem_mbox_t *mb, uint32_t sector_index, uint8_t buf_idx);

/* ---- CONSUMER-side API (audio_thread only) ---- */

/*
 * If the mailbox's currently published ready_word names exactly
 * `needed_sector`, writes its buffer slot index to *out_buf_idx,
 * atomically publishes THIS as the consumer's new current slot
 * (releasing whatever slot it previously held), and returns true.
 * Otherwise returns false and touches nothing. Wait-free: a single
 * acquire load, and (only on success) a single release store -- never
 * blocks, never loops.
 */
bool st_stem_mbox_try_acquire(st_stem_mbox_t *mb, uint32_t needed_sector, uint8_t *out_buf_idx);

/* Publishes a new "sector I currently need" for the producer to read.
 * Cheap to call every time the consumer's own need changes; callers
 * should avoid calling it when the value has not changed (not required
 * for correctness, just avoids a redundant atomic store). */
void st_stem_mbox_set_requested_sector(st_stem_mbox_t *mb, uint32_t sector_index);

#endif /* STEMTAPE_PLAYER_STEM_BUFMBOX_H_ */
