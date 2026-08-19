/*
 * st_bulk_xfer.c — see st_bulk_xfer.h for the full contract and doc.
 */

#include "st_bulk_xfer.h"

#include <string.h>

static uint32_t rd_u32le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u32le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

void st_bulk_parse_header(const uint8_t in[ST_BULK_REQ_HEADER_BYTES], st_bulk_req_header_t *out)
{
	out->version = in[ST_BULK_REQ_OFF_VERSION];
	out->seq = rd_u32le(in + ST_BULK_REQ_OFF_SEQ);
	out->dest_block = rd_u32le(in + ST_BULK_REQ_OFF_DEST_BLOCK);
	out->payload_len = rd_u32le(in + ST_BULK_REQ_OFF_PAYLOAD_LEN);
	out->payload_crc32 = rd_u32le(in + ST_BULK_REQ_OFF_PAYLOAD_CRC32);
}

void st_bulk_build_response(st_bulk_status_t status, uint32_t seq, uint32_t dest_block, uint32_t verified_crc32,
			     uint8_t out[ST_BULK_RESP_BYTES])
{
	out[ST_BULK_RESP_OFF_STATUS] = (uint8_t)status;
	wr_u32le(out + ST_BULK_RESP_OFF_SEQ, seq);
	wr_u32le(out + ST_BULK_RESP_OFF_DEST_BLOCK, dest_block);
	wr_u32le(out + ST_BULK_RESP_OFF_VERIFIED_CRC, verified_crc32);
	out[ST_BULK_RESP_OFF_RETRYABLE] = st_bulk_status_is_retryable(status) ? 1u : 0u;
}

bool st_bulk_status_is_retryable(st_bulk_status_t status)
{
	switch (status) {
	case ST_BULK_ERR_TIMEOUT_PAYLOAD:
	case ST_BULK_ERR_CDC_OVERFLOW:
	case ST_BULK_ERR_CRC_MISMATCH:
	case ST_BULK_ERR_EMMC_WRITE_FAIL:
	case ST_BULK_ERR_EMMC_READBACK_FAIL:
	case ST_BULK_ERR_READBACK_CRC_MISMATCH:
		/* Transient/physical: the exact same request may succeed if resent verbatim. */
		return true;
	case ST_BULK_OK:
	case ST_BULK_ERR_UNSUPPORTED_VERSION:
	case ST_BULK_ERR_BAD_LENGTH:
	case ST_BULK_ERR_LAYOUT_NOT_READY:
	case ST_BULK_ERR_NO_SESSION:
	case ST_BULK_ERR_SESSION_CLOSED:
	case ST_BULK_ERR_OUT_OF_SEQUENCE:
	case ST_BULK_ERR_DEST_MISMATCH:
	case ST_BULK_ERR_OUT_OF_BOUNDS:
	case ST_BULK_ERR_ACTIVE_REGION:
	case ST_BULK_ERR_OUTSIDE_FROZEN_PAIR:
	default:
		/* Structural/protocol: retrying the SAME request cannot help -- the host
		 * must fix its own state (re-query 'Q', resync, or abort) first. */
		return false;
	}
}

void st_bulk_seq_reset(st_bulk_seq_t *sq, uint32_t region_start, uint32_t region_cap)
{
	memset(sq, 0, sizeof(*sq));
	sq->has_committed = false;
	sq->next_seq = 0u;
	sq->region_start = region_start;
	sq->region_cap = region_cap;
}

st_bulk_seq_check_t st_bulk_seq_check(const st_bulk_seq_t *sq, uint32_t seq, uint32_t dest_block)
{
	bool is_new = (seq == sq->next_seq);
	/* seq+1 == next_seq (not next_seq-1 == seq) avoids underflowing next_seq when it is 0 --
	 * has_committed already guards against a spurious match on a genuinely fresh session. */
	bool is_retry = sq->has_committed && ((seq + 1u) == sq->next_seq);

	if (!is_new && !is_retry) {
		return ST_BULK_SEQ_OUT_OF_ORDER;
	}

	uint64_t expected_dest = (uint64_t)sq->region_start + (uint64_t)seq * ST_BULK_BLOCKS_PER_SECTOR;

	if ((uint64_t)dest_block != expected_dest) {
		return ST_BULK_SEQ_DEST_MISMATCH;
	}

	uint64_t sector_end_off = (uint64_t)seq * ST_BULK_BLOCKS_PER_SECTOR + ST_BULK_BLOCKS_PER_SECTOR;

	if (sector_end_off > (uint64_t)sq->region_cap) {
		return ST_BULK_SEQ_OUT_OF_BOUNDS;
	}

	return is_new ? ST_BULK_SEQ_NEW : ST_BULK_SEQ_RETRY;
}

void st_bulk_seq_advance(st_bulk_seq_t *sq, uint32_t seq)
{
	if (seq != sq->next_seq) {
		return; /* defensive: only a genuinely-NEW seq may ever advance the tracker */
	}
	sq->next_seq = seq + 1u;
	sq->has_committed = true;
}
