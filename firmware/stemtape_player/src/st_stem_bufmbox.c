/*
 * st_stem_bufmbox.c — see st_stem_bufmbox.h for the full protocol
 * specification and correctness argument.
 */

#include "st_stem_bufmbox.h"

static inline int32_t pack_ready_word(uint32_t sector_index, uint8_t buf_idx)
{
	return (int32_t)((sector_index << 1) | (uint32_t)(buf_idx & 1u));
}

void st_stem_mbox_init(st_stem_mbox_t *mb, uint32_t initial_sector, uint8_t initial_buf_idx)
{
	/* Single-threaded boot context -- no concurrent access to guard
	 * against yet, but using the same atomic primitives here (rather
	 * than plain writes) keeps this function trivially correct under
	 * whatever memory model the backend provides, and costs nothing. */
	st_atomic_set(&mb->ready_word, pack_ready_word(initial_sector, initial_buf_idx));
	st_atomic_set(&mb->requested_sector, (int32_t)initial_sector);
	st_atomic_set(&mb->consumer_slot, (int32_t)(initial_buf_idx & 1u));
}

uint8_t st_stem_mbox_producer_target_slot(const st_stem_mbox_t *mb)
{
	int32_t consumer_slot = st_atomic_get(&mb->consumer_slot);

	return (uint8_t)(1 - consumer_slot);
}

uint32_t st_stem_mbox_producer_requested_sector(const st_stem_mbox_t *mb)
{
	return (uint32_t)st_atomic_get(&mb->requested_sector);
}

void st_stem_mbox_publish_ready(st_stem_mbox_t *mb, uint32_t sector_index, uint8_t buf_idx)
{
	/* RELEASE store: every plain write the caller made to the buffer
	 * bytes/metadata before this call is guaranteed visible to any
	 * consumer whose acquire load of this SAME atomic observes the
	 * value stored here -- see st_stem_bufmbox.h's own "VISIBILITY
	 * GUARANTEE" section. */
	st_atomic_set(&mb->ready_word, pack_ready_word(sector_index, buf_idx));
}

bool st_stem_mbox_try_acquire(st_stem_mbox_t *mb, uint32_t needed_sector, uint8_t *out_buf_idx)
{
	/* ACQUIRE load: pairs with the producer's release store in
	 * st_stem_mbox_publish_ready(), making its prior plain writes
	 * visible here once this observes the published value. */
	int32_t ready_word = st_atomic_get(&mb->ready_word);

	if (ready_word == ST_STEM_MBOX_NO_SECTOR) {
		return false;
	}

	uint32_t sector = (uint32_t)(ready_word >> 1);
	uint8_t buf_idx = (uint8_t)(ready_word & 1);

	if (sector != needed_sector) {
		return false;
	}

	/* RELEASE store: publishes "I am now reading buf_idx", which is
	 * simultaneously the release signal that frees whatever slot the
	 * consumer previously held (see st_stem_mbox_producer_target_
	 * slot() -- it is always the complement of this value). */
	st_atomic_set(&mb->consumer_slot, (int32_t)buf_idx);
	*out_buf_idx = buf_idx;
	return true;
}

void st_stem_mbox_set_requested_sector(st_stem_mbox_t *mb, uint32_t sector_index)
{
	st_atomic_set(&mb->requested_sector, (int32_t)sector_index);
}
