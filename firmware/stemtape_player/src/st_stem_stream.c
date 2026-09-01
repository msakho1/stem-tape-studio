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

/* -O2 FOR THIS FUNCTION ONLY -- see st_stem_mix.c's matching note and
 * sp1_emmc.c's crc16() for the established precedent. Pure computation, no
 * timing or aliasing dependency. main.c's audio thread calls this on every
 * output frame at 48 kHz (and st_stream_advance_frame() below calls it
 * again), so the division by a compile-time constant wants to be the
 * multiply-and-shift -O2 emits rather than the call -Os is entitled to. */
__attribute__((optimize("O2")))
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

void st_stream_set_reverse(st_stream_t *st, bool reverse)
{
	if (st->reverse == reverse) {
		return;
	}
	st->reverse = reverse;

	/*
	 * POSITION AND RESIDENCY ARE NOT TOUCHED. See the header: the head is
	 * still on the frame it was on, and the group holding that frame still
	 * holds it. The only thing that changed is which neighbour comes next.
	 *
	 * The terminal states ARE lifted, but only the one the new direction
	 * frees. A head parked at START_OF_SONG has tape ahead of it again the
	 * moment it turns forward; a head parked at END_OF_SONG has tape ahead
	 * of it the moment it turns back. Leaving either latched would make the
	 * track silently refuse to move after a turn -- and the spec is explicit
	 * that turning reverse off "lets it move forward from the beginning
	 * again".
	 */
	if (!reverse && st->state == ST_STREAM_START_OF_SONG) {
		st->state = ST_STREAM_PLAYING;
		return;
	}
	if (reverse && st->state == ST_STREAM_END_OF_SONG) {
		/* END_OF_SONG parks song_frame at `frames`, one PAST the last
		 * real index -- unlike START_OF_SONG, which parks at 0, a
		 * frame that genuinely exists. Turning around therefore has to
		 * pull the head back onto the song before it can play, and
		 * that is a position change, so residency goes with it. This is
		 * the one place direction moves a head, and it moves it onto
		 * the nearest frame that exists rather than to anywhere
		 * chosen. */
		if (st->frames > 0u) {
			st->song_frame = st->frames - 1u;
			st->ready_sector = ST_STREAM_NO_SECTOR;
			st->state = ST_STREAM_PLAYING;
		}
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

bool st_stream_seek(st_stream_t *st, uint32_t frame)
{
	if (frame >= st->frames) {
		return false;   /* never address a frame outside the song */
	}

	st->song_frame = frame;

	/* INVALIDATE RESIDENCY. The sector that was ready is almost certainly
	 * not the one this frame needs, and claiming otherwise would make the
	 * caller decode the wrong sector's bytes as if they were this frame's
	 * -- exactly the "stale sector data" failure a seek must not cause.
	 * The consumer re-acquires from the mailbox on its next pass, the same
	 * way it does after a loop wrap (ST_STREAM_TICK_LOOPED invalidates
	 * this same field for this same reason).
	 *
	 * ST_STREAM_NO_SECTOR is deliberately used rather than "the sector
	 * this frame needs": residency is a fact about the buffer, and only a
	 * real, validated mailbox acquire may assert it. */
	st->ready_sector = ST_STREAM_NO_SECTOR;

	/* A seek out of END_OF_SONG or UNDERRUN resumes playing: the position
	 * that could not be served is no longer the position we are at. A
	 * STOPPED stream stays stopped -- seeking is not a transport command. */
	if (st->state != ST_STREAM_STOPPED) {
		st->state = ST_STREAM_PLAYING;
	}
	return true;
}

/* -O2, same reason as st_stream_required_sector() above: this is the other
 * per-output-frame call on the 48 kHz path. */
__attribute__((optimize("O2")))
st_stream_tick_t st_stream_advance_frame(st_stream_t *st)
{
	if (st->state == ST_STREAM_STOPPED || st->state == ST_STREAM_END_OF_SONG ||
	    st->state == ST_STREAM_START_OF_SONG) {
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

	if (st->reverse) {
		/* THE MIRROR, and the reason it is a separate branch rather
		 * than a signed step: the end conditions are not symmetric.
		 * Forward runs off the end into a state that parks one PAST
		 * the song and may loop; backward runs into frame 0, which is
		 * a real frame, parks ON it, and never wraps. */
		if (st->song_frame == 0u) {
			st->state = ST_STREAM_START_OF_SONG;
			return ST_STREAM_TICK_START_REACHED;
		}
		st->song_frame--;
		if (st_stream_required_sector(st) != needed) {
			return ST_STREAM_TICK_SECTOR_CROSSED;
		}
		return ST_STREAM_TICK_OK;
	}

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

/* -O2: this is THE call the 48 kHz path makes, once per run. See the
 * header for the precondition and for why the run form exists at all. */
__attribute__((optimize("O2")))
st_stream_tick_t st_stream_advance_frames(st_stream_t *st, uint32_t count)
{
	if (count == 0u) {
		return ST_STREAM_TICK_NOT_PLAYING;
	}
	if (st->state == ST_STREAM_STOPPED || st->state == ST_STREAM_END_OF_SONG ||
	    st->state == ST_STREAM_START_OF_SONG) {
		return ST_STREAM_TICK_NOT_PLAYING;
	}

	uint32_t needed = st_stream_required_sector(st);

	if (st->ready_sector != needed) {
		/* Same episode accounting as the per-frame form: counted on the
		 * TRANSITION into underrun, never once per stuck frame. A
		 * starved caller asks for a whole run and gets none of it. */
		if (st->state != ST_STREAM_UNDERRUN) {
			st->underrun_count++;
		}
		st->state = ST_STREAM_UNDERRUN;
		return ST_STREAM_TICK_UNDERRUN;
	}

	st->state = ST_STREAM_PLAYING;

	if (st->reverse) {
		/* Exactly `count` calls to the per-frame form, in one step --
		 * the same equivalence the forward path claims and the same
		 * host test walks. A caller honouring the precondition passes
		 * at most (fis + 1), so `count > song_frame` means it asked to
		 * step off the front of the song; the head parks ON frame 0
		 * rather than at some negative index, because frame 0 is a real
		 * frame and START_OF_SONG is where a reversed head stops. */
		if (count > st->song_frame) {
			st->song_frame = 0u;
			st->state = ST_STREAM_START_OF_SONG;
			return ST_STREAM_TICK_START_REACHED;
		}
		st->song_frame -= count;
		if (st_stream_required_sector(st) != needed) {
			return ST_STREAM_TICK_SECTOR_CROSSED;
		}
		return ST_STREAM_TICK_OK;
	}

	st->song_frame += count;

	if (st->song_frame >= st->frames) {
		if (st->loop_enabled) {
			st->song_frame = 0u;
			/* Identical reasoning to the per-frame form: the sector
			 * that held the song's last frame is not sector 0, so it
			 * must be invalidated rather than assumed current. */
			st->ready_sector = ST_STREAM_NO_SECTOR;
			return ST_STREAM_TICK_LOOPED;
		}
		st->state = ST_STREAM_END_OF_SONG;
		return ST_STREAM_TICK_ENDED;
	}

	if (st_stream_required_sector(st) != needed) {
		return ST_STREAM_TICK_SECTOR_CROSSED;
	}
	return ST_STREAM_TICK_OK;
}
