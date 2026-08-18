/*
 * st_library_io.c — see st_library_io.h. PURE.
 */

#include "st_library_io.h"

#include <string.h>

#define ST_LIBIO_COPY_BYTES (ST_LIBRARY_HEADER_SECTORS_PER_COPY * ST_SECTOR_BYTES)

/* Static (not stack) work buffers: a serialized copy is 16 KiB (2 sectors)
 * and st_library_header_t itself (96 slots) is several KiB -- neither
 * belongs on a bounded thread stack, matching st_transfer.c's own
 * static-work-buffer precedent for exactly the same reason. Library
 * load/save is inherently serial (one transfer/commit at a time, audio
 * paused -- see main.c), so single static instances are safe without
 * additional locking, also matching st_transfer.c. */
static uint8_t s_libio_raw[ST_LIBIO_COPY_BYTES];
static st_library_header_t s_libio_tmp;

/* Wraparound-safe "is a newer than b" compare, matching the header's own
 * documented generation semantics. */
static bool gen_is_newer(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) > 0;
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

	read_ok0 = read_copy(0u, read_fn, ctx);
	valid0 = read_ok0 && st_library_header_deserialize(s_libio_raw, ST_LIBIO_COPY_BYTES, out);

	read_ok1 = read_copy(1u, read_fn, ctx);
	valid1 = read_ok1 &&
		 st_library_header_deserialize(s_libio_raw, ST_LIBIO_COPY_BYTES, &s_libio_tmp);

	if (valid0 && valid1) {
		if (gen_is_newer(s_libio_tmp.generation, out->generation)) {
			*out = s_libio_tmp;
			*trusted_copy = 1;
		} else {
			*trusted_copy = 0;
		}
		return ST_LIBIO_LOADED;
	}
	if (valid1) {
		*out = s_libio_tmp;
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
