/*
 * st_stem_stream.h — pure stored-song CONTINUOUS streaming state machine
 * (STEM TAPE Phase 2 continuous streaming: pure module added, host-test-
 * only, in Slice 3A; wired into main.c's streamer_thread()/looper_audio_
 * block() in Slice 3B; Slice 3B.1 corrected the cross-thread handoff --
 * see below).
 *
 * Slice 2 played back exactly one resident STSC sector (<= 340 frames,
 * ~7 ms), looped indefinitely. This module is the pure logic core for
 * playing the WHOLE song, every sector in order. It owns NO storage, NO
 * buffers, and does NO I/O of its own: it only ever operates on the
 * immutable song geometry taken from the selected st_stix_record_t
 * (song_start_block, song_block_count, frames, sector_count) and on
 * caller-supplied sector header data -- the same "injected geometry,
 * caller supplies I/O" convention st_stix.h/st_ab_session.h already use,
 * which is exactly what makes this module host-testable without any real
 * flash, and exactly what the real audio thread needs: no eMMC access,
 * no allocation, no blocking, ever, from this code.
 *
 * OWNERSHIP / THREAD SAFETY (Slice 3B.1 correction): Slice 3B originally
 * had TWO threads mutate this SAME struct directly (streamer_thread
 * calling st_stream_sector_ready()/a since-removed st_stream_report_
 * corrupt(); audio_thread calling st_stream_advance_frame()), guarded
 * only by `volatile` fields and a documented write-order argument. That
 * was replaced: this struct is now exclusively owned and mutated by ONE
 * thread -- the real audio thread -- end to end. The producer thread
 * (streamer_thread) NEVER calls a mutating function on a shared
 * st_stream_t instance; it only calls st_stream_validate_sector(), which
 * reads ONLY the immutable geometry fields (song_start_block/song_block_
 * count/frames/sector_count/loop_enabled -- written once by st_stream_
 * init() before any concurrent thread exists, never reassigned after),
 * so a plain (non-atomic, non-volatile) struct is correct: every mutable
 * field genuinely has exactly one writer thread for its entire lifetime,
 * which is why none of them need be `volatile` any more. The actual
 * cross-thread handoff -- which sector is ready, which physical buffer
 * holds it, which buffer the producer may safely refill -- now lives in
 * st_stem_bufmbox.h's own formally-specified atomic mailbox; see that
 * header for the full protocol. The real caller sequence below reflects
 * this: step 6 (recover a validated sector) and the old "report a
 * corrupt sector into this struct" step are gone from THIS module --
 * that response now belongs entirely to the mailbox + the caller's own
 * per-pass logic (see main.c's own comments at its call sites).
 *
 * THE ONE SEQUENCE the real audio thread (the sole caller of every
 * mutating function below) follows, once per output frame:
 *   1. st_stream_init()            -- once, at boot, before either
 *                                      thread's steady-state loop starts.
 *                                      Fails closed on any geometry that
 *                                      could not possibly be a valid STSC
 *                                      sector sequence.
 *   2. st_stream_required_sector() -- which sector index the CURRENT
 *                                      song_frame needs.
 *   3. if that sector isn't already known-ready (st->ready_sector !=
 *      needed), ask the mailbox (st_stem_mbox_try_acquire()) whether the
 *      producer has published it; if so, call st_stream_sector_ready()
 *      on THIS SAME (audio-thread-owned) struct to record it locally.
 *   4. st_stream_advance_frame() -- advances by one frame if the needed
 *      sector is (now) ready; otherwise records an UNDERRUN tick
 *      (song_frame frozen, never guessed) without touching the mailbox
 *      at all -- the mailbox is polled again next frame.
 *
 * The producer side (streamer_thread) has its own, separate sequence,
 * documented in st_stem_bufmbox.h and at its main.c call sites: read the
 * mailbox's own target buffer slot, fill + st_stream_validate_sector()
 * it (read-only geometry access -- safe from any thread), then publish
 * it ready via the mailbox -- never touching st_stream_t's mutable
 * fields.
 *
 * STATES (deterministic, exactly these four, no others):
 *   STOPPED      -- song_frame frozen; advance_frame() is a no-op.
 *   PLAYING      -- the sector st->ready_sector matches what song_frame
 *                    currently needs; advance_frame() moves forward.
 *   UNDERRUN     -- playing was requested but the needed sector was
 *                    never marked ready in time; song_frame FROZEN
 *                    (never guessed, never replays stale sector data),
 *                    a diagnostic counter increments once per episode,
 *                    and the state recovers to PLAYING automatically the
 *                    moment st_stream_sector_ready() supplies the exact
 *                    sector this state was waiting for (called by the
 *                    SAME audio thread, after the mailbox confirms it).
 *   END_OF_SONG  -- the last frame was consumed and loop_enabled was
 *                    false; song_frame frozen at `frames` (one past the
 *                    last valid index); advance_frame() is a no-op,
 *                    matching STOPPED's own "stop at end" behavior
 *                    exactly, until st_stream_play() or a fresh
 *                    st_stream_init() is called.
 *   START_OF_SONG -- END_OF_SONG's mirror, reachable only while
 *                    `reverse` is set: a backward head consumed frame 0
 *                    and there is nothing before it. song_frame frozen
 *                    AT 0 (a real, playable index, unlike END_OF_SONG's
 *                    one-past-the-end), advance is a no-op, and
 *                    st_stream_set_reverse(st, false) lifts it straight
 *                    back to PLAYING. Per docs/stem-tape-per-track-
 *                    reverse-spec.md: "A reversed track that reaches the
 *                    absolute beginning stops/clamps there. It does not
 *                    wrap to the end."
 * (A persistently corrupt sector -- the producer's own validation keeps
 * failing -- is no longer a distinct state this module tracks: the
 * mailbox simply never publishes it ready, which this module already
 * represents as UNDERRUN. This is a deliberate simplification, not a
 * capability loss: Slice 3B's own production wiring already made a
 * corrupt-stop behaviorally indistinguishable from underrun, because
 * st_stream_play() was called unconditionally every block whenever the
 * transport stayed in PLAY -- see that commit's own note. The producer
 * still counts corrupt-validation failures on its own diagnostic
 * counter, in main.c, for observability.)
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
 * (initial state, and immediately after a loop wrap, which invalidates
 * whatever sector index used to be resident -- the caller must never
 * assume its own physical buffer is still valid for the new position
 * just because the index looks unchanged). */
#define ST_STREAM_NO_SECTOR 0xFFFFFFFFu

typedef enum {
	ST_STREAM_STOPPED = 0,
	ST_STREAM_PLAYING,
	ST_STREAM_END_OF_SONG,
	ST_STREAM_UNDERRUN,
	ST_STREAM_START_OF_SONG,
} st_stream_state_t;

typedef struct {
	/* Immutable song geometry, set once by st_stream_init() from the
	 * selected st_stix_record_t, from a single-threaded boot context
	 * before any concurrent caller exists. Never reassigned by any
	 * other function in this file. Read-only after init by BOTH the
	 * audio thread (its own logic) and the producer thread
	 * (st_stream_validate_sector()'s geometry checks) -- concurrent
	 * READS of a value no one ever writes again are never a data race,
	 * so plain (non-volatile) is correct here regardless of the
	 * two-thread access pattern. */
	uint32_t song_start_block;
	uint32_t song_block_count;
	uint32_t frames;
	uint32_t sector_count;
	bool loop_enabled;

	/*
	 * Mutable playback state -- plain (non-volatile, non-atomic), by
	 * design: every field below has exactly ONE writer thread (the
	 * real audio thread) for the entire lifetime of a selected song,
	 * per the ownership rule documented at the top of this file. The
	 * producer thread never reads OR writes any of these -- the
	 * cross-thread handoff lives entirely in st_stem_bufmbox.h's own
	 * atomic mailbox, not here.
	 */
	st_stream_state_t state;
	uint32_t song_frame;     /* the ONE authoritative absolute song frame */
	uint32_t ready_sector;   /* sector index this thread has locally confirmed ready, or ST_STREAM_NO_SECTOR */
	uint32_t underrun_count; /* diagnostic: UNDERRUN episodes (not ticks); never reset here */
	/*
	 * THE DIRECTION OF THIS HEAD. false = forward (every stream, always,
	 * before per-track reverse existed, and still every stream that is not
	 * the one reversed track).
	 *
	 * It lives here rather than in the caller because every function that
	 * moves song_frame has to agree about which way it moves, and a
	 * direction held outside the struct is a second source of truth that
	 * one of those functions will eventually be updated without.
	 *
	 * It is NOT part of the immutable geometry: the geometry above is
	 * written once at init and read by two threads, while this is mutable
	 * and audio-thread-owned like every other field in this block. The
	 * producer thread never reads it -- what to fetch NEXT for a reversed
	 * head is derived by the consumer and published through the mailbox's
	 * requested_sector exactly as a forward head's is.
	 */
	bool reverse;
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
 * Called once at boot, from a single-threaded boot context, strictly
 * before the producer thread's steady-state prefetch loop or the audio
 * thread's steady-state mixing loop begin touching this instance.
 *
 * Slice C3 adds exactly one other legal caller: audio_thread's own
 * post-commit runtime reload (main.c's looper_audio_block(), PASS C),
 * which re-runs this on the SAME `st_stream_t` instance mid-session, from
 * inside audio_thread itself, to pick up a newly uploaded song without a
 * reboot. This remains safe under the single-writer rule above: it is not
 * a second thread touching the instance, it is the instance's own sole
 * legitimate writer (audio_thread) reinitializing it synchronously on its
 * own thread, and only while stem_active is provably false for the whole
 * reload window (see g_stem_reload_req's own doc comment in main.c for
 * why) -- so no read of `st` by the same thread's mixing logic can ever
 * observe a mid-reinit state. Every OTHER caller must still treat this as
 * boot-only, single-shot initialization.
 */
bool st_stream_init(st_stream_t *st, uint32_t song_start_block, uint32_t song_block_count, uint32_t frames,
		     uint32_t sector_count, bool loop_enabled);

/* Which STSC sector index the CURRENT song_frame requires. Always
 * < st->sector_count by construction (st_stream_init()'s own geometry
 * check guarantees it) -- this function itself performs no bounds
 * checking because none can ever be needed. Audio-thread-only (reads
 * the audio-thread-owned song_frame). */
uint32_t st_stream_required_sector(const st_stream_t *st);

/*
 * Validates one freshly-read sector's header against this song's own
 * geometry and the sector index the caller claims to have physically
 * read (which need not already equal st_stream_required_sector() --
 * the producer may prefetch ahead of the playhead). Checked in this
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
 * st11_sector_read_header()'s own job. Reads ONLY the immutable geometry
 * fields (see this struct's own doc comment) -- safe to call from the
 * producer thread on a shared instance the audio thread also owns.
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
 * Audio-thread-only: called after the mailbox (st_stem_bufmbox.h)
 * confirms the sector is genuinely published, never by the producer.
 */
void st_stream_sector_ready(st_stream_t *st, uint32_t sector_index);

/*
 * Transport control, reusing the same "freeze, never reset" convention
 * main.c's own existing g_playing/p_w already follow: st_stream_play()
 * moves STOPPED -> PLAYING without touching song_frame (a no-op from any
 * other state); st_stream_stop() moves any state -> STOPPED, freezing
 * song_frame wherever it currently is. Audio-thread-only.
 */
void st_stream_play(st_stream_t *st);
void st_stream_stop(st_stream_t *st);

/*
 * Moves the playhead to an ARBITRARY frame inside the song, forward or
 * backward, without stopping and without restarting. This is what a global
 * loop's wrap and its exit both need: "play from exactly here, now."
 *
 * Returns false and changes nothing for a frame outside the song, so no
 * caller can seek beyond the committed region.
 *
 * Residency is INVALIDATED (ready_sector -> ST_STREAM_NO_SECTOR), because
 * the sector that was ready is almost certainly not the one this frame
 * lives in; decoding the old buffer at the new position is precisely the
 * stale-data failure a seek must never cause. The consumer re-acquires
 * from the mailbox on its next pass -- the same handling a loop wrap
 * already gets via ST_STREAM_TICK_LOOPED.
 *
 * A STOPPED stream stays stopped: seeking is a position change, not a
 * transport command. UNDERRUN and END_OF_SONG become PLAYING again, since
 * the position that could not be served is no longer where we are.
 *
 * Audio-thread-only, like every other mutator of this struct.
 */
bool st_stream_seek(st_stream_t *st, uint32_t frame);

/*
 * DIRECTION, and the two rules that make it a head rather than an effect.
 *
 * st_stream_set_reverse() does NOT move song_frame, does NOT stop, does NOT
 * restart, and does NOT invalidate residency. The frame the head is on is
 * still the frame it is on, and the group holding that frame is still the
 * group holding it -- turning around does not change either. That is the
 * whole of docs/stem-tape-per-track-reverse-spec.md's one rule: "Reverse
 * changes the direction of that track's head. It never changes its position."
 *
 * The one state it does touch is the pair of terminal states, and only in the
 * direction that frees the head: setting reverse=false lifts START_OF_SONG
 * back to PLAYING, and setting reverse=true lifts END_OF_SONG back to PLAYING
 * with song_frame pulled back to the last real frame (END_OF_SONG parks one
 * PAST the song, which is not a playable index). A head that has run out of
 * tape in one direction has not run out in the other.
 *
 * STOPPED is never lifted: direction is not a transport command.
 *
 * Audio-thread-only, like every other mutator here.
 */
void st_stream_set_reverse(st_stream_t *st, bool reverse);

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
	ST_STREAM_TICK_START_REACHED,    /* reverse only: retreated past frame 0 -- state is now
					   * START_OF_SONG and song_frame is CLAMPED at 0, the mirror of
					   * ST_STREAM_TICK_ENDED. Never wraps to the end of the song. */
} st_stream_tick_t;

/*
 * Called once per output audio frame while the caller wants playback to
 * advance. Never touches any sector buffer, and never touches the
 * mailbox, itself -- pure bookkeeping over this thread-owned struct;
 * the caller decodes+mixes the CURRENT song_frame using whatever buffer
 * holds st->ready_sector, before or after calling this (order does not
 * matter to this function, it only ever advances position). See the enum
 * above for the full set of outcomes. Audio-thread-only.
 */
st_stream_tick_t st_stream_advance_frame(st_stream_t *st);

/*
 * Advances by `count` frames in one step, for a caller that renders a RUN
 * of frames at a time instead of one at a time.
 *
 * PRECONDITION, which the caller must establish before calling and which
 * this function does not re-derive per frame: all `count` frames lie
 * inside the sector that is currently ready, and inside the song. A caller
 * computes exactly that by clamping its run to whichever comes first --
 * the end of the sector, the end of the song, or the end of its output
 * block. main.c's audio path is written that way.
 *
 * REVERSED, the same precondition mirrors exactly. A forward head at offset
 * `fis` inside its sector may consume at most (FRAMES_PER_SECTOR - fis)
 * frames and lands on the first frame of the NEXT sector; a backward head at
 * the same offset may consume at most (fis + 1) -- frames fis, fis-1, ... 0 --
 * and lands on the LAST frame of the previous one. Both are "as many frames
 * as remain in this sector in the direction of travel", which is the single
 * rule the caller's clamp actually implements.
 *
 * Given that precondition this is EXACTLY equivalent to calling
 * st_stream_advance_frame() `count` times: same final song_frame, same
 * state, same underrun_count, same terminal tick value. That equivalence
 * is not asserted, it is host-tested -- tests/test_stem_stream.c walks a
 * whole song both ways and compares the full state sequence and a hash of
 * the mixed audio, so the two can never silently diverge.
 *
 * WHY IT EXISTS: the per-frame form forced main.c's 48 kHz loop to
 * re-derive the needed sector (a division), re-test residency, and poll
 * the SPSC mailbox with a barriered atomic, once per output frame. All of
 * that is invariant across a run -- a run cannot cross a sector boundary
 * by construction -- so it was ~48000 repetitions a second of work whose
 * answer could not change. On this device that is not free: the eMMC read
 * path is CPU-bound, so audio-thread cycles convert directly into lost
 * stream throughput, which is what made stored playback run slow.
 *
 * count == 0 is a no-op and returns ST_STREAM_TICK_NOT_PLAYING.
 */
st_stream_tick_t st_stream_advance_frames(st_stream_t *st, uint32_t count);

#endif /* STEMTAPE_PLAYER_STEM_STREAM_H_ */
