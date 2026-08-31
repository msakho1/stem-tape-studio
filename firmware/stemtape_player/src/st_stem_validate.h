/*
 * st_stem_validate.h — the atomic-commit gate for a Stem Tape 4-stem song.
 *
 * PURE: no Zephyr, no I/O. Called directly from the real xfer_service()'s
 * 'Z' (stem-song commit) verb in main.c: xfer_service() does the actual
 * eMMC read-back and CRC-32 computation (st_crc32.c) over the just-written
 * blocks, then hands the declared-vs-actual values to
 * st_stem_validate_commit() here. Only a ST_STEM_OK return allows
 * xfer_service() to set present[]=1 for the four tracks and call the real
 * meta_write_blocks() -- there is no other path to a committed stem song.
 *
 * This is the single production validation path: it is not a parallel or
 * host-only system, and there is no other function anywhere that decides
 * whether an uploaded song is allowed to become playable.
 */

#ifndef STEMTAPE_STEM_VALIDATE_H_
#define STEMTAPE_STEM_VALIDATE_H_

#include <stdbool.h>
#include <stdint.h>

#define ST_STEM_COUNT 4u

typedef enum {
	ST_STEM_OK = 0,
	ST_STEM_ERR_MISSING_STEM,     /* present_mask != 0x0F: not all 4 stems declared */
	ST_STEM_ERR_ZERO_LENGTH,      /* a declared frame_count is 0 */
	ST_STEM_ERR_LENGTH_MISMATCH,  /* the 4 declared frame_counts are not identical --
					* the shared-transport invariant every stem song
					* depends on (see main.c's looper_audio_block()
					* PASS A/B: one playhead, tracks assumed equal length) */
	ST_STEM_ERR_TOO_LARGE,        /* a declared frame_count exceeds the device's real
					* per-track block capacity (TRACK_BLOCKS) */
	ST_STEM_ERR_CRC_MISMATCH,     /* a declared per-stem CRC-32 does not match the CRC-32
					* actually computed over the just-written blocks */
} st_stem_result_t;

typedef struct {
	uint8_t  present_mask;                   /* bit i set = stem i declared present;
						    * must be exactly 0x0F (all four) */
	uint32_t frame_count[ST_STEM_COUNT];     /* declared length, in eMMC blocks, per stem */
	uint32_t declared_crc32[ST_STEM_COUNT];  /* CRC-32 the companion tool declared for each stem */
	uint32_t actual_crc32[ST_STEM_COUNT];    /* CRC-32 the firmware computed from a REAL
						    * read-back of what is actually on the card
						    * (never trust write-side data alone) */
	uint32_t track_block_capacity;           /* TRACK_BLOCKS: the real per-track region size
						    * on this device */
} st_stem_commit_t;

/*
 * Decides whether a 4-stem song may be committed. Pure decision function --
 * never mutates storage. Checked in a fixed, documented order so a caller
 * surfacing the result to a companion tool or LED pattern gets the single
 * most relevant reason, not just "rejected":
 *   1. all four stems present
 *   2. no stem is zero-length
 *   3. all four share one frame count (deterministic sync requires this)
 *   4. that shared length fits the device's real per-track capacity
 *   5. every stem's real read-back CRC-32 matches what was declared
 */
st_stem_result_t st_stem_validate_commit(const st_stem_commit_t *c);

#endif /* STEMTAPE_STEM_VALIDATE_H_ */
