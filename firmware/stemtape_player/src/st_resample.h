/*
 * st_resample.h -- converting between SOURCE frames and OUTPUT frames when the
 * transport is not running at 1x.
 *
 * ======================================================================
 * WHY THIS EXISTS AS ITS OWN FILE
 * ======================================================================
 * The stem audio path has one inner loop and six bounds on it: the run may
 * not leave the sector, the song, the loop window, the output block, the
 * seam's arm frame or the seam's jump frame. Every one of those was written
 * when output frames and source frames were THE SAME NUMBER, so the loop
 * could clamp once and use the answer for both.
 *
 * Tape inertia breaks that equality. At half speed a run of 100 source frames
 * fills 200 output slots. The six bounds do not all live in the same domain
 * any more -- sector/song/loop are limits on SOURCE, block/seam are limits on
 * OUTPUT -- and the conversion between them is the only genuinely new
 * arithmetic in the feature. It is small, it is exact, and getting it wrong
 * reads past the end of a sector buffer, so it is here on its own where it
 * can be host-tested against every rate and every starting phase rather than
 * buried in a 48 kHz loop.
 *
 * ======================================================================
 * THE CURSOR
 * ======================================================================
 * The playhead is (integer source frame, fractional offset). The integer part
 * is the stream's own song_frame, which nothing here touches. The fraction is
 * `frac_q16`, in [0, 65536), and it is the ONLY new piece of playback state.
 *
 * Output frame j is interpolated between the source frame BEHIND the cursor
 * and the source frame AT it -- deliberately backward-looking. A
 * forward-looking reader needs source frame c+1, which at the end of a run is
 * in the next sector and not resident; looking back needs only the previous
 * frame, which the renderer already decoded and carries. That choice is what
 * lets a run of ONE source frame still produce output instead of stalling.
 *
 * ======================================================================
 * RATE ABOVE 1x, AND ITS STRUCTURAL CEILING
 * ======================================================================
 * This used to clamp at exactly unity, because inertia is an envelope in 0..1
 * and nothing ever asked to go faster. The rocker's semitone control does: a
 * pitch above 0 reads the tape FASTER than the output is produced, so the
 * cursor can cross more than one source frame per output frame and the
 * renderer has to walk the frames it passes over rather than jumping them.
 *
 * ST_RS_RATE_MAX is 2x, and that is a STRUCTURAL bound, not a musical one:
 * two source frames per output frame is the most the renderer's carried
 * previous-frame can span in one step, and the run arithmetic below is
 * written against it. It is NOT the limit the player actually uses --
 * st_pitch.h caps pitch far below this, at what the eMMC read path can
 * sustain (about 1.19x). Two separate ceilings for two separate reasons: this
 * one says what the reader can do, that one says what the storage can feed.
 */
#ifndef ST_RESAMPLE_H
#define ST_RESAMPLE_H

#include <stdint.h>

/* Same fixed point as the inertia envelope: 65536 == 1.0 == source speed. */
#define ST_RS_ONE 65536u

/* The structural ceiling described above: two source frames per output frame. */
#define ST_RS_RATE_MAX (2u * ST_RS_ONE)

/* See "RATE ABOVE 1x" above. Zero is also refused: a rate of zero would emit
 * output forever without ever consuming a source frame, which is a stalled
 * transport, not a slow one. The inertia module declares the reel stopped
 * well above this. */
static inline uint32_t st_rs_rate_clamp(uint32_t rate_q16)
{
	if (rate_q16 > ST_RS_RATE_MAX) {
		return ST_RS_RATE_MAX;
	}
	if (rate_q16 == 0u) {
		return 1u;
	}
	return rate_q16;
}

/*
 * How many output frames this run of `src_frames` source frames can fill,
 * starting at cursor fraction `frac_q16`, at `rate_q16` (already clamped).
 *
 * The bound is `frac + out * rate <= src * ONE`: satisfy that and the highest
 * source index the renderer touches is src_frames - 1 and the frames consumed
 * are at most src_frames -- the run's own limit, which is what the caller's
 * sector/song/loop clamps established.
 *
 * NEVER RETURNS ZERO. A zero-length run would spin the caller's loop forever
 * in a real-time thread. The formula can produce zero in the degenerate corner
 * of a short run whose cursor has almost crossed it already, so it is floored
 * at one output frame.
 *
 * THAT FLOOR IS THE ONE PLACE THE BOUND CAN BE EXCEEDED, and above 1x it
 * genuinely can: one forced output frame at 1.9x from a one-frame run would
 * consume two. The renderer therefore clamps its own reads to the run it was
 * given rather than trusting this -- see stem_render_run(). Belt and braces,
 * because the cost of being wrong here is a read past a sector buffer in a
 * real-time thread on a part with no MMU.
 */
static inline uint32_t st_rs_out_frames(uint32_t src_frames, uint32_t frac_q16,
					 uint32_t rate_q16)
{
	uint32_t room, n;

	if (src_frames == 0u) {
		return 0u;
	}
	room = src_frames * ST_RS_ONE;
	if (frac_q16 >= room) {
		return 1u;
	}
	n = (room - frac_q16) / rate_q16;
	return (n == 0u) ? 1u : n;
}

/*
 * Convert a duration expressed in OUTPUT frames into the source frames the
 * playhead covers in that time. The seam's duck is a fixed number of output
 * frames; the run loop arms it by comparing SOURCE positions, so the two have
 * to be put in the same domain or the duck arms at the wrong moment and the
 * wrap fires early. Floored at 1 so the arm distance never collapses to zero
 * at a crawl.
 */
static inline uint32_t st_rs_src_for_out(uint32_t out_frames, uint32_t rate_q16)
{
	uint32_t n = (uint32_t)(((uint64_t)out_frames * rate_q16) / ST_RS_ONE);

	return (n == 0u) ? 1u : n;
}

#endif /* ST_RESAMPLE_H */
