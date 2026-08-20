/*
 * st_stem_bufmbox.c — see st_stem_bufmbox.h for the full protocol
 * specification and correctness argument.
 */

#include "st_stem_bufmbox.h"

void st_stem_mbox_init(st_stem_mbox_t *mb, uint32_t initial_sector)
{
	uint32_t i;
	uint32_t held_slot = st_stem_mbox_slot_of(initial_sector);

	/* Single-threaded boot/reload context -- no concurrent access to
	 * guard against yet, but using the same atomic primitives here
	 * (rather than plain writes) keeps this trivially correct under
	 * whatever memory model the backend provides, and costs nothing. */
	for (i = 0; i < ST_STEM_MBOX_SLOTS; i++) {
		st_atomic_set(&mb->slot_sector[i], ST_STEM_MBOX_NO_SECTOR);
	}
	st_atomic_set(&mb->slot_sector[held_slot], (int32_t)initial_sector);
	st_atomic_set(&mb->requested_sector, (int32_t)initial_sector);
	st_atomic_set(&mb->held_sector, (int32_t)initial_sector);
}

bool st_stem_mbox_producer_next_fill(const st_stem_mbox_t *mb, uint32_t sector_count,
				      uint32_t *out_sector, uint32_t *out_slot)
{
	uint32_t requested;
	int32_t held;
	uint32_t forbidden_slot;
	uint32_t window;
	uint32_t k;

	if (sector_count == 0u) {
		return false;
	}

	requested = (uint32_t)st_atomic_get(&mb->requested_sector);
	/* Read FRESH every call, never cached: this is the whole mechanism
	 * that keeps the producer off the buffer the consumer is reading.
	 * A stale (older) value is always safe -- held_sector only moves
	 * forward, so an old read can only make this more conservative. */
	held = st_atomic_get(&mb->held_sector);
	forbidden_slot = (held == ST_STEM_MBOX_NO_SECTOR)
			 ? ST_STEM_MBOX_SLOTS      /* nothing held: no slot is off-limits */
			 : st_stem_mbox_slot_of((uint32_t)held);

	if (requested >= sector_count) {
		/* Defensive: a requested sector outside the song cannot be
		 * fetched. Never trust it into the modulo below. */
		return false;
	}

	/* The window can never exceed the ring itself, and a song shorter
	 * than the ring must not be scanned past its own end (that would
	 * map two different window positions onto the same sector). */
	window = ST_STEM_MBOX_SLOTS;
	if (window > sector_count) {
		window = sector_count;
	}

	/* Nearest gap first: k == 0 is the sector the consumer needs RIGHT
	 * NOW, which after a seek is legitimately missing and must be
	 * fetched before any read-ahead. */
	for (k = 0; k < window; k++) {
		uint32_t sector = requested + k;
		uint32_t slot;

		if (sector >= sector_count) {
			sector -= sector_count;    /* wrap at the loop seam */
		}
		slot = st_stem_mbox_slot_of(sector);

		if (slot == forbidden_slot) {
			continue;
		}
		if ((uint32_t)st_atomic_get(&mb->slot_sector[slot]) == sector) {
			continue;                  /* already resident */
		}

		*out_sector = sector;
		*out_slot = slot;
		return true;
	}

	return false;
}

uint32_t st_stem_mbox_producer_requested_sector(const st_stem_mbox_t *mb)
{
	return (uint32_t)st_atomic_get(&mb->requested_sector);
}

void st_stem_mbox_publish_ready(st_stem_mbox_t *mb, uint32_t sector_index, uint32_t slot)
{
	/* RELEASE store: every plain write the caller made to this slot's
	 * buffer bytes before this call is guaranteed visible to any
	 * consumer whose acquire load of this SAME atomic observes the
	 * value stored here -- see st_stem_bufmbox.h's own "VISIBILITY
	 * GUARANTEE" section. */
	st_atomic_set(&mb->slot_sector[slot], (int32_t)sector_index);
}

bool st_stem_mbox_try_acquire(st_stem_mbox_t *mb, uint32_t needed_sector, uint32_t *out_slot)
{
	uint32_t slot = st_stem_mbox_slot_of(needed_sector);
	/* ACQUIRE load: pairs with the producer's release store in
	 * st_stem_mbox_publish_ready(), making its prior plain buffer
	 * writes visible here once this observes the published value.
	 *
	 * Observing `needed_sector` in the slot that needed_sector itself
	 * maps to IS the proof that this buffer holds that sector -- there
	 * is no separate index/flag pair that could be seen half-updated. */
	int32_t resident = st_atomic_get(&mb->slot_sector[slot]);

	if (resident == ST_STEM_MBOX_NO_SECTOR || (uint32_t)resident != needed_sector) {
		return false;
	}

	/* RELEASE store: publishes "I am now reading this sector", which is
	 * simultaneously the release signal freeing whatever the consumer
	 * held before (see st_stem_mbox_producer_next_fill(), which skips
	 * only the slot mapping to this value). */
	st_atomic_set(&mb->held_sector, (int32_t)needed_sector);
	*out_slot = slot;
	return true;
}

void st_stem_mbox_release(st_stem_mbox_t *mb)
{
	/* Store only when it changes: this is called from the real-time
	 * audio thread on every frame that finds its sector missing, and a
	 * redundant atomic store per frame at 48 kHz is pure waste. The
	 * consumer is the ONLY writer of held_sector, so reading it back
	 * here cannot race with anything. */
	if (st_atomic_get(&mb->held_sector) != ST_STEM_MBOX_NO_SECTOR) {
		st_atomic_set(&mb->held_sector, ST_STEM_MBOX_NO_SECTOR);
	}
}

void st_stem_mbox_set_requested_sector(st_stem_mbox_t *mb, uint32_t sector_index)
{
	st_atomic_set(&mb->requested_sector, (int32_t)sector_index);
}
