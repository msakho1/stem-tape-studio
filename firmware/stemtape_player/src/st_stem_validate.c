/*
 * st_stem_validate.c — see st_stem_validate.h. PURE.
 */

#include "st_stem_validate.h"

st_stem_result_t st_stem_validate_commit(const st_stem_commit_t *c)
{
	uint32_t i;

	if (c->present_mask != 0x0Fu) {
		return ST_STEM_ERR_MISSING_STEM;
	}
	for (i = 0; i < ST_STEM_COUNT; i++) {
		if (c->frame_count[i] == 0u) {
			return ST_STEM_ERR_ZERO_LENGTH;
		}
	}
	for (i = 1; i < ST_STEM_COUNT; i++) {
		if (c->frame_count[i] != c->frame_count[0]) {
			return ST_STEM_ERR_LENGTH_MISMATCH;
		}
	}
	for (i = 0; i < ST_STEM_COUNT; i++) {
		if (c->frame_count[i] > c->track_block_capacity) {
			return ST_STEM_ERR_TOO_LARGE;
		}
	}
	for (i = 0; i < ST_STEM_COUNT; i++) {
		if (c->declared_crc32[i] != c->actual_crc32[i]) {
			return ST_STEM_ERR_CRC_MISMATCH;
		}
	}
	return ST_STEM_OK;
}
