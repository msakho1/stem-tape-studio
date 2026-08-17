/*
 * st_storage_layout.h — Stem Tape standalone player: on-eMMC library layout.
 *
 * PURE: constants and plain structs only, no I/O, no Zephyr. Single source
 * of truth for st_transfer.c, the companion tool (mirrored in
 * docs/stem-tape-transfer-v1.md and stem-tape-transfer-v1-fixtures.json),
 * and the (deferred) playback engine.
 *
 * Storage version: ST_STORAGE_LAYOUT_VERSION 1.
 *
 * Layout, in sectors (ST_SECTOR_BYTES each), from sector 0:
 *
 *   [0 .. ST_LIBRARY_HEADER_SECTORS)   library header (2 copies, torn-write
 *                                      safe — see st_library_header_t)
 *   [ST_STAGING_SECTOR0 ..
 *    +ST_STAGING_SECTOR_COUNT)         upload staging region (one song's
 *                                      worth; see docs/stem-tape-transfer-v1.md
 *                                      section 6). NEVER visible to playback.
 *   [ST_SONG_DATA_SECTOR0 .. end)      committed song payloads, one
 *                                      contiguous, sector-aligned run per
 *                                      slot, allocated by
 *                                      st_storage_compute_slot_capacity()
 *                                      against the device's actual reported
 *                                      capacity — never a UI-hardcoded count.
 *
 * The classic SP-1 Tape Looper's own 512-byte-block mono-loop format
 * (firmware/src/main.c, EMMC_BLOCK_SIZE / SLOT0_BLOCK / TRACK_BLOCKS) is a
 * completely different, incompatible layout. This header defines a disjoint
 * address space and never reads or writes a classic-looper block address.
 */

#ifndef STEMTAPE_PLAYER_STORAGE_LAYOUT_H_
#define STEMTAPE_PLAYER_STORAGE_LAYOUT_H_

#include <stdbool.h>
#include <stdint.h>

#define ST_STORAGE_LAYOUT_VERSION 1u

/* ---- sector / audio format --------------------------------------------
 * timknapen/SP-1-dev wiki "Audio format" / "Data Structure": 8192-byte
 * sectors, 24-bit PCM, 48 kHz, four stereo stems. Never downgraded. */
#define ST_SECTOR_BYTES         8192u
#define ST_SAMPLE_RATE_HZ       48000u
#define ST_STEM_COUNT           4u   /* Vocal, Drums, Bass, Instrument */
#define ST_CHANNELS_PER_STEM    2u   /* stereo */
#define ST_BYTES_PER_SAMPLE     3u   /* 24-bit packed, little-endian */
#define ST_FRAME_BYTES          (ST_STEM_COUNT * ST_CHANNELS_PER_STEM * ST_BYTES_PER_SAMPLE) /* 24 */

/* Stem indices, fixed order (docs/FIRMWARE_CONTRACT_V1.md section 2). */
#define ST_STEM_VOCAL       0u
#define ST_STEM_DRUMS       1u
#define ST_STEM_BASS        2u
#define ST_STEM_INSTRUMENT  3u

/* ---- library header -----------------------------------------------------
 * Two copies (mirrors firmware/src/main.c's META_BLOCK / META_BLOCKS torn-
 * write-safe pattern): a reader trusts the copy with the higher valid
 * `generation`; a writer always writes the OTHER copy first, then the
 * copy the reader currently trusts, so a power loss mid-write leaves at
 * least one fully-valid copy. An unreadable/invalid header (both copies
 * bad, or first boot) means "zero songs, read-only until ST_XFER 'I' is
 * explicitly sent" -- NEVER an implicit reformat. */
#define ST_LIBRARY_HEADER_MAGIC     0x53544C31u /* 'STL1' */
#define ST_LIBRARY_HEADER_SECTORS   2u  /* one sector per copy */

#define ST_MAX_SLOTS 256u /* hard array cap; real count is capacity-detected */

typedef struct {
	uint32_t song_id_hash;     /* companion-tool-assigned stable id */
	uint32_t frame_count;      /* 0 = slot empty/uncommitted */
	uint32_t start_sector;     /* absolute sector, valid iff frame_count != 0 */
	uint32_t payload_crc32;
	uint8_t  stem_present_mask;   /* bit i set = stem i has audio (else silent) */
	uint8_t  stem_mute_mask;      /* bit i set = stem i muted */
	uint8_t  stem_solo_mask;      /* bit i set = stem i soloed */
	uint8_t  stem_link_mask;      /* bit i set = stem i linked (see FIRMWARE_CONTRACT_V1) */
	uint8_t  active_stem;         /* 0..ST_STEM_COUNT-1 */
	uint8_t  stem_gain_q8[ST_STEM_COUNT];    /* 0..255, fixed-point 0..1.0 per stem fader */
	uint8_t  master_volume_q8;               /* 0..255 */
	uint8_t  scrub_speed_index;              /* 0..3, see st_scrub.h */
	/* FX: two scopes, one active bank/algorithm/macro/latch each. */
	uint8_t  fx_stem_bank;    uint8_t fx_stem_algorithm;   uint8_t fx_stem_macro_q8;   bool fx_stem_latched;
	uint8_t  fx_global_bank;  uint8_t fx_global_algorithm; uint8_t fx_global_macro_q8; bool fx_global_latched;
	uint32_t reserved[2];     /* zero; future-safe like the looper's tail-appended fields */
} st_slot_meta_t;

typedef struct {
	uint32_t magic;         /* ST_LIBRARY_HEADER_MAGIC */
	uint32_t layout_version;
	uint32_t generation;    /* monotonically increasing; higher wins on read */
	uint32_t slot_count;    /* capacity-detected at init time, <= ST_MAX_SLOTS */
	uint32_t current_slot;  /* persisted "current song", index into slot[] */
	st_slot_meta_t slot[ST_MAX_SLOTS];
	uint32_t header_crc32;  /* over every prior byte of this struct */
} st_library_header_t;

/* ---- staging region -----------------------------------------------------
 * One song's worth of headroom for an in-flight upload, sized for the
 * largest song this build supports (ST_MAX_SONG_SECONDS). Disjoint from
 * every committed slot's sectors -- an in-progress upload can never
 * overlap, let alone corrupt, a song a listener could currently be
 * playing. */
#define ST_MAX_SONG_SECONDS 600u /* 10 minutes; a firmware policy ceiling, not a hardware limit */
#define ST_STAGING_SECTOR_COUNT \
	(((uint64_t)ST_MAX_SONG_SECONDS * ST_SAMPLE_RATE_HZ * ST_FRAME_BYTES + ST_SECTOR_BYTES - 1u) \
	 / ST_SECTOR_BYTES)

#define ST_LIBRARY_HEADER_SECTOR0 0u
#define ST_STAGING_SECTOR0        (ST_LIBRARY_HEADER_SECTOR0 + ST_LIBRARY_HEADER_SECTORS)
#define ST_SONG_DATA_SECTOR0      (ST_STAGING_SECTOR0 + ST_STAGING_SECTOR_COUNT)

/*
 * Capacity-detected slot count: NOT a UI-hardcoded number. Given the
 * device's total usable sectors (post header+staging reservation) and an
 * average expected song length, returns how many slots the library header
 * should allocate room for, clamped to ST_MAX_SLOTS. The companion tool
 * and firmware both call this against the SAME reported eMMC capacity, so
 * they can never disagree about how many slots exist.
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
	avg_song_sectors = ((uint64_t)avg_song_seconds * ST_SAMPLE_RATE_HZ * ST_FRAME_BYTES +
			     ST_SECTOR_BYTES - 1u) / ST_SECTOR_BYTES;
	if (avg_song_sectors == 0u) {
		avg_song_sectors = 1u;
	}
	n = usable / avg_song_sectors;
	return (n > ST_MAX_SLOTS) ? ST_MAX_SLOTS : (uint32_t)n;
}

/* Sector span a song of `frame_count` frames occupies. */
static inline uint32_t st_storage_song_sectors(uint32_t frame_count)
{
	uint64_t bytes = (uint64_t)frame_count * ST_FRAME_BYTES;

	return (uint32_t)((bytes + ST_SECTOR_BYTES - 1u) / ST_SECTOR_BYTES);
}

#endif /* STEMTAPE_PLAYER_STORAGE_LAYOUT_H_ */
