/*
 * st_stem_bufmbox.h — lock-free single-producer/single-consumer N-slot
 * sector ring for STEM TAPE continuous streaming.
 *
 * WHY THIS EXISTS: the first version of this handoff was two `volatile`
 * fields plus a DOCUMENTED write order, which is not a synchronization
 * primitive at all -- `volatile` says nothing about cross-thread
 * visibility ordering, and "we always write A before B" has no mechanism
 * enforcing that the other thread cannot observe B before A. That was
 * replaced by a formally specified atomic handoff, which this file still
 * is: every value crossing the thread boundary is an atomic object and
 * every ordering requirement is expressed as acquire/release pairing.
 *
 * WHY N SLOTS AND NOT TWO: the double-buffer version could hold at most
 * ONE sector of read-ahead. A sector is 7.08 ms of audio and a worst-case
 * eMMC read measured 16.1 ms on real hardware, so the design had
 * PERMANENTLY NEGATIVE margin -- the producer could not, even in
 * principle, stay ahead of a single slow read. The consumer then stalls,
 * and because st_stream_advance_frame() freezes song_frame while the
 * needed sector is missing, a stall does not drop audio, it TIME-STRETCHES
 * it: the song plays at whatever fraction of real time the card can
 * sustain. That is what a starving stream actually sounds like, and no
 * amount of correctness elsewhere fixes it. N slots give (N-1) sectors of
 * read-ahead so ordinary read jitter is absorbed instead of becoming
 * audible.
 *
 * ROLES (exactly one thread each, for the whole lifetime of a mailbox):
 *   PRODUCER (real firmware: streamer_thread) -- the ONLY thread that
 *     ever fills or validates a buffer, and the only caller of
 *     st_stem_mbox_producer_next_fill() / st_stem_mbox_publish_ready().
 *   CONSUMER (real firmware: audio_thread) -- the ONLY thread that ever
 *     decodes/reads buffer bytes, and the only caller of
 *     st_stem_mbox_try_acquire() / st_stem_mbox_set_requested_sector().
 * Calling a "wrong side" function from the other role is a caller bug
 * this module does not defend against (matches every other single-
 * writer-per-field convention already used throughout this codebase).
 *
 * THE MAPPING: sector s always lives in slot (s % ST_STEM_MBOX_SLOTS).
 * This is what keeps the consumer wait-free: to find out whether the one
 * sector it needs is resident it computes a single index and does a
 * single atomic load -- never a scan, never a search. It is also what
 * makes a seek or a loop wrap self-correcting: after a discontinuity the
 * slot for the newly needed sector simply holds some other sector index,
 * so try_acquire() fails, and the producer refills it. Nothing has to
 * detect the discontinuity or reset anything.
 *
 * THE HANDOFF, exactly:
 *   - `slot_sector[i]` (producer writes, consumer reads): which sector
 *     index slot i currently holds, or ST_STEM_MBOX_NO_SECTOR. Because
 *     the sector index IS the published value (not a separate flag), a
 *     consumer that observes `needed` in the slot it computed for
 *     `needed` has, by construction, observed the right buffer -- there
 *     is no index/flag pair that could be seen half-updated.
 *   - `requested_sector` (consumer writes, producer reads): the sector
 *     the consumer currently needs. Only the consumer knows this (it owns
 *     song position), so it must publish it; it is the producer's
 *     starting point for deciding what to fetch.
 *   - `held_sector` (consumer writes, producer reads): the sector the
 *     consumer most recently ACQUIRED and is therefore reading bytes out
 *     of right now, or ST_STEM_MBOX_NO_SECTOR before the first acquire.
 *     This is the release signal: the producer must never write the slot
 *     that maps to held_sector, and it recomputes that forbidden slot
 *     FRESH (never cached) every time it picks a target. "Do not refill a
 *     buffer the consumer has not released" is therefore satisfied
 *     structurally rather than by any wait or poll -- and a STALE read of
 *     held_sector is always safe, because held_sector only ever moves
 *     forward, so an older value can only make the producer more
 *     conservative, never less.
 *
 * WHY `held` AND `requested` ARE SEPARATE: they differ at exactly the
 * moment that matters. Immediately after a seek the consumer needs sector
 * R but holds nothing, and the producer must be free to fill R itself. If
 * one field served both roles the producer would treat R as forbidden and
 * deadlock, each side waiting for the other.
 *
 * VISIBILITY GUARANTEE ("buffer bytes must become visible before the slot
 * is published"): the producer writes the plain (non-atomic) buffer bytes
 * BEFORE calling st_stem_mbox_publish_ready(), which performs a RELEASE
 * store of that slot. The consumer's st_stem_mbox_try_acquire() performs
 * an ACQUIRE load of the SAME slot. A release store synchronizes-with an
 * acquire load of the same atomic object that observes the stored value
 * (C11 5.1.2.4 / Zephyr's own stronger SC guarantee), so every plain
 * write the producer made before its release store is visible to the
 * consumer once its acquire load succeeds. This is the formal mechanism,
 * not a convention, and it is exactly what the adversarial concurrent-
 * pthread host tests in tests/test_stem_bufmbox.c exercise.
 *
 * NON-BLOCKING: every function here is wait-free (bounded atomic
 * operations, no loop that can spin on another thread, no mutex, no
 * allocation). The consumer side in particular runs on the real-time
 * audio thread and must never lock, block or allocate.
 *
 * BACKEND: st_atomic32_t / st_atomic_get() / st_atomic_set() map to
 * Zephyr's real atomic_t when ST_STEM_BUFMBOX_ZEPHYR is defined (set by
 * CMakeLists.txt for the firmware target only), and to C11 <stdatomic.h>
 * with EXPLICIT acquire/release otherwise (the host test build) -- SAME
 * protocol, SAME ordering requirements. The host backend deliberately
 * uses the minimal ordering the protocol actually needs, not seq_cst, so
 * a test failure reflects a genuine violation rather than an artifact of
 * an over-strong default.
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

/*
 * How many physical sector buffers the ring owns, and therefore how many
 * sectors of read-ahead exist: (SLOTS - 1), because the slot mapping to
 * the sector the consumer currently holds is never a legal fill target.
 *
 * MUST be the length of the buffer array the caller passes around (see
 * main.c's g_stem_bufs[]) -- a BUILD_ASSERT there ties the two together
 * so they cannot drift.
 *
 * The budget that sets this: one sector is 7.08 ms of audio and the
 * measured worst-case eMMC read is 16.1 ms, so read-ahead must exceed
 * ~2.3 sectors merely to survive ONE bad read, with margin on top for
 * consecutive bad reads and for the streamer being preempted. Two slots
 * (one sector, 7.08 ms) was structurally unable to meet that; see this
 * header's own "WHY N SLOTS AND NOT TWO".
 *
 * FOUR. Three sectors of read-ahead = 21.2 ms, the first value with
 * positive margin over the 16.1 ms measured worst-case read.
 *
 * This was 2 for exactly one commit, because 2 was what the RAM budget
 * allowed: raising it to 4 pushed RAM to 245,086 B and the CI budget gate
 * correctly rejected it for leaving under its required 32 KiB free. That
 * gate was right, and it has NOT been relaxed to make this fit. What paid
 * for the two extra slots is the 16 KB batchbuf that went away with the
 * classic play-ring read-ahead (PASS 2) in streamer_thread -- a pass whose
 * body could never execute, because no track in this firmware can reach
 * TS_PLAY (the classic-source-absence CI gate proves it fail-closed). The
 * 16 KB it reserved to stage reads it would never make is now these slots.
 *
 * WHY DEPTH IS NOT OPTIONAL, even once throughput is fixed: average speed
 * and reliability are different properties. A read that is on average
 * comfortably faster than 7.08 ms still stalls occasionally on the card's
 * own internal housekeeping -- this driver's start-bit hunt allows up to
 * 80 ms for exactly that. With one sector of slack, ANY stall past 7.08 ms
 * is an audible hole no matter how quick the average is. Depth is what
 * converts "fast enough on average" into "fast enough every time".
 *
 * Deeper is still better, and the remaining ~131 KB of provably-silent
 * classic Tape Looper play rings (docs/stem-tape-capability-gap-analysis.md)
 * would buy 16 more slots. That reclaim is a larger excision of the classic
 * engine and stays a separate commit, on top of a protocol already proven
 * in the real image.
 */
#ifndef ST_STEM_MBOX_SLOTS
#define ST_STEM_MBOX_SLOTS 4u
#endif

/* Value of a slot that holds no valid sector, and of held_sector before
 * the consumer's first successful acquire. Negative so it can never
 * collide with a real (unsigned) sector index. */
#define ST_STEM_MBOX_NO_SECTOR (-1)

typedef struct {
	/* producer publishes: sector index resident in each slot */
	st_atomic32_t slot_sector[ST_STEM_MBOX_SLOTS];
	/* consumer publishes: sector index it currently needs */
	st_atomic32_t requested_sector;
	/* consumer publishes: sector index it is currently reading bytes
	 * from (its slot is the producer's forbidden target), or
	 * ST_STEM_MBOX_NO_SECTOR before the first acquire */
	st_atomic32_t held_sector;
} st_stem_mbox_t;

/* The slot a given sector index always maps to. Exposed because both
 * sides and the tests must agree on it, and because it is the whole
 * reason the consumer never has to search. */
static inline uint32_t st_stem_mbox_slot_of(uint32_t sector_index)
{
	return sector_index % ST_STEM_MBOX_SLOTS;
}

/*
 * Initializes the ring with one already-resident, already-adopted sector
 * (matching a real boot/reload sequence, where the first sector is read
 * synchronously before either thread's steady-state loop starts, so there
 * is no concurrent access to guard against here). Every other slot is
 * marked empty, so the producer will fill them in order.
 *
 * `initial_sector` is placed in ITS OWN mapped slot -- the caller does
 * not choose a slot, because the mapping is not a free parameter.
 */
void st_stem_mbox_init(st_stem_mbox_t *mb, uint32_t initial_sector);

/* ---- PRODUCER-side API (streamer_thread only) ---- */

/*
 * Chooses the next sector to fetch, if any.
 *
 * Scans forward from the consumer's currently requested sector over the
 * whole read-ahead window, and returns the FIRST sector in that window
 * whose mapped slot does not already hold it -- i.e. the nearest gap the
 * consumer will reach soonest, so a fill is always spent on the most
 * urgent missing sector rather than on speculative depth.
 *
 * `sector_count` is the song's own length, used to wrap the window at the
 * loop point so read-ahead continues across the seam instead of stalling
 * there. Pass the real count; 0 is treated as "no song" and returns false.
 *
 * The slot mapping to the consumer's held sector is skipped -- that
 * buffer is being read right now.
 *
 * Returns false when the window is already full (nothing to do). On true,
 * *out_sector is the sector to read and *out_slot is where to put it;
 * the caller MUST fill that slot and then call publish_ready() with the
 * SAME pair.
 */
bool st_stem_mbox_producer_next_fill(const st_stem_mbox_t *mb, uint32_t sector_count,
				      uint32_t *out_sector, uint32_t *out_slot);

/* Which sector index the consumer currently needs. */
uint32_t st_stem_mbox_producer_requested_sector(const st_stem_mbox_t *mb);

/*
 * Publishes `slot` as holding `sector_index`, fully written and validated.
 * Callers MUST perform every buffer-byte write BEFORE calling this -- see
 * this header's own "VISIBILITY GUARANTEE" for exactly what that buys.
 */
void st_stem_mbox_publish_ready(st_stem_mbox_t *mb, uint32_t sector_index, uint32_t slot);

/* ---- CONSUMER-side API (audio_thread only) ---- */

/*
 * If `needed_sector`'s own mapped slot currently holds exactly that
 * sector, writes the slot index to *out_slot, atomically publishes this
 * as the consumer's newly held sector (releasing whatever it held
 * before), and returns true. Otherwise returns false and touches nothing.
 *
 * Wait-free: one acquire load, and on success one release store -- never
 * blocks, never loops, never searches.
 */
bool st_stem_mbox_try_acquire(st_stem_mbox_t *mb, uint32_t needed_sector, uint32_t *out_slot);

/*
 * Publishes "I am not reading any buffer right now" (held_sector becomes
 * ST_STEM_MBOX_NO_SECTOR), freeing every slot for the producer.
 *
 * WHY THIS IS REQUIRED, not an optimization. The producer skips the slot
 * mapping to held_sector, which is what stops it overwriting the buffer
 * being decoded. At a loop wrap the consumer's held sector is the song's
 * LAST sector while the sector it now needs is 0 -- and if the song's
 * length happens to satisfy (sector_count % SLOTS) == 1, those two map to
 * the SAME slot. The producer would then refuse forever to fill the one
 * sector the consumer is waiting for, and the consumer would never
 * acquire anything and so never update held_sector: a permanent stall,
 * for particular song lengths only.
 *
 * Calling this whenever the needed sector is genuinely not resident
 * removes that class of stall entirely, and is simply true: a consumer
 * that has nothing to read is holding nothing. Idempotent and cheap --
 * it stores only on the pass that actually changes the value.
 */
void st_stem_mbox_release(st_stem_mbox_t *mb);

/* Publishes a new "sector I currently need" for the producer to read.
 * Cheap to call whenever the consumer's need changes; callers should
 * avoid calling it when the value has not changed (not required for
 * correctness, just avoids a redundant atomic store). */
void st_stem_mbox_set_requested_sector(st_stem_mbox_t *mb, uint32_t sector_index);

#endif /* STEMTAPE_PLAYER_STEM_BUFMBOX_H_ */
