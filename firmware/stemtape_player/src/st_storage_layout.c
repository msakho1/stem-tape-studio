/*
 * st_storage_layout.c — explicit, bounds-checked little-endian byte-packing
 * for st_library_header_t / st_slot_meta_t. See st_storage_layout.h.
 *
 * This is the file that actually defines the on-disk format. Nothing here
 * ever does `memcpy(&header, buf, sizeof(header))` or vice versa — every
 * field is packed/unpacked one at a time through the put_ and get_ helpers
 * below, so the wire size is exactly what this file writes, never whatever
 * the compiler happened to lay a struct out as (that mismatch is what
 * overflowed a single sector in layout v1).
 *
 * PURE: no I/O, no Zephyr.
 */

#include "st_storage_layout.h"

#include <string.h>

#include "st_crc32.h"

/* ---- little-endian primitive packing ---- */

static void put_u8(uint8_t **p, uint8_t v)
{
	*(*p)++ = v;
}

static uint8_t get_u8(const uint8_t **p)
{
	return *(*p)++;
}

static void put_u16le(uint8_t **p, uint16_t v)
{
	(*p)[0] = (uint8_t)(v & 0xFFu);
	(*p)[1] = (uint8_t)((v >> 8) & 0xFFu);
	*p += 2;
}

static uint16_t get_u16le(const uint8_t **p)
{
	uint16_t v = (uint16_t)((*p)[0] | ((uint16_t)(*p)[1] << 8));
	*p += 2;
	return v;
}

static void put_u32le(uint8_t **p, uint32_t v)
{
	(*p)[0] = (uint8_t)(v & 0xFFu);
	(*p)[1] = (uint8_t)((v >> 8) & 0xFFu);
	(*p)[2] = (uint8_t)((v >> 16) & 0xFFu);
	(*p)[3] = (uint8_t)((v >> 24) & 0xFFu);
	*p += 4;
}

static uint32_t get_u32le(const uint8_t **p)
{
	uint32_t v = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1] << 8) |
		     ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24);
	*p += 4;
	return v;
}

/* Fixed-width char buffer: copies up to `n` bytes, null-terminates within
 * `n` if the source is shorter, silently truncates (never overruns) if
 * longer. */
static void put_str(uint8_t **p, const char *s, uint32_t n)
{
	uint32_t len = 0;

	if (s != NULL) {
		while (len < n && s[len] != '\0') {
			len++;
		}
	}
	memcpy(*p, s, len);
	memset(*p + len, 0, n - len);
	*p += n;
}

static void get_str(const uint8_t **p, char *out, uint32_t n)
{
	memcpy(out, *p, n);
	out[n - 1] = '\0'; /* fail-closed: always terminated even if the source wasn't */
	*p += n;
}

/* ---- slot record: exactly ST_SLOT_RECORD_BYTES, verified below ---- */

#define ST_SLOT_RECORD_USED_BYTES 133u

#if !defined(__cplusplus)
_Static_assert(ST_SLOT_RECORD_USED_BYTES <= ST_SLOT_RECORD_BYTES,
	       "packed slot record fields overflow the declared record size");
#endif

static void slot_serialize(const st_slot_meta_t *s, uint8_t *out)
{
	uint8_t *p = out;
	uint32_t i;

	put_u32le(&p, s->song_id_hash);
	put_u32le(&p, s->frame_count);
	put_u32le(&p, s->start_sector);
	for (i = 0; i < ST_STEM_COUNT; i++) {
		put_u32le(&p, s->stem_content_frames[i]);
	}
	for (i = 0; i < ST_STEM_COUNT; i++) {
		put_u32le(&p, s->stem_crc32[i]);
	}
	put_u16le(&p, s->bpm_q8);
	put_u32le(&p, s->downbeat_frame);
	put_u8(&p, s->stem_present_mask);
	put_u8(&p, s->stem_mute_mask);
	put_u8(&p, s->stem_solo_mask);
	put_u8(&p, s->stem_link_mask);
	put_u8(&p, s->active_stem);
	for (i = 0; i < ST_STEM_COUNT; i++) {
		put_u8(&p, s->stem_gain_q8[i]);
	}
	put_u8(&p, s->master_volume_q8);
	put_u8(&p, s->scrub_speed_index);
	put_u8(&p, s->fx_stem_bank);
	put_u8(&p, s->fx_stem_algorithm);
	put_u8(&p, s->fx_stem_macro_q8);
	put_u8(&p, s->fx_stem_latched);
	put_u8(&p, s->fx_global_bank);
	put_u8(&p, s->fx_global_algorithm);
	put_u8(&p, s->fx_global_macro_q8);
	put_u8(&p, s->fx_global_latched);
	put_str(&p, s->title, sizeof(s->title));
	put_str(&p, s->artist, sizeof(s->artist));
	/* Reserved tail padding, explicit zero fill up to the fixed record size. */
	memset(p, 0, ST_SLOT_RECORD_BYTES - (uint32_t)(p - out));
}

static void slot_deserialize(const uint8_t *in, st_slot_meta_t *s)
{
	const uint8_t *p = in;
	uint32_t i;

	memset(s, 0, sizeof(*s));
	s->song_id_hash = get_u32le(&p);
	s->frame_count = get_u32le(&p);
	s->start_sector = get_u32le(&p);
	for (i = 0; i < ST_STEM_COUNT; i++) {
		s->stem_content_frames[i] = get_u32le(&p);
	}
	for (i = 0; i < ST_STEM_COUNT; i++) {
		s->stem_crc32[i] = get_u32le(&p);
	}
	s->bpm_q8 = get_u16le(&p);
	s->downbeat_frame = get_u32le(&p);
	s->stem_present_mask = get_u8(&p);
	s->stem_mute_mask = get_u8(&p);
	s->stem_solo_mask = get_u8(&p);
	s->stem_link_mask = get_u8(&p);
	s->active_stem = get_u8(&p);
	for (i = 0; i < ST_STEM_COUNT; i++) {
		s->stem_gain_q8[i] = get_u8(&p);
	}
	s->master_volume_q8 = get_u8(&p);
	s->scrub_speed_index = get_u8(&p);
	s->fx_stem_bank = get_u8(&p);
	s->fx_stem_algorithm = get_u8(&p);
	s->fx_stem_macro_q8 = get_u8(&p);
	s->fx_stem_latched = get_u8(&p);
	s->fx_global_bank = get_u8(&p);
	s->fx_global_algorithm = get_u8(&p);
	s->fx_global_macro_q8 = get_u8(&p);
	s->fx_global_latched = get_u8(&p);
	get_str(&p, s->title, sizeof(s->title));
	get_str(&p, s->artist, sizeof(s->artist));
	/* remaining reserved tail bytes ignored */
}

uint32_t st_library_header_serialized_size(uint32_t slot_count)
{
	if (slot_count > ST_MAX_SLOTS) {
		return 0u;
	}
	return ST_LIBRARY_HEADER_FIXED_BYTES + slot_count * ST_SLOT_RECORD_BYTES;
}

uint32_t st_library_header_serialize(st_library_header_t *h, uint8_t *out, uint32_t out_cap)
{
	uint8_t *p = out;
	uint32_t need;
	uint32_t i;
	uint32_t crc;

	if (h == NULL || out == NULL || h->slot_count > ST_MAX_SLOTS) {
		return 0u;
	}
	need = st_library_header_serialized_size(h->slot_count);
	if (need == 0u || need > out_cap ||
	    need > (uint64_t)ST_LIBRARY_HEADER_SECTORS_PER_COPY * ST_SECTOR_BYTES) {
		return 0u;
	}
	if (h->current_slot >= h->slot_count && h->slot_count != 0u) {
		return 0u;
	}

	put_u32le(&p, ST_LIBRARY_HEADER_MAGIC);
	put_u32le(&p, ST_STORAGE_LAYOUT_VERSION);
	put_u32le(&p, h->generation);
	put_u32le(&p, h->slot_count);
	put_u32le(&p, h->current_slot);
	for (i = 0; i < h->slot_count; i++) {
		slot_serialize(&h->slot[i], p);
		p += ST_SLOT_RECORD_BYTES;
	}

	/* CRC covers every byte written so far (fixed fields minus the CRC
	 * field itself, plus every slot record). */
	crc = st_crc32_compute(out, (uint32_t)(p - out));
	h->header_crc32 = crc;
	put_u32le(&p, crc);

	return (uint32_t)(p - out);
}

bool st_library_header_deserialize(const uint8_t *in, uint32_t in_len, st_library_header_t *h)
{
	const uint8_t *p = in;
	uint32_t magic;
	uint32_t layout_version;
	uint32_t generation;
	uint32_t slot_count;
	uint32_t current_slot;
	uint32_t need;
	uint32_t stored_crc;
	uint32_t computed_crc;
	uint32_t i;

	if (in == NULL || h == NULL || in_len < ST_LIBRARY_HEADER_FIXED_BYTES) {
		return false;
	}

	magic = get_u32le(&p);
	if (magic != ST_LIBRARY_HEADER_MAGIC) {
		return false;
	}
	layout_version = get_u32le(&p);
	if (layout_version != ST_STORAGE_LAYOUT_VERSION) {
		return false;
	}
	generation = get_u32le(&p);
	slot_count = get_u32le(&p);
	if (slot_count > ST_MAX_SLOTS) {
		return false;
	}
	current_slot = get_u32le(&p);
	if (slot_count != 0u && current_slot >= slot_count) {
		return false;
	}

	need = st_library_header_serialized_size(slot_count);
	if (need == 0u || in_len < need) {
		return false;
	}

	/* CRC over everything up to (not including) the trailing CRC field. */
	computed_crc = st_crc32_compute(in, need - 4u);
	{
		const uint8_t *crc_p = in + need - 4u;
		stored_crc = get_u32le(&crc_p);
	}
	if (computed_crc != stored_crc) {
		return false;
	}

	h->magic = magic;
	h->layout_version = layout_version;
	h->generation = generation;
	h->slot_count = slot_count;
	h->current_slot = current_slot;
	memset(h->slot, 0, sizeof(h->slot));
	for (i = 0; i < slot_count; i++) {
		slot_deserialize(p, &h->slot[i]);
		p += ST_SLOT_RECORD_BYTES;
	}
	h->header_crc32 = stored_crc;
	return true;
}
