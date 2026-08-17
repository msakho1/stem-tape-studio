/*
 * st_transfer.c — see st_transfer.h. PURE.
 */

#include "st_transfer.h"

#include <string.h>

#include "st_crc32.h"
#include "st_transfer_protocol.h"

void st_xfer_txn_reset(st_xfer_txn_t *t)
{
	memset(t, 0, sizeof(*t));
}

st_xfer_result_t st_xfer_begin(st_xfer_txn_t *t, uint16_t slot, uint32_t frame_count,
				uint32_t expected_crc32, uint8_t stem_present_mask,
				uint32_t total_slots, uint32_t *resume_sector)
{
	uint32_t sectors;

	if (slot >= total_slots) {
		return ST_XFER_ERR_BAD_SLOT;
	}
	sectors = st_storage_song_sectors(frame_count);
	if (sectors > ST_STAGING_SECTOR_COUNT) {
		return ST_XFER_ERR_TOO_LARGE;
	}

	if (t->open && t->slot == slot && t->frame_count == frame_count &&
	    t->expected_crc32 == expected_crc32) {
		/* Identical tuple: RESUME. staged_through is untouched. */
		*resume_sector = t->staged_through;
		return ST_XFER_OK;
	}

	/* Fresh begin (or a different tuple superseding a stale in-progress
	 * transaction for this slot): discard any prior staging progress. */
	st_xfer_txn_reset(t);
	t->open = true;
	t->slot = slot;
	t->frame_count = frame_count;
	t->total_sectors = sectors;
	t->expected_crc32 = expected_crc32;
	t->stem_present_mask = stem_present_mask;
	t->staged_through = 0u;
	t->verified = false;
	*resume_sector = 0u;
	return ST_XFER_OK;
}

st_xfer_result_t st_xfer_stage_sector(st_xfer_txn_t *t, uint32_t sector_index,
				       const uint8_t data[ST_SECTOR_BYTES], uint32_t sector_crc32,
				       st_sector_write_fn write_fn, void *ctx)
{
	uint32_t actual_crc;

	if (!t->open) {
		return ST_XFER_ERR_NO_TRANSACTION;
	}
	if (sector_index != t->staged_through || sector_index >= t->total_sectors) {
		return ST_XFER_ERR_BAD_SECTOR;
	}
	actual_crc = st_crc32_compute(data, ST_SECTOR_BYTES);
	if (actual_crc != sector_crc32) {
		t->verified = false;
		return ST_XFER_ERR_SECTOR_CRC;
	}
	if (write_fn(ST_STAGING_SECTOR0 + sector_index, data, ctx) != 0) {
		t->verified = false;
		return ST_XFER_ERR_WRITE_FAILED;
	}
	t->staged_through++;
	t->verified = false; /* new data since the last verify */
	return ST_XFER_OK;
}

st_xfer_result_t st_xfer_verify(st_xfer_txn_t *t, st_sector_read_fn read_fn, void *ctx)
{
	uint32_t i;
	uint32_t crc = ST_CRC32_INIT;
	uint8_t buf[ST_SECTOR_BYTES];

	if (!t->open) {
		return ST_XFER_ERR_NO_TRANSACTION;
	}
	if (t->staged_through != t->total_sectors) {
		return ST_XFER_ERR_BAD_SECTOR; /* not every sector staged yet */
	}
	for (i = 0; i < t->total_sectors; i++) {
		if (read_fn(ST_STAGING_SECTOR0 + i, buf, ctx) != 0) {
			t->verified = false;
			return ST_XFER_ERR_READ_FAILED;
		}
		crc = st_crc32_update(crc, buf, ST_SECTOR_BYTES);
	}
	crc ^= 0xFFFFFFFFu;
	if (crc != t->expected_crc32) {
		t->verified = false;
		return ST_XFER_ERR_PAYLOAD_CRC;
	}
	t->verified = true;
	return ST_XFER_OK;
}

st_xfer_result_t st_xfer_commit_precheck(st_xfer_txn_t *t)
{
	if (!t->open) {
		return ST_XFER_ERR_NO_TRANSACTION;
	}
	if (!t->verified) {
		return ST_XFER_ERR_NOT_VERIFIED;
	}
	/* Gate passed: the caller now performs the real eMMC flush + slot
	 * commit. Clear the transaction here so a repeated C (or a stray one
	 * after a successful commit) cannot be mistaken for a second valid
	 * commit of stale state. */
	st_xfer_txn_reset(t);
	return ST_XFER_OK;
}

void st_xfer_abort(st_xfer_txn_t *t)
{
	st_xfer_txn_reset(t);
}

bool st_xfer_check_token(const uint8_t *token, uint32_t len)
{
	if (len != ST_DESTRUCTIVE_CONFIRM_LEN) {
		return false;
	}
	return memcmp(token, ST_DESTRUCTIVE_CONFIRM_TOKEN, ST_DESTRUCTIVE_CONFIRM_LEN) == 0;
}

bool st_xfer_build_slot_meta(const st_xfer_txn_t *t, uint32_t start_sector,
			      st_slot_meta_t *meta)
{
	uint8_t i;

	if (!t->open || !t->verified) {
		return false;
	}
	memset(meta, 0, sizeof(*meta));
	meta->frame_count = t->frame_count;
	meta->start_sector = start_sector;
	meta->payload_crc32 = t->expected_crc32;
	meta->stem_present_mask = t->stem_present_mask;
	/* Fresh performance-state defaults -- never carried over from a
	 * reused slot index. */
	meta->active_stem = ST_STEM_VOCAL;
	for (i = 0; i < ST_STEM_COUNT; i++) {
		meta->stem_gain_q8[i] = 0xC0u; /* ~0.75, a sane non-silent default */
	}
	meta->master_volume_q8 = 0xC0u;
	meta->scrub_speed_index = 1u; /* DEFAULT_SCRUB_SPEED_INDEX, see st_scrub.h */
	return true;
}
