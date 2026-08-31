/*
 * st_xfer_wire.c — see st_xfer_wire.h. PURE.
 */

#include "st_xfer_wire.h"

#include <string.h>

#include "st_storage_layout.h"
#include "st_transfer_protocol.h"

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

void st_xfer_wire_encode_version(uint8_t out[16], uint32_t capability_flags)
{
	uint8_t *p = out;

	memcpy(p, ST_XFER_VERSION_MAGIC, ST_XFER_VERSION_MAGIC_LEN);
	p += ST_XFER_VERSION_MAGIC_LEN;
	*p++ = (uint8_t)ST_XFER_PROTOCOL_MINOR;
	*p++ = (uint8_t)ST_STORAGE_LAYOUT_VERSION;
	*p++ = 0u; /* reserved */
	*p++ = 0u; /* reserved */
	put_u32le(&p, capability_flags);
	put_u32le(&p, ST_SECTOR_BYTES);
}

bool st_xfer_wire_decode_begin_req(const uint8_t *buf, uint32_t len,
				    uint16_t *slot, st_xfer_song_meta_t *meta)
{
	const uint8_t *p = buf;
	uint32_t i;

	if (buf == NULL || slot == NULL || meta == NULL || len < ST_XFER_WIRE_BEGIN_REQ_LEN) {
		return false;
	}
	memset(meta, 0, sizeof(*meta));

	*slot = get_u16le(&p);
	meta->song_id_hash = get_u32le(&p);
	meta->frame_count = get_u32le(&p);
	meta->expected_crc32 = get_u32le(&p);
	meta->stem_present_mask = *p++;
	for (i = 0; i < ST_STEM_COUNT; i++) {
		meta->stem_content_frames[i] = get_u32le(&p);
	}
	for (i = 0; i < ST_STEM_COUNT; i++) {
		meta->stem_crc32[i] = get_u32le(&p);
	}
	meta->bpm_q8 = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
	p += 2;
	meta->downbeat_frame = get_u32le(&p);
	memcpy(meta->title, p, sizeof(meta->title) - 1u);
	meta->title[sizeof(meta->title) - 1u] = '\0';
	p += sizeof(meta->title);
	memcpy(meta->artist, p, sizeof(meta->artist) - 1u);
	meta->artist[sizeof(meta->artist) - 1u] = '\0';
	p += sizeof(meta->artist);

	return (uint32_t)(p - buf) == ST_XFER_WIRE_BEGIN_REQ_LEN;
}

void st_xfer_wire_encode_begin_rsp(uint8_t out[ST_XFER_WIRE_BEGIN_RSP_LEN], uint32_t resume_sector)
{
	uint8_t *p = out;

	*p++ = (uint8_t)ST_XFER_RSP_BEGIN_OK;
	put_u32le(&p, resume_sector);
}

bool st_xfer_wire_decode_delete_req(const uint8_t *buf, uint32_t len,
				     uint16_t *slot, const uint8_t **token_out)
{
	const uint8_t *p = buf;

	if (buf == NULL || slot == NULL || token_out == NULL || len < ST_XFER_WIRE_DELETE_REQ_LEN) {
		return false;
	}
	*slot = get_u16le(&p);
	*token_out = p;
	return true;
}
