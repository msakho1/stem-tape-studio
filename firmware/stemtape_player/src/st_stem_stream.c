/*
 * st_stem_stream.c — see st_stem_stream.h for the full design doc.
 */

#include "st_stem_stream.h"

#include <string.h>

bool st_stream_init(st_stream_t *st, uint32_t song_start_block, uint32_t song_block_count, uint32_t frames,
		     uint32_t sector_count, bool loop_enabled)
{
	memset(st, 0, sizeof(*st));
	st->ready_sector = ST_STREAM_NO_SECTOR;
	st->state = ST_STREAM_STOPPED;

	if (frames == 0u || sector_count == 0u) {
		return false;
	}

	/* Exactly ceil(frames / ST11_FRAMES_PER_SECTOR) sectors, no more,
	 * no fewer -- computed in 64-bit so a pathological frames value
	 * near UINT32_MAX cannot wrap the "+ (FRAMES_PER_SECTOR - 1)"
	 * rounding term. */
	uint64_t expected_sectors =
		((uint64_t)frames + (ST11_FRAMES_PER_SECTOR - 1u)) / ST11_FRAMES_PER_SECTOR;

	if ((uint64_t)sector_count != expected_sectors) {
		return false;
	}

	/* Never read beyond the selected song region: the sectors this
	 * song claims must fit inside its own reserved block capacity. */
	if ((uint64_t)sector_count * ST11_BLOCKS_PER_SECTOR > (uint64_t)song_block_count) {
		return false;
	}

	st->song_start_block = song_start_block;
	st->song_block_count = song_block_count;
	st->frames = frames;
	st->sector_count = sector_count;
	st->loop_enabled = loop_enabled;
	st->song_frame = 0u;
	st->ready_sector = ST_STREAM_NO_SECTOR;
	st->state = ST_STREAM_STOPPED;
	st->underrun_count = 0u;
	return true;
}

uint32_t st_stream_required_sector(const st_stream_t *st)
{
	return st->song_frame / ST11_FRAMES_PER_SECTOR;
}

bool st_stream_validate_sector(const st_stream_t *st, uint32_t sector_index,
				const st11_sector_header_t *header)
{
	if (sector_index >= st->sector_count) {
		return false;
	}
	if (header->sector_index != sector_index) {
		return false;
	}

	uint32_t expected_first_frame = sector_index * ST11_FRAMES_PER_SECTOR;

	if (header->first_frame != expected_first_frame) {
		return false;
	}

	bool is_last_sector = (sector_index + 1u == st->sector_count);
	uint32_t expected_frame_count =
		is_last_sector ? (st->frames - expected_first_frame) : ST11_FRAMES_PER_SECTOR;

	if (header->frame_count != expected_frame_count) {
		return false;
	}
	return true;
}

void st_stream_sector_ready(st_stream_t *st, uint32_t sector_index)
{
	st->ready_sector = sector_index;
	if (st->state == ST_STREAM_UNDERRUN && sector_index == st_stream_required_sector(st)) {
		st->state = ST_STREAM_PLAYING;
	}
}

void st_stream_play(st_stream_t *st)
{
	if (st->state == ST_STREAM_STOPPED) {
		st->state = ST_STREAM_PLAYING;
	}
}

void st_stream_stop(st_stream_t *st)
{
	st->state = ST_STREAM_STOPPED;
}

st_stream_tick_t st_stream_advance_frame(st_stream_t *st)
{
	if (st->state == ST_STREAM_STOPPED || st->state == ST_STREAM_END_OF_SONG) {
		return ST_STREAM_TICK_NOT_PLAYING;
	}

	uint32_t needed = st_stream_required_sector(st);

	if (st->ready_sector != needed) {
		/* Count once per UNDERRUN episode (the transition into it),
		 * matching main.c's own established g_starve_cnt[] per-
		 * track-underrun-episode counting convention -- not once
		 * per stuck tick. */
		if (st->state != ST_STREAM_UNDERRUN) {
			st->underrun_count++;
		}
		st->state = ST_STREAM_UNDERRUN;
		return ST_STREAM_TICK_UNDERRUN;
	}

	/* state is PLAYING (or was UNDERRUN and st_stream_sector_ready()
	 * already recovered it to PLAYING before this call). */
	st->state = ST_STREAM_PLAYING;
	st->song_frame++;

	if (st->song_frame >= st->frames) {
		if (st->loop_enabled) {
			st->song_frame = 0u;
			/* The old ready_sector (whatever sector held the
			 * song's last frame) is NOT sector 0 in general --
			 * invalidate it so the caller is forced to supply a
			 * freshly-validated sector 0 before the next tick
			 * can advance, rather than ever assuming stale data
			 * is still current. */
			st->ready_sector = ST_STREAM_NO_SECTOR;
			return ST_STREAM_TICK_LOOPED;
		}
		st->state = ST_STREAM_END_OF_SONG;
		return ST_STREAM_TICK_ENDED;
	}

	uint32_t new_needed = st_stream_required_sector(st);

	if (new_needed != needed) {
		return ST_STREAM_TICK_SECTOR_CROSSED;
	}
	return ST_STREAM_TICK_OK;
}
