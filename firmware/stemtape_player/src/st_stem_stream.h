/*
 * st_stem_stream.h — pure stored-song CONTINUOUS streaming state machine
 * (STEM TAPE Phase 2 continuous streaming, Slice 3A).
 *
 * Slice 2 played back exactly one resident STSC sector (<= 340 frames,
 * ~7 ms), looped indefinitely. This module is the pure logic core for
 * playing the WHOLE song, every sector in order, that Slice 3B's real
 * double-buffered streamer_thread prefetch wiring will drive. It owns NO
 * storage, NO buffers, and does NO I/O of its own: it only ever operates
 * on the immutable song geometry taken from the selected st_stix_record_t
 * (song_start_block, song_block_count, frames, sector_count) and on
 * caller-supplied sector header data -- the same "injected geometry,
 * caller supplies I/O" convention st_stix.h/st_ab_session.h already use,
 * which is exactly what makes this module host-testable without any real
 * flash, and exactly what Slice 3B's audio thread needs: no eMMC access,
 * no allocation, no blocking, ever, from this code.
 *
 * THE ONE SEQUENCE a real caller (streamer_thread, in Slice 3B) follows:
 *   1. st_stream_init()            -- once, from the selected song's own
 *                                      STIX geometry. Fails closed on any
 *                                      geometry that could not possibly
 *                                      be a valid STSC sector sequence.
 *   2. st_stream_required_sector() -- which sector index the CURRENT
 *                                      song_frame needs.
 *   3. read that physical sector off eMMC into a caller-owned buffer,
 *      st11_sector_read_header() it, and pass the result to...
 *   4. st_stream_validate_sector() -- checks sector_index, first_frame,
 *      frame_count and bounds against the song's own declared geometry,
 *      for EVERY sector, not just the first -- st11_sector_read_header()
 *      already checked the magic; this checks everything else Slice 3A's
 *      own directive requires.
 *   5. on success, st_stream_sector_ready() marks that sector index
 *      resident/playable (recovering from UNDERRUN if that was the
 *      sector the stream was waiting for). The audio path then calls
 *      st_stream_advance_frame() once per output frame: it reports
 *      whether the frame just consumed stayed inside the same sector,
 *      crossed into a new one (Slice 3B re-does 2-5 for the new sector,
 *      ideally well ahead of the playhead reaching it), reached end-of-
 *      song (loops back to frame 0, per st_stream_init()'s own
 *      loop_enabled choice, or stops), or could not advance at all
 *      because the sector it needs was never marked ready in time.
 *   6. if a read/validate genuinely FAILS (corrupt header, wrong
 *      sector_index/first_frame/frame_count, out-of-range, or a short
 *      read that never produced ST11_SECTOR_BYTES worth of real data),
 *      the caller calls st_stream_report_corrupt() instead of
 *      st_stream_sector_ready() -- this stops playback safely (a corrupt
 *      sector will not become valid on retry without external
 *      intervention, so unlike underrun this state does not auto-
 *      recover) rather than ever decoding/mixing garbage bytes as if
 *      they were real audio.
 *
 * STATES (deterministic, exactly these five, no others):
 *   STOPPED      -- song_frame frozen; advance_frame() is a no-op.
 *   PLAYING      -- the sector st->ready_sector matches what song_frame
 *                    currently needs; advance_frame() moves forward.
 *   UNDERRUN     -- playing was requested but the needed sector was
 *                    never marked ready in time; song_frame FROZEN
 *                    (never guessed, never replays stale sector data),
 *                    a diagnostic counter increments once per episode,
 *                    and the state recovers to PLAYING automatically the
 *                    moment st_stream_sector_ready() supplies the exact
 *                    sector this state was waiting for.
 *   END_OF_SONG  -- the last frame was consumed and loop_enabled was
 *                    false; song_frame frozen at `frames` (one past the
 *                    last valid index); advance_frame() is a no-op,
 *                    matching STOPPED's own "stop at end" behavior
 *                    exactly, until st_stream_play() or a fresh
 *                    st_stream_init() is called.
 *   (STOPPED is also the state st_stream_report_corrupt() lands in --
 *    there is no separate sixth "halted" state; a corrupt stop and a
 *    user stop are the same STOPPED state, distinguished only by which
 *    diagnostic counter incremented on the way in.)
 *
 * PURE: no I/O, no Zephyr, no dynamic allocation, no floating point,
 * bounded/O(1) execution per call -- safe to drive from a hard-real-time
 * context exactly like st_stem_mix.h's own mixer.
 */

#ifndef STEMTAPE_PLAYER_STEM_STREAM_H_
#define STEMTAPE_PLAYER_STEM_STREAM_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_sector_v11.h"
#include "st_v11_format.h"

/* st->ready_sector's value when no sector has ever been marked ready
 * (initial state, and immediately after a loop wrap or a corrupt-stop,
 * both of which invalidate whatever sector index used to be resident --
 * Slice 3B must never assume its own physical buffer is still valid for
 * the new position just because the index looks unchanged). */
#define ST_STREAM_NO_SECTOR 0xFFFFFFFFu

typedef enum {
	ST_STREAM_STOPPED = 0,
	ST_STREAM_PLAYING,
	ST_STREAM_END_OF_SONG,
	ST_STREAM_UNDERRUN,
} st_stream_state_t;

typedef struct {
	/* Immutable song geometry, set once by st_stream_init() from the
	 * selected st_stix_record_t. Never reassigned by any other
	 * function in this file. */
	uint32_t song_start_block;
	uint32_t song_block_count;
	uint32_t frames;
	uint32_t sector_count;
	bool loop_enabled;

	/* Mutable playback state. */
	st_stream_state_t state;
	uint32_t song_frame;     /* the ONE authoritative absolute song frame */
	uint32_t ready_sector;   /* sector index currently marked resident/playable, or ST_STREAM_NO_SECTOR */
	uint32_t underrun_count; /* diagnostic: episodes (not ticks) of UNDERRUN entered; never reset here */
	uint32_t corrupt_count;  /* diagnostic: st_stream_report_corrupt() calls; never reset here */
} st_stream_t;

/*
 * Validates the song's own STIX geometry and initializes `st` as STOPPED
 * at song_frame 0, ready_sector ST_STREAM_NO_SECTOR. Returns false (and
 * leaves `st` fully zeroed/STOPPED -- safe to leave unused, never played)
 * if the geometry itself could not describe a valid STSC sector sequence:
 *   - frames == 0 or sector_count == 0
 *   - sector_count != ceil(frames / ST11_FRAMES_PER_SECTOR) -- the exact
 *     number of sectors this many frames requires, no more, no fewer
 *     (matches st11_sector_header_t's own "frame_count <= FRAMES_PER_
 *     SECTOR, short on the final sector, never zero" contract)
 *   - (uint64_t)sector_count * ST11_BLOCKS_PER_SECTOR > song_block_count
 *     -- the sectors this song claims would not fit inside its own
 *     reserved capacity ("never read beyond the selected song region",
 *     enforced structurally here rather than merely by convention)
 */
bool st_stream_init(st_stream_t *st, uint32_t song_start_block, uint32_t song_block_count, uint32_t frames,
		     uint32_t sector_count, bool loop_enabled);

/* Which STSC sector index the CURRENT song_frame requires. Always
 * < st->sector_count by construction (st_stream_init()'s own geometry
 * check guarantees it) -- this function itself performs no bounds
 * checking because none can ever be needed. */
uint32_t st_stream_required_sector(const st_stream_t *st);

/*
 * Validates one freshly-read sector's header against this song's own
 * geometry and the sector index the caller claims to have physically
 * read (which need not already equal st_stream_required_sector() --
 * Slice 3B's caller may prefetch ahead of the playhead). Checked in this
 * fixed order:
 *   1. sector_index < st->sector_count            ("never read beyond
 *      the selected song region")
 *   2. header->sector_index == sector_index        (misaddressed read)
 *   3. header->first_frame == sector_index * ST11_FRAMES_PER_SECTOR
 *   4. header->frame_count == the EXACT expected count for that sector:
 *      ST11_FRAMES_PER_SECTOR for every sector except the last,
 *      (st->frames - header->first_frame) for the last sector -- "handle
 *      the final partial sector exactly"
 * Does NOT itself check the 'STSC' magic -- that is
 * st11_sector_read_header()'s own job; a caller whose read failed there
 * (or a short/incomplete physical read) must call
 * st_stream_report_corrupt() directly instead of fabricating header
 * fields to pass in here.
 */
bool st_stream_validate_sector(const st_stream_t *st, uint32_t sector_index,
				const st11_sector_header_t *header);

/*
 * Marks `sector_index` (already checked true by st_stream_validate_
 * sector()) as the currently resident, playable sector. If the stream
 * was UNDERRUN and `sector_index` is exactly the sector it was waiting
 * for (st_stream_required_sector() at the moment of the call), recovers
 * the state back to PLAYING. Harmless (records the index, no state
 * change) if called while STOPPED/END_OF_SONG/already-PLAYING.
 */
void st_stream_sector_ready(st_stream_t *st, uint32_t sector_index);

/*
 * A sector read/validate genuinely FAILED (bad header, wrong sector_
 * index/first_frame/frame_count, out-of-range, or a short physical
 * read). Stops playback SAFELY: state -> STOPPED, song_frame frozen
 * wherever it was, ready_sector invalidated back to ST_STREAM_NO_SECTOR
 * (never let a caller assume a since-discarded buffer is still current),
 * corrupt_count incremented. Unlike UNDERRUN this does not auto-recover
 * -- corrupt data does not become valid by itself; resuming requires an
 * explicit st_stream_play() once the caller has a validated sector
 * ready again.
 */
void st_stream_report_corrupt(st_stream_t *st);

/*
 * Transport control, reusing the same "freeze, never reset" convention
 * main.c's own existing g_playing/p_w already follow: st_stream_play()
 * moves STOPPED -> PLAYING without touching song_frame (a no-op from any
 * other state); st_stream_stop() moves any state -> STOPPED, freezing
 * song_frame wherever it currently is.
 */
void st_stream_play(st_stream_t *st);
void st_stream_stop(st_stream_t *st);

typedef enum {
	ST_STREAM_TICK_NOT_PLAYING = 0,  /* state was STOPPED or END_OF_SONG already; no-op */
	ST_STREAM_TICK_OK,               /* advanced by one frame, same sector as before */
	ST_STREAM_TICK_SECTOR_CROSSED,   /* advanced by one frame, now needs a NEW sector */
	ST_STREAM_TICK_LOOPED,           /* advanced past the last frame; loop_enabled -- wrapped to frame 0,
					   * ready_sector invalidated, now needs sector 0 again */
	ST_STREAM_TICK_ENDED,            /* advanced past the last frame; !loop_enabled -- state is now
					   * END_OF_SONG, song_frame frozen at `frames` */
	ST_STREAM_TICK_UNDERRUN,         /* could not advance: the sector song_frame needs was never marked
					   * ready; state is (now) UNDERRUN, song_frame UNCHANGED */
} st_stream_tick_t;

/*
 * Called once per output audio frame while the caller wants playback to
 * advance. Never touches any sector buffer itself -- pure bookkeeping;
 * the caller decodes+mixes the CURRENT song_frame using whatever buffer
 * holds st->ready_sector, before or after calling this (order does not
 * matter to this function, it only ever advances position). See the enum
 * above for the full set of outcomes.
 */
st_stream_tick_t st_stream_advance_frame(st_stream_t *st);

#endif /* STEMTAPE_PLAYER_STEM_STREAM_H_ */
