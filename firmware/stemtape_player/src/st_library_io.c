/*
 * st_library_io.c — see st_library_io.h. PURE.
 */

#include "st_library_io.h"

#include <string.h>

#define ST_LIBIO_COPY_BYTES (ST_LIBRARY_HEADER_SECTORS_PER_COPY * ST_SECTOR_BYTES)

/* Static (not stack) work buffer: a serialized copy occupies
 * ST_LIBRARY_HEADER_SECTORS_PER_COPY sectors -- this does not belong on a
 * bounded thread stack, matching st_transfer.c's own static-work-buffer
 * precedent for exactly the same reason. Library load/save is inherently
 * serial (one transfer/commit at a time, audio paused -- see main.c), so
 * a single static instance is safe without additional locking, also
 * matching st_transfer.c.
 *
 * Deliberately only ONE raw buffer and NO second `st_library_header_t`
 * scratch instance: st_library_header_t is itself several KiB (grows with
 * ST_MAX_SLOTS), and st_libio_load() below only ever needs ONE candidate
 * fully deserialized into `*out` at a time -- see its comment for how the
 * generation compare is done from the raw bytes directly, before
 * committing to a real (all-or-nothing, per
 * st_library_header_deserialize()'s own contract) deserialize into `*out`. */
static uint8_t s_libio_raw[ST_LIBIO_COPY_BYTES];

/* Wraparound-safe "is a newer than b" compare, matching the header's own
 * documented generation semantics. */
static bool gen_is_newer(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) > 0;
}

/* Reads just the `generation` field (bytes 8..11, little-endian -- see
 * st_storage_layout.c's serialize field order: magic, layout_version,
 * generation, ...) directly out of a raw, not-yet-validated copy buffer.
 * This is a PEEK, not a validation -- it exists only to decide which
 * candidate is worth the real, fully-validated
 * st_library_header_deserialize() attempt below; a garbage/corrupt buffer
 * peeking a garbage generation is harmless, because deserialize() itself
 * still gates whether that candidate is ever actually used. */
static uint32_t peek_generation(const uint8_t *raw)
{
	return (uint32_t)raw[8] | ((uint32_t)raw[9] << 8) |
	       ((uint32_t)raw[10] << 16) | ((uint32_t)raw[11] << 24);
}

static bool read_copy(uint32_t copy_index, st_sector_read_fn read_fn, void *ctx)
{
	uint32_t base = ST_LIBRARY_HEADER_SECTOR0 + copy_index * ST_LIBRARY_HEADER_SECTORS_PER_COPY;
	uint32_t i;

	for (i = 0; i < ST_LIBRARY_HEADER_SECTORS_PER_COPY; i++) {
		if (read_fn(base + i, s_libio_raw + (uint64_t)i * ST_SECTOR_BYTES, ctx) != 0) {
			return false;
		}
	}
	return true;
}

static bool write_copy(uint32_t copy_index, const uint8_t *buf, st_sector_write_fn write_fn,
			void *ctx)
{
	uint32_t base = ST_LIBRARY_HEADER_SECTOR0 + copy_index * ST_LIBRARY_HEADER_SECTORS_PER_COPY;
	uint32_t i;

	for (i = 0; i < ST_LIBRARY_HEADER_SECTORS_PER_COPY; i++) {
		if (write_fn(base + i, buf + (uint64_t)i * ST_SECTOR_BYTES, ctx) != 0) {
			return false;
		}
	}
	return true;
}

static void build_fresh(st_library_header_t *out, uint32_t slot_count_if_fresh)
{
	memset(out, 0, sizeof(*out));
	out->magic = ST_LIBRARY_HEADER_MAGIC;
	out->layout_version = ST_STORAGE_LAYOUT_VERSION;
	out->generation = 0u;
	out->slot_count = (slot_count_if_fresh > ST_MAX_SLOTS) ? ST_MAX_SLOTS : slot_count_if_fresh;
	out->current_slot = 0u;
	/* every slot[] entry is already zeroed (frame_count == 0 == empty slot) */
}

st_libio_load_result_t st_libio_load(st_library_header_t *out, uint32_t slot_count_if_fresh,
				      int *trusted_copy, st_sector_read_fn read_fn, void *ctx)
{
	bool read_ok0, read_ok1;
	bool valid0, valid1;
	bool attempt1;

	/* Copy 0: deserialize (all-or-nothing) directly into *out. On failure
	 * *out is left completely untouched, per st_library_header_deserialize()'s
	 * own contract. */
	read_ok0 = read_copy(0u, read_fn, ctx);
	valid0 = read_ok0 && st_library_header_deserialize(s_libio_raw, ST_LIBIO_COPY_BYTES, out);

	/* Copy 1: only worth a real (all-or-nothing) deserialize attempt into
	 * *out if it could actually win -- either copy 0 wasn't valid, or
	 * copy 1's peeked generation is newer than the copy 0 generation
	 * already sitting in *out. A failed attempt here leaves *out exactly
	 * as it was (still copy 0's data, if valid0), so it's always safe to
	 * try opportunistically. */
	read_ok1 = read_copy(1u, read_fn, ctx);
	valid1 = false;
	if (read_ok1) {
		attempt1 = !valid0 || gen_is_newer(peek_generation(s_libio_raw), out->generation);
		if (attempt1) {
			valid1 = st_library_header_deserialize(s_libio_raw, ST_LIBIO_COPY_BYTES, out);
		}
	}

	if (valid1) {
		*trusted_copy = 1;
		return ST_LIBIO_LOADED;
	}
	if (valid0) {
		*trusted_copy = 0;
		return ST_LIBIO_LOADED;
	}

	*trusted_copy = -1;
	if (read_ok0 || read_ok1) {
		build_fresh(out, slot_count_if_fresh);
		return ST_LIBIO_FRESH;
	}
	return ST_LIBIO_READ_FAILED;
}

bool st_libio_save(st_library_header_t *h, int trusted_copy,
		    st_sector_write_fn write_fn, void *ctx)
{
	uint32_t serialized;
	uint32_t first_copy, second_copy;

	h->generation++;
	serialized = st_library_header_serialize(h, s_libio_raw, ST_LIBIO_COPY_BYTES);
	if (serialized == 0u) {
		return false;
	}
	if (serialized < ST_LIBIO_COPY_BYTES) {
		memset(s_libio_raw + serialized, 0, ST_LIBIO_COPY_BYTES - serialized);
	}

	/* Write the currently-UNTRUSTED (or, if neither was trusted, copy 0)
	 * copy first, then the previously-trusted copy second -- see the
	 * header comment's torn-write guarantee. */
	if (trusted_copy == 0) {
		first_copy = 1u; second_copy = 0u;
	} else if (trusted_copy == 1) {
		first_copy = 0u; second_copy = 1u;
	} else {
		first_copy = 0u; second_copy = 1u;
	}

	if (!write_copy(first_copy, s_libio_raw, write_fn, ctx)) {
		return false;
	}
	if (!write_copy(second_copy, s_libio_raw, write_fn, ctx)) {
		return false;
	}
	return true;
}
