/*
 * st_transfer_protocol.h — Stem Tape companion transfer protocol v1: wire
 * format constants only. See docs/stem-tape-transfer-v1.md for the full
 * specification and docs/stem-tape-transfer-v1-fixtures.json for the
 * companion-tool-facing mirror of these same values.
 *
 * PURE: no I/O, no Zephyr.
 */

#ifndef STEMTAPE_PLAYER_TRANSFER_PROTOCOL_H_
#define STEMTAPE_PLAYER_TRANSFER_PROTOCOL_H_

#include <stdint.h>

#define ST_XFER_PROTOCOL_MAJOR 1u
#define ST_XFER_PROTOCOL_MINOR 0u

/* Entry handshake: byte-identical to the classic SP-1 Tape Looper protocol
 * (firmware/src/main.c xfer_service()'s MAGIC[8]) so existing companion
 * tooling's detection logic is unchanged. */
#define ST_XFER_MAGIC_LEN 8u
static const uint8_t ST_XFER_MAGIC[ST_XFER_MAGIC_LEN] = { 'S','P','1','X','F','E','R','!' };

/* Idle timeout inside transfer mode before an uncommitted transaction is
 * abandoned (classic protocol's own value, reused unchanged). */
#define ST_XFER_IDLE_TIMEOUT_MS 15000u

/* ---- commands: classic (unchanged wire behavior) ---- */
#define ST_XFER_CMD_PING   'P'  /* -> layout info, classic response shape */
#define ST_XFER_CMD_READ   'R'  /* -> raw sector read (classic address space only) */
#define ST_XFER_CMD_WRITE  'W'  /* -> raw sector write (classic address space only) */
#define ST_XFER_CMD_FLUSH  'F'
#define ST_XFER_CMD_EXIT   'X'  /* commit + leave transfer mode */

/* ---- commands: new in Stem Tape v1 ---- */
#define ST_XFER_CMD_VERSION 'V'
#define ST_XFER_CMD_BEGIN   'B'
#define ST_XFER_CMD_STAGE   'S'
#define ST_XFER_CMD_VERIFY  'K'
#define ST_XFER_CMD_COMMIT  'C'
#define ST_XFER_CMD_ABORT   'A'
#define ST_XFER_CMD_DELETE  'D'
#define ST_XFER_CMD_INIT    'I'

/* ---- response bytes ---- */
#define ST_XFER_RSP_OK_GENERIC   'k' /* K -> ok */
#define ST_XFER_RSP_ERR_GENERIC  'e'
#define ST_XFER_RSP_BEGIN_OK     'b'
#define ST_XFER_RSP_STAGE_OK     's'
#define ST_XFER_RSP_COMMIT_OK    'c'
#define ST_XFER_RSP_ABORT_OK     'a'
#define ST_XFER_RSP_DELETE_OK    'd'
#define ST_XFER_RSP_INIT_OK      'i'

/* ---- version/capability response (16 bytes, see docs section 2) ---- */
#define ST_XFER_VERSION_RSP_LEN 16u
#define ST_XFER_VERSION_MAGIC_LEN 4u
static const uint8_t ST_XFER_VERSION_MAGIC[ST_XFER_VERSION_MAGIC_LEN] = { 'S','T','V','1' };

#define ST_XFER_CAP_TRANSACTIONAL_SLOTS (1u << 0)
#define ST_XFER_CAP_CRC32               (1u << 1)

/* ---- destructive-confirmation token (D, I) ----
 * Deliberately distinct from ST_XFER_MAGIC so a plain reconnect can never
 * be mistaken for a destructive confirmation. A companion tool must
 * surface an explicit user confirmation UI before ever sending this. */
#define ST_DESTRUCTIVE_CONFIRM_LEN 8u
static const uint8_t ST_DESTRUCTIVE_CONFIRM_TOKEN[ST_DESTRUCTIVE_CONFIRM_LEN] =
	{ 'S','T','C','O','N','F','R','M' };

/* CRC-32, IEEE 802.3 (reflected 0xEDB88320), matching the polynomial the
 * classic looper already uses for its own index-repair checks. */
#define ST_CRC32_POLY_REFLECTED 0xEDB88320u
#define ST_CRC32_INIT           0xFFFFFFFFu

#endif /* STEMTAPE_PLAYER_TRANSFER_PROTOCOL_H_ */
