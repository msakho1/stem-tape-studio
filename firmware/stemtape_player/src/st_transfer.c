/*
 * st_transfer.c — see st_transfer.h. PURE.
 */

#include "st_transfer.h"

#include <string.h>

#include "st_crc32.h"
#include "st_sector_codec.h"
#include "st_transfer_protocol.h"

void st_xfer_txn_reset(st_xfer_txn_t *t)
{
	memset(t, 0, sizeof(*t));
}

st_xfer_result_t st_xfer_begin(st_xfer_txn_t *t, uint16_t slot, const st_xfer_song_meta_t *meta,
				uint32_t total_slots, uint32_t *resume_sector)
{
	uint32_t sectors;
	uint32_t i;

	if (slot >= total_slots) {
		return ST_XFER_ERR_BAD_SLOT;
	}
	sectors = st_storage_song_sectors(meta->frame_count);
	if (sectors > ST_STAGING_SECTOR_COUNT) {
		return ST_XFER_ERR_TOO_LARGE;
	}
	for (i = 0; i < ST_STEM_COUNT; i++) {
		if (meta->stem_content_frames[i] > meta->frame_count) {
			return ST_XFER_ERR_BAD_TIMING;
		}
	}
	if (meta->frame_count != 0u && meta->downbeat_frame >= meta->frame_count) {
		return ST_XFER_ERR_BAD_TIMING;
	}

	if (t->open && t->slot == slot && t->meta.frame_count == meta->frame_count &&
	    t->meta.expected_crc32 == meta->expected_crc32) {
		/* Identical tuple: RESUME. staged_through is untouched, but refresh
		 * the rest of the declared metadata (title/artist/etc may have been
		 * re-sent with corrected text on a retry). */
		t->meta = *meta;
		*resume_sector = t->staged_through;
		return ST_XFER_OK;
	}

	/* Fresh begin (or a different tuple superseding a stale in-progress
	 * transaction for this slot): discard any prior staging progress. */
	st_xfer_txn_reset(t);
	t->open = true;
	t->slot = slot;
	t->meta = *meta;
	t->total_sectors = sectors;
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

st_xfer_result_t st_xfer_verify(st_xfer_txn_t *t, uint8_t *scratch_sector,
				 st_sector_read_fn read_fn, void *ctx)
{
	uint32_t i;
	uint32_t s;
	uint32_t payload_crc = ST_CRC32_INIT;
	uint32_t stem_crc[ST_STEM_COUNT];
	uint32_t frames_consumed[ST_STEM_COUNT] = { 0 };

	if (!t->open) {
		return ST_XFER_ERR_NO_TRANSACTION;
	}
	if (t->staged_through != t->total_sectors) {
		return ST_XFER_ERR_BAD_SECTOR; /* not every sector staged yet */
	}

	for (s = 0; s < ST_STEM_COUNT; s++) {
		stem_crc[s] = ST_CRC32_INIT;
	}

	for (i = 0; i < t->total_sectors; i++) {
		uint32_t frame_in_sector;

		if (read_fn(ST_STAGING_SECTOR0 + i, scratch_sector, ctx) != 0) {
			t->verified = false;
			return ST_XFER_ERR_READ_FAILED;
		}
		payload_crc = st_crc32_update(payload_crc, scratch_sector, ST_SECTOR_BYTES);

		for (frame_in_sector = 0; frame_in_sector < ST_SECTOR_FRAME_CAPACITY;
		     frame_in_sector++) {
			/* One frame (32 bytes) decoded at a time -- on the stack is fine,
			 * unlike the whole-sector 10880-byte frame array this replaces --
			 * see st_sector_decode_frame()'s own doc comment. */
			st_audio_frame_t frame;
			const st_audio_frame_t *fr = &frame;

			st_sector_decode_frame(scratch_sector, frame_in_sector, &frame);
			for (s = 0; s < ST_STEM_COUNT; s++) {
				uint8_t sample_bytes[8];

				if (frames_consumed[s] >= t->meta.stem_content_frames[s]) {
					continue; /* past this stem's declared content -- silence
						   * padding, excluded from its CRC by design */
				}
				sample_bytes[0] = (uint8_t)(fr->stem_l[s] & 0xFF);
				sample_bytes[1] = (uint8_t)((fr->stem_l[s] >> 8) & 0xFF);
				sample_bytes[2] = (uint8_t)((fr->stem_l[s] >> 16) & 0xFF);
				sample_bytes[3] = (uint8_t)((fr->stem_l[s] >> 24) & 0xFF);
				sample_bytes[4] = (uint8_t)(fr->stem_r[s] & 0xFF);
				sample_bytes[5] = (uint8_t)((fr->stem_r[s] >> 8) & 0xFF);
				sample_bytes[6] = (uint8_t)((fr->stem_r[s] >> 16) & 0xFF);
				sample_bytes[7] = (uint8_t)((fr->stem_r[s] >> 24) & 0xFF);
				stem_crc[s] = st_crc32_update(stem_crc[s], sample_bytes, 8u);
				frames_consumed[s]++;
			}
		}
	}
	payload_crc ^= 0xFFFFFFFFu;
	if (payload_crc != t->meta.expected_crc32) {
		t->verified = false;
		return ST_XFER_ERR_PAYLOAD_CRC;
	}
	for (s = 0; s < ST_STEM_COUNT; s++) {
		uint32_t finalized = stem_crc[s] ^ 0xFFFFFFFFu;

		if (finalized != t->meta.stem_crc32[s]) {
			t->verified = false;
			return ST_XFER_ERR_PAYLOAD_CRC;
		}
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
		return ST_XFER_ERR_INTERRUPTED_COMMIT;
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
	meta->song_id_hash = t->meta.song_id_hash;
	meta->frame_count = t->meta.frame_count;
	meta->start_sector = start_sector;
	meta->stem_present_mask = t->meta.stem_present_mask;
	meta->bpm_q8 = t->meta.bpm_q8;
	meta->downbeat_frame = t->meta.downbeat_frame;
	for (i = 0; i < ST_STEM_COUNT; i++) {
		meta->stem_content_frames[i] = t->meta.stem_content_frames[i];
		meta->stem_crc32[i] = t->meta.stem_crc32[i];
	}
	memcpy(meta->title, t->meta.title, sizeof(meta->title));
	meta->title[sizeof(meta->title) - 1] = '\0';
	memcpy(meta->artist, t->meta.artist, sizeof(meta->artist));
	meta->artist[sizeof(meta->artist) - 1] = '\0';

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
