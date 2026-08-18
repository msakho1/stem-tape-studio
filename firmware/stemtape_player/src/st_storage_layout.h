/*
 * st_storage_layout.h — Stem Tape standalone player: on-eMMC library layout.
 *
 * STORAGE LAYOUT VERSION 2 — corrects v1's overflow bug (sizeof(header) was
 * 11,288 bytes crammed into one 8,192-byte sector) with an EXPLICITLY
 * SERIALIZED, bounds-checked wire format: the on-disk layout is defined by
 * st_storage_layout.c's byte-packing functions, never by raw C struct
 * layout (which is compiler/ABI-dependent and was the root cause of the
 * mismatch between the "fits in one sector" assumption and reality).
 *
 * PURE: constants, plain structs, and inline byte-conversion helpers only —
 * no I/O, no Zephyr.
 *
 * Address space, all in ST_SECTOR_BYTES (8192-byte) LOGICAL sectors — the
 * ONE canonical resume/addressing unit used everywhere (this header, the
 * protocol doc, the fixtures, and the companion tool). The physical eMMC
 * driver speaks 512-byte blocks; st_storage_sector_to_block() is the ONLY
 * place that conversion happens, and it is checked (see below):
 *
 *   [0 .. ST_LIBRARY_HEADER_SECTORS)     library header, ST_LIBRARY_HEADER_COPIES
 *                                        redundant copies of
 *                                        ST_LIBRARY_HEADER_SECTORS_PER_COPY
 *                                        sectors each (generation+CRC32
 *                                        selects the trusted copy on read;
 *                                        a write always updates the OTHER
 *                                        copy first, then the trusted one)
 *   [ST_STAGING_SECTOR0 ..
 *    +ST_STAGING_SECTOR_COUNT)           upload staging region, one song's
 *                                        worth. Never visible to playback.
 *   [ST_SONG_DATA_SECTOR0 .. end)        committed song payloads
 *
 * This whole region starts at ST_STORAGE_BASE_BLOCK (512-byte blocks), the
 * SAME safe offset the classic SP-1 Tape Looper's own on-flash format
 * already uses for its first data block (firmware/src/main.c's
 * SLOT0_BLOCK) [looper a8dd127:796] — 2 MiB in, past every bootloader/
 * stock-firmware-reserved block (0..4095). This is a SEPARATE, disjoint
 * region from the classic looper's own 512-byte-block mono-loop format;
 * this header never reads or writes a classic-looper block address, and
 * the classic looper's format never reads or writes here.
 */

#ifndef STEMTAPE_PLAYER_STORAGE_LAYOUT_H_
#define STEMTAPE_PLAYER_STORAGE_LAYOUT_H_

#include <stdbool.h>
#include <stdint.h>

#define ST_STORAGE_LAYOUT_VERSION 2u

/* ---- sector / audio format --------------------------------------------
 * [wiki timknapen/SP-1-dev "Data Structure"]: "The flash memory of the
 * SP-1 is divided up into 8192 (0x2000) byte sectors."
 * [wiki timknapen/SP-1-dev "Audio format"]: 48 kHz, 24-bit, 8 channels
 * (4 stereo stems) -- see st_sector_codec.h for the exact documented
 * frame/sub-block packing this sector size implies. */
#define ST_SECTOR_BYTES         8192u
#define ST_SAMPLE_RATE_HZ       48000u
#define ST_STEM_COUNT           4u   /* Vocal, Drums, Bass, Instrument */
#define ST_CHANNELS_PER_STEM    2u   /* stereo */

/* Stem indices, fixed order (docs/FIRMWARE_CONTRACT_V1.md section 2). */
#define ST_STEM_VOCAL       0u
#define ST_STEM_DRUMS       1u
#define ST_STEM_BASS        2u
#define ST_STEM_INSTRUMENT  3u

/* ---- canonical block/sector addressing -----------------------------------
 * [looper a8dd127:796]: SLOT0_BLOCK = 4096 (512-byte blocks), "2MB-aligned
 * ... so every trk_blk stays 2MB-aligned" -- the proven safe first data
 * block on this hardware, past the bootloader/stock-reserved region. */
#define ST_STORAGE_BASE_BLOCK 4096u   /* 512-byte blocks; [looper a8dd127:796] */
#define ST_EMMC_BLOCK_BYTES   512u
#define ST_BLOCKS_PER_SECTOR  (ST_SECTOR_BYTES / ST_EMMC_BLOCK_BYTES) /* 16 */

/* The ONLY conversion from a logical sector to a physical 512-byte eMMC
 * block. Checked: fails (returns false, *block_out unmodified) rather than
 * silently wrapping on an address that would not fit a uint32_t block
 * number. */
static inline bool st_storage_sector_to_block(uint32_t logical_sector, uint32_t *block_out)
{
	uint64_t block = (uint64_t)ST_STORAGE_BASE_BLOCK +
			  (uint64_t)logical_sector * ST_BLOCKS_PER_SECTOR;

	if (block > 0xFFFFFFFFull) {
		return false;
	}
	*block_out = (uint32_t)block;
	return true;
}

/* ---- library header, EXPLICITLY SERIALIZED (see st_storage_layout.c) ----
 *
 * Two redundant copies, each ST_LIBRARY_HEADER_SECTORS_PER_COPY sectors,
 * exactly mirroring the classic looper's own META_BLOCK/META_BLOCKS
 * torn-write-safe pattern: a reader trusts whichever copy has valid magic
 * + a matching header_crc32 AND the higher `generation`; a writer updates
 * the OTHER (untrusted) copy first, then the previously-trusted one, so a
 * power loss mid-write always leaves at least one fully valid copy.
 *
 * ST_SLOT_RECORD_BYTES is the FIXED serialized size of one song's
 * metadata (see st_slot_meta_t below) -- deliberately larger than the
 * fields currently defined (133 bytes) for future-safety, exactly like
 * the classic looper's own tail-appended meta_blk fields.
 */
#define ST_LIBRARY_HEADER_MAGIC     0x53544C32u /* 'STL2' -- bumped with the layout version */
#define ST_LIBRARY_HEADER_FIXED_BYTES 24u /* magic(4)+layout_version(4)+generation(4)+
					    * slot_count(4)+current_slot(4)+header_crc32(4) */
#define ST_SLOT_RECORD_BYTES        144u  /* actual fields sum to 133; see st_storage_layout.c */

#define ST_MAX_SLOTS 96u /* a deliberate FIRMWARE POLICY ceiling (not a hardware limit) --
			   * see the overflow this replaces: 256 slots at the old ad-hoc
			   * struct size overflowed a single sector by 3096 bytes.
			   * st_storage_compute_slot_capacity() still clamps DOWN from
			   * here based on real reported device capacity. */

#define ST_LIBRARY_HEADER_SERIALIZED_MAX_BYTES \
	(ST_LIBRARY_HEADER_FIXED_BYTES + (uint64_t)ST_MAX_SLOTS * ST_SLOT_RECORD_BYTES)

#define ST_LIBRARY_HEADER_SECTORS_PER_COPY 2u
#define ST_LIBRARY_HEADER_COPIES           2u
#define ST_LIBRARY_HEADER_SECTORS \
	(ST_LIBRARY_HEADER_SECTORS_PER_COPY * ST_LIBRARY_HEADER_COPIES)

/* Compile-time proof the worst case (every one of ST_MAX_SLOTS slots
 * populated) fits its reserved sectors, per copy. This is the assertion
 * the v1 layout was missing. */
#if !defined(__cplusplus)
_Static_assert(ST_LIBRARY_HEADER_SERIALIZED_MAX_BYTES <=
		       (uint64_t)ST_LIBRARY_HEADER_SECTORS_PER_COPY * ST_SECTOR_BYTES,
	       "library header (worst case) overflows its reserved sectors per copy");
#endif

typedef struct {
	uint32_t song_id_hash;      /* companion-tool-assigned stable id */
	uint32_t frame_count;       /* 0 = slot empty/uncommitted; shared duration,
				      * all 4 stems are frame-interleaved together
				      * (see st_sector_codec.h) so they share one count */
	uint32_t start_sector;      /* first logical song-data sector; valid iff frame_count != 0 */
	uint32_t stem_content_frames[ST_STEM_COUNT]; /* per-stem REAL content length; the
						       * remainder up to frame_count is silence
						       * (a stem may end before the others) */
	uint32_t stem_crc32[ST_STEM_COUNT];          /* per-stem checksum, computed over just
						       * that stem's decoded L/R samples across
						       * every frame -- independent of the others,
						       * even though they share physical sectors */
	uint16_t bpm_q8;             /* BPM as an 8.8 fixed point; 0 = not detected/unknown */
	uint32_t downbeat_frame;     /* frame offset of the first downbeat; 0 = unknown */
	uint8_t  stem_present_mask;  /* bit i set = stem i has audio (else silent) */
	uint8_t  stem_mute_mask;
	uint8_t  stem_solo_mask;
	uint8_t  stem_link_mask;
	uint8_t  active_stem;        /* 0..ST_STEM_COUNT-1 */
	uint8_t  stem_gain_q8[ST_STEM_COUNT];
	uint8_t  master_volume_q8;
	uint8_t  scrub_speed_index;  /* 0..3, see st_scrub.h */
	uint8_t  fx_stem_bank;    uint8_t fx_stem_algorithm;   uint8_t fx_stem_macro_q8;   uint8_t fx_stem_latched;
	uint8_t  fx_global_bank;  uint8_t fx_global_algorithm; uint8_t fx_global_macro_q8; uint8_t fx_global_latched;
	char     title[32];   /* null-terminated; zero-padded */
	char     artist[32];  /* null-terminated; zero-padded */
} st_slot_meta_t;

typedef struct {
	uint32_t magic;
	uint32_t layout_version;
	uint32_t generation;    /* monotonically increasing; higher (mod wraparound-safe
				  * comparison) wins on read */
	uint32_t slot_count;    /* capacity-detected at init time, <= ST_MAX_SLOTS */
	uint32_t current_slot;  /* persisted "current song", index into slot[] */
	st_slot_meta_t slot[ST_MAX_SLOTS]; /* only the first `slot_count` are ever
					     * serialized -- see st_storage_layout.c */
	uint32_t header_crc32;  /* over every serialized byte before this field */
} st_library_header_t;

#define ST_LIBRARY_HEADER_SECTOR0 0u
#define ST_STAGING_SECTOR0        (ST_LIBRARY_HEADER_SECTOR0 + ST_LIBRARY_HEADER_SECTORS)

/* ---- staging region -----------------------------------------------------
 * One song's worth of headroom for an in-flight upload, sized for the
 * largest song this build supports (ST_MAX_SONG_SECONDS). Disjoint from
 * every committed slot's sectors. */
#define ST_MAX_SONG_SECONDS 600u /* 10 minutes; a firmware policy ceiling, not a hardware limit */

/* SP-1 sector audio capacity: 340 frames/sector (see st_sector_codec.h),
 * NOT a raw byte-division -- replaces v1's `ceil(frames*24/8192)`, which
 * did not account for the 8 reserved timing/tempo/LED bytes per 2048-byte
 * sub-block (32 reserved bytes/sector total, out of 8192). */
#define ST_SECTOR_FRAME_CAPACITY 340u

#define ST_STAGING_SECTOR_COUNT \
	(((uint64_t)ST_MAX_SONG_SECONDS * ST_SAMPLE_RATE_HZ + ST_SECTOR_FRAME_CAPACITY - 1u) \
	 / ST_SECTOR_FRAME_CAPACITY)

#define ST_SONG_DATA_SECTOR0 (ST_STAGING_SECTOR0 + ST_STAGING_SECTOR_COUNT)

/*
 * Capacity-detected slot count: NOT a UI-hardcoded number. Given the
 * device's total usable sectors (post header+staging reservation) and an
 * average expected song length, returns how many slots the library header
 * should allocate room for, clamped to ST_MAX_SLOTS.
 */
static inline uint32_t st_storage_compute_slot_capacity(uint64_t total_sectors,
							  uint32_t avg_song_seconds)
{
	uint64_t usable;
	uint64_t avg_song_sectors;
	uint64_t n;

	if (total_sectors <= ST_SONG_DATA_SECTOR0) {
		return 0u;
	}
	if (avg_song_seconds == 0u) {
		avg_song_seconds = 180u; /* 3 minutes: a reasonable default estimate */
	}
	usable = total_sectors - ST_SONG_DATA_SECTOR0;
	avg_song_sectors = ((uint64_t)avg_song_seconds * ST_SAMPLE_RATE_HZ +
			     ST_SECTOR_FRAME_CAPACITY - 1u) / ST_SECTOR_FRAME_CAPACITY;
	if (avg_song_sectors == 0u) {
		avg_song_sectors = 1u;
	}
	n = usable / avg_song_sectors;
	return (n > ST_MAX_SLOTS) ? ST_MAX_SLOTS : (uint32_t)n;
}

/* Sector span a song of `frame_count` frames occupies, using the real SP-1
 * per-sector frame capacity (340), not a raw byte division. */
static inline uint32_t st_storage_song_sectors(uint32_t frame_count)
{
	return (uint32_t)(((uint64_t)frame_count + ST_SECTOR_FRAME_CAPACITY - 1u) /
			   ST_SECTOR_FRAME_CAPACITY);
}

/*
 * Song-data allocator: a simple monotonic ("log-structured", never
 * reused/compacted in this release) bump allocator computed directly from
 * the EXISTING committed slots -- no separate watermark field, so no wire-
 * format change is needed to add it. A freshly committed song's
 * start_sector is always st_storage_next_free_song_sector(h) BEFORE that
 * slot's own record is written; deleting a slot does not reclaim its
 * sectors (documented limitation -- a compacting allocator is out of
 * scope for this release, and re-uploading to a slot always gets a fresh
 * range past every other committed song, so the previously committed
 * song's bytes are simply never referenced again, never overwritten out
 * from under a still-playing stream).
 */
static inline uint32_t st_storage_next_free_song_sector(const st_library_header_t *h)
{
	uint32_t next = ST_SONG_DATA_SECTOR0;
	uint32_t i;

	for (i = 0; i < h->slot_count; i++) {
		if (h->slot[i].frame_count == 0u) {
			continue; /* empty slot: no sectors reserved */
		}
		uint32_t end = h->slot[i].start_sector +
				st_storage_song_sectors(h->slot[i].frame_count);
		if (end > next) {
			next = end;
		}
	}
	return next;
}

/* ---- explicit serialization (st_storage_layout.c) ---- */

/* Serializes the fixed header fields + exactly `h->slot_count` slot
 * records (NOT all ST_MAX_SLOTS -- compact, matches real capacity) into
 * `out`, which must be at least st_library_header_serialized_size(h)
 * bytes and no larger than ST_LIBRARY_HEADER_SECTORS_PER_COPY *
 * ST_SECTOR_BYTES. Returns the number of bytes written, or 0 on a
 * bounds/parameter failure (fails closed -- never writes a truncated or
 * out-of-bounds record). Sets `h->header_crc32` as a side effect (the CRC
 * covers every byte written before it).
 */
uint32_t st_library_header_serialize(st_library_header_t *h, uint8_t *out, uint32_t out_cap);

/* Computes the exact byte size st_library_header_serialize() would need
 * for `slot_count` slots -- used both to size the caller's buffer and by
 * the compile-time assertion's runtime-checkable counterpart. */
uint32_t st_library_header_serialized_size(uint32_t slot_count);

/* Deserializes and validates: magic, layout_version, an in-bounds
 * slot_count, and header_crc32 all must check out, or this returns false
 * and does not modify `*h` at all (fails closed on any corruption). */
bool st_library_header_deserialize(const uint8_t *in, uint32_t in_len, st_library_header_t *h);

#endif /* STEMTAPE_PLAYER_STORAGE_LAYOUT_H_ */
