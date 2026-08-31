/*
 * st_planar.h -- v1.2 song-planar addressing: each stem's whole timeline
 * contiguous in its own quarter of the song region.
 *
 * ======================================================================
 * WHY THE LAYOUT CHANGES AT ALL
 * ======================================================================
 * In v1.1 every 24-byte frame holds all four stems together, so a stem that
 * plays from a different point in the song than the others cannot be fetched
 * without fetching the whole sector -- 2,048 useful bytes out of 8,192.
 *
 * That is measured, not argued. On hardware, reversing a stem stored at a
 * sector END costs one extra read and ran clean (92% busy against an 83%
 * baseline, zero dropouts). Reversing one stored in the MIDDLE splits the
 * remaining three and costs about what two do -- which failed, with 742
 * dropouts at 99% busy. Under v1.1 the feature is therefore capped at two of
 * the four stems, and the requirement is all four.
 *
 * ======================================================================
 * WHAT PLANAR BUYS, AND WHY IT IS FREE
 * ======================================================================
 * With each stem contiguous, a stem is fetched entirely on its own. Where it
 * reads from -- forwards, backwards, scrubbed somewhere else entirely --
 * changes only the ADDRESS of its read. Never the number of reads, never the
 * bytes moved, never the cost. That is the whole point, and it is why the cost
 * is flat in WHICH stem diverges and in HOW MANY do.
 *
 * The catch, and the reason st_pl_plan_batch() exists: one group per stem per
 * span pays four fixed read costs where v1.1 paid one (5147 us, worse than the
 * four-stem failure). Batching N spans per read is legitimate precisely
 * because a stem's timeline is contiguous -- N spans of one stem are 4N
 * consecutive blocks, one read. At N=4 the device moves the same 16 blocks per
 * span for the same single fixed cost as today: 3178 us, unchanged.
 *
 * ======================================================================
 * A REVERSED STEM STILL READS FORWARD
 * ======================================================================
 * Reverse is a playback-order property, not a storage-access one. A batch for
 * a reversed stem covers groups [g-N+1, g] and is read as ONE ASCENDING block
 * run, exactly like a forward batch; only the order the consumer plays those
 * groups back in is reversed. Reading descending would break the contiguity
 * that makes the whole scheme cost nothing.
 *
 * ======================================================================
 * GEOMETRY PARITY WITH v1.1
 * ======================================================================
 * A group is 4 blocks: an 8-byte header plus 340 frames of 6 bytes.
 * 8 + 340*6 = 2048 exactly. Four groups at one group index carry the same 340
 * frames, so frames-per-group EQUALS v1.1's frames-per-sector and a song
 * occupies the same number of blocks. Every STIX geometry field carries over
 * untouched, and the static asserts in st_planar.c pin that rather than
 * trusting this comment.
 *
 * PURE: no I/O, no Zephyr, no allocation.
 */

#ifndef STEMTAPE_PLAYER_PLANAR_H_
#define STEMTAPE_PLAYER_PLANAR_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_sector_v11.h"
#include "st_v11_format.h"

#define ST_PL_STEMS            ST11_STEM_COUNT              /* 4 */
#define ST_PL_GROUP_BLOCKS     4u
#define ST_PL_GROUP_BYTES      (ST_PL_GROUP_BLOCKS * ST11_PHYSICAL_BLOCK_BYTES) /* 2048 */
#define ST_PL_HEADER_BYTES     8u
#define ST_PL_FRAME_BYTES      ST11_STEM_FRAME_BYTES        /* 6: L+R, 24-bit */
#define ST_PL_FRAMES_PER_GROUP ST11_FRAMES_PER_SECTOR       /* 340 -- v1.1 parity */

/* Blocks one group index costs across all four stems. Equals
 * ST11_BLOCKS_PER_SECTOR, which is what makes a v1.2 song exactly as long as
 * the v1.1 song it was converted from. */
#define ST_PL_BLOCKS_PER_GROUP_ALL (ST_PL_STEMS * ST_PL_GROUP_BLOCKS)

/* Group header: two magic bytes, the stem it belongs to, flags, then the group
 * index. Present so a GROUP-ONLY read is self-validating -- under planar every
 * read is a group read, including a diverging stem's, and a read with nothing
 * to check it against is a mis-address waiting to be played as audio. */
#define ST_PL_MAGIC_0 0x50u   /* 'P' */
#define ST_PL_MAGIC_1 0x4Cu   /* 'L' */

#define ST_PL_OFF_MAGIC_0    0u
#define ST_PL_OFF_MAGIC_1    1u
#define ST_PL_OFF_STEM       2u
#define ST_PL_OFF_FLAGS      3u
#define ST_PL_OFF_GROUP      4u   /* u32 LE */
#define ST_PL_OFF_FRAMES     ST_PL_HEADER_BYTES

typedef struct {
	uint32_t stem;
	uint32_t flags;
	uint32_t group_index;
} st_pl_header_t;

typedef enum {
	ST_PL_FWD = 0,
	ST_PL_REV = 1,
} st_pl_dir_t;

/* One stem's read for one batch. `blocks` is always a whole number of groups,
 * and the run is always ASCENDING -- see the reverse note above. */
typedef struct {
	uint32_t stem;
	uint32_t first_group;  /* lowest group index the read covers */
	uint32_t groups;       /* how many groups; 0 means nothing to read */
	uint32_t block;        /* absolute first block */
	uint32_t blocks;       /* groups * ST_PL_GROUP_BLOCKS */
} st_pl_read_t;

/* ---- geometry ---------------------------------------------------- */

/* Groups a song of `frames` occupies. Identical to v1.1's sector count. */
static inline uint32_t st_pl_groups_for_frames(uint32_t frames)
{
	return (frames + ST_PL_FRAMES_PER_GROUP - 1u) / ST_PL_FRAMES_PER_GROUP;
}

/* Blocks the whole song occupies, all four stems. Identical to v1.1's
 * sector_count * ST11_BLOCKS_PER_SECTOR. */
static inline uint32_t st_pl_song_blocks(uint32_t groups)
{
	return groups * ST_PL_BLOCKS_PER_GROUP_ALL;
}

/*
 * THE ADDRESS. Stem `stem`'s quarter starts at
 * song_start_block + stem * groups * ST_PL_GROUP_BLOCKS, and group
 * `group_index` sits ST_PL_GROUP_BLOCKS into it per group.
 *
 * Derived from `groups` rather than stored, so no new STIX field is needed:
 * the index already carries the song's frame count and sector count.
 */
uint32_t st_pl_group_block(uint32_t song_start_block, uint32_t groups,
			    uint32_t stem, uint32_t group_index);

/* ---- group header ------------------------------------------------ */

void st_pl_write_header(uint8_t group[ST_PL_GROUP_BYTES], uint32_t stem,
			 uint32_t group_index);

bool st_pl_read_header(const uint8_t group[ST_PL_GROUP_BYTES],
			st_pl_header_t *out);

/* True only when the group is well-formed AND is the exact one asked for.
 * Both halves matter: a well-formed group from the wrong address is precisely
 * the failure a per-group header exists to catch. */
bool st_pl_validate(const uint8_t group[ST_PL_GROUP_BYTES], uint32_t want_stem,
		     uint32_t want_group);

/* Byte offset of one frame's L sample inside a group buffer. */
static inline uint32_t st_pl_frame_off(uint32_t frame_in_group)
{
	return ST_PL_OFF_FRAMES + frame_in_group * ST_PL_FRAME_BYTES;
}

/* ---- decode -------------------------------------------------------- */

/*
 * ONE FRAME, FROM FOUR GROUPS, EACH AT ITS OWN POSITION.
 *
 * `frame_in_group[k]` is where stem k is reading, independently of the other
 * three. With all four equal this is bit-identical to
 * st11_sector_decode_frame() on the sector those groups came from, which a
 * test pins against the recorded song.
 *
 * PER-STEM INDICES FROM THE START, DELIBERATELY. Nothing in the v1.2 read path
 * needs them to differ -- all four stems stay together until reverse exists.
 * But per-track reverse is precisely "one stem's head is somewhere else", so
 * an interface taking a single shared index would have to be torn open again
 * to add it. This one does not change when reverse arrives; only its callers
 * start passing different numbers.
 *
 * The caller is responsible for each index being < ST_PL_FRAMES_PER_GROUP and
 * for each group holding the span that stem is actually reading. Bounds are
 * not re-checked here: this runs once per frame at 48 kHz, and the group
 * headers were validated when the group was fetched.
 */
/*
 * ONE STEM, ONE FRAME, out of that stem's own group.
 *
 * st_pl_decode_frame() below is four of these. This exists because the
 * resampler's "frame behind the cursor" is genuinely per-stem: when one stem
 * crosses a source frame the other three may not have, and once directions
 * differ the frame behind a reversed stem is the one at a HIGHER index.
 * Decoding all four there would be three stems' work thrown away, and three
 * stems read at a position that is not theirs.
 *
 * Same bounds contract as st_pl_decode_frame(): `frame_in_group` must be
 * < ST_PL_FRAMES_PER_GROUP and `group` must hold the span this stem is
 * reading. Not re-checked -- this runs inside the 48 kHz loop, and the group
 * header was validated when the group was fetched.
 */
void st_pl_decode_stem(const uint8_t *group, uint32_t frame_in_group,
			int32_t *out_l, int32_t *out_r);

void st_pl_decode_frame(const uint8_t *const groups[ST_PL_STEMS],
			 const uint32_t frame_in_group[ST_PL_STEMS],
			 st11_audio_frame_t *out);

/*
 * THE SAME DECODE WHEN ALL FOUR HEADS ARE TOGETHER -- which is every frame
 * until reverse exists.
 *
 * Not a convenience: building a four-element index array per frame, three
 * times per frame, inside a 48 kHz loop is real work spent representing a
 * distinction that does not exist yet. This takes the one index the caller
 * actually has.
 *
 * Bit-identical to st_pl_decode_frame() with all four indices equal, which is
 * what the tests assert rather than assume. When reverse arrives, the reversed
 * stem's caller moves to the array form and the other three can keep using
 * this.
 */
static inline void st_pl_decode_frame_shared(const uint8_t *const groups[ST_PL_STEMS],
					      uint32_t frame_in_group,
					      st11_audio_frame_t *out)
{
	const uint32_t idx[ST_PL_STEMS] = {
		frame_in_group, frame_in_group, frame_in_group, frame_in_group
	};

	st_pl_decode_frame(groups, idx, out);
}

/* ---- the read-ahead ring's geometry ------------------------------- */

/*
 * G AND R, THE TWO NUMBERS THE READ PATH IS SIZED BY. See
 * docs/stem-tape-v1.2-planar-format.md for the derivation; the short version:
 *
 *   RAM     = 4 stems x G x 2048. At G=6 that is 49,152 B -- byte for byte
 *             what the v1.1 sector ring cost, so the format change is
 *             RAM-NEUTRAL and reshapes [6][8192] into [4][6][2048].
 *   DEPTH   = G - R spans always buffered. A single worst-case fetch was
 *             MEASURED at 21.6-23.6 ms under load against a 7.083 ms span, so
 *             fewer than 4 buffered is thinner than one observed stall.
 *   COST    = one read per stem per refill, 3834 us/span at R=2. 92% busy
 *             against today's 83%, and 92% is an operating point this device
 *             has already run with zero silence frames.
 *   R | G   = REQUIRED, and the rule that decided these values. A batch is one
 *             emmc_read_blocks() only while its destination groups are
 *             contiguous, and slot is group % G, so a run that crosses the end
 *             of the ring is two reads. When R divides G, runs aligned to a
 *             multiple of R never cross it. G=7/R=3 looked 3 points cheaper
 *             and is not: its batches cycle 3,3,1.
 *
 * G is not free to differ from the mailbox's own depth -- the mailbox IS the
 * ring -- so it is taken from there rather than restated, and asserted below.
 */
#define ST_PL_REFILL_GROUPS 2u

/* How many groups tile one v1.1 sector's worth of bytes -- four, one per
 * stem. Used where something still needs 8192 contiguous bytes out of a
 * group-shaped pool. */
#define ST_PL_GROUPS_PER_SECTOR (ST11_SECTOR_BYTES / ST_PL_GROUP_BYTES)

_Static_assert(ST_PL_FRAMES_PER_GROUP == ST11_FRAMES_PER_SECTOR,
		"a group spans exactly what a v1.1 sector spanned");

/* ---- conversion from v1.1 ---------------------------------------- */

/*
 * ONE v1.1 SECTOR -> FOUR v1.2 GROUPS, carrying the same 340 frames.
 *
 * This is the whole migration, expressed once: the companion performs it when
 * encoding an upload, and the fixture test performs it to prove the audio
 * survives. Nothing else about a song changes.
 *
 * The sample bytes are MOVED, NOT RECOMPUTED -- the six bytes of stem k's
 * frame f are copied verbatim out of the v1.1 frame into group k. That is why
 * every per-stem checksum is unaffected by the format change, and the fixture
 * test checks exactly that against the companion's own declared values rather
 * than against a number this code produced.
 *
 * Frames past the sector's own `frame_count` are left as silence, matching
 * v1.1's short final sector.
 *
 * Returns false, writing nothing, if the sector's header does not parse.
 */
bool st_pl_from_v11_sector(const uint8_t sector[ST11_SECTOR_BYTES],
			    uint8_t groups[ST_PL_STEMS][ST_PL_GROUP_BYTES]);

/* ---- batched read plan ------------------------------------------- */

/*
 * ONE READ PER STEM, for `groups_per_batch` groups each.
 *
 * `head_group[k]` is the group stem k needs NEXT, and `dir[k]` which way it is
 * playing. A forward stem's batch runs [head, head+N); a reversed stem's runs
 * (head-N, head] -- the same N groups it is about to play, ordered ascending
 * on the device.
 *
 * THE COST DOES NOT DEPEND ON EITHER ARRAY. Every stem gets exactly one read
 * of the same size whatever its head or direction, which is the property the
 * whole format exists for. A test pins it.
 *
 * Returns the number of entries written (always ST_PL_STEMS, though a stem
 * whose batch falls entirely outside the song gets groups == 0).
 */
uint32_t st_pl_plan_batch(st_pl_read_t out[ST_PL_STEMS],
			   uint32_t song_start_block, uint32_t groups,
			   const uint32_t head_group[ST_PL_STEMS],
			   const st_pl_dir_t dir[ST_PL_STEMS],
			   uint32_t groups_per_batch);

#endif /* STEMTAPE_PLAYER_PLANAR_H_ */
