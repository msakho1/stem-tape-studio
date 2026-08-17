/*
 * Stem Tape LED Feedback Protocol v1 — shared constants.
 *
 * Single source of truth for:
 *   (a) the verified 8-channel physical LED index map, and
 *   (b) the MIDI channel-16 wire protocol the website uses to drive it.
 *
 * Every other LED source file (led_frame.c, led_midi.c, led_render.c,
 * main.c) and the host tests include ONLY this header for these values —
 * no magic numbers are re-declared elsewhere. docs/stem-tape-led-feedback-v1.md
 * mirrors this table for the web team; keep both in sync if either changes.
 *
 * Physical index -> GPIO, cross-checked against three independent sources
 * before any code was written (see docs/stem-tape-led-feedback-v1.md
 * "Evidence" section for the full citation):
 *   - timknapen/SP-1-dev stemplayer_pins.h (community pinout reference)
 *   - bnjreece/feldd-sp1-firmware app.overlay + src/led.c (hardware-tested
 *     PWM2/PWM3 implementation on this same board)
 *   - this M0 firmware's own pinned Tape Looper provenance (existing
 *     track_leds[]/leds[] GPIO arrays in main.c, commit a8dd127)
 *
 *   idx | logical LED       | GPIO   | PWM instance | PWM channel
 *   ----+--------------------+--------+--------------+------------
 *    0  | Track 1            | P0.29  | PWM2         | 0
 *    1  | Track 2            | P0.26  | PWM2         | 1
 *    2  | Track 3            | P1.15  | PWM2         | 2
 *    3  | Track 4            | P1.14  | PWM2         | 3
 *    4  | Playback/side 1    | P0.01  | PWM3         | 0
 *    5  | Playback/side 2    | P1.12  | PWM3         | 1
 *    6  | Playback/side 3    | P0.00  | PWM3         | 2
 *    7  | Playback/side 4    | P1.13  | PWM3         | 3
 *
 * There is no separate Function-button LED channel: FUNCTION (P0.27) is a
 * plain input GPIO with no emitter of its own (see main.c PWR_PIN). The two
 * Function-dot visuals and the website's "play-indicator" are host-side
 * illustration only — never physical LED channels. Do not add them here.
 *
 * Never drive P0.22 (BQ24232 nCHG), P0.24 (BQ24232 nPGOOD), or P0.21 outside
 * the existing charger_init()/BQ_NCE_PIN implementation: charger-control/
 * status nets, not available LED outputs.
 */

#ifndef STEMTAPE_LED_PROTOCOL_H_
#define STEMTAPE_LED_PROTOCOL_H_

#include <stdint.h>

/* ------------------------------------------------------- physical layer -- */

#define LED_PHYSICAL_COUNT   8u

/* Physical indices, in the table order above. */
#define LED_IDX_TRACK1       0u
#define LED_IDX_TRACK2       1u
#define LED_IDX_TRACK3       2u
#define LED_IDX_TRACK4       3u
#define LED_IDX_PLAYBACK1    4u
#define LED_IDX_PLAYBACK2    5u
#define LED_IDX_PLAYBACK3    6u
#define LED_IDX_PLAYBACK4    7u

/* idx < LED_TRACK_ROW_COUNT is the track row; the rest is the playback row. */
#define LED_TRACK_ROW_COUNT  4u

/* Proven PWM period (feldd app.overlay, hardware-tested on this board). */
#define LED_PWM_PERIOD_US    1024u

/* M0's existing electrical brightness ceilings (unchanged from the prior
 * software-PWM implementation's LED_PWM_ON_US / LED_STATUS_ON_US). Never
 * raise these without a separately cited and reviewed electrical
 * justification. */
#define LED_TRACK_MAX_PULSE_US     52u
#define LED_PLAYBACK_MAX_PULSE_US  66u

/* Incoming/staged brightness levels are 7-bit MIDI values. */
#define LED_LEVEL_MAX        127u

/* ------------------------------------------------- MIDI transport layer -- */

/* MIDI channel 16, zero-indexed as the wire/UMP value (channel 1 = index 0,
 * so channel 16 = index 15). Existing surface controls stay on channel 1
 * (index 0, ST_MIDI_CHANNEL in midi_protocol.h) and are never touched. */
#define LED_MIDI_CHANNEL     15u

/* Control Change numbers reserved for this protocol on channel 16. */
#define LED_CC_STAGE_FIRST   80u  /* 80..87: stage physical index 0..7    */
#define LED_CC_STAGE_LAST    87u
#define LED_CC_COMMIT        88u  /* value = frame sequence 0..127        */
#define LED_CC_HEARTBEAT     89u  /* value = most recently committed seq  */
#define LED_CC_RELEASE       90u  /* release ownership, clear runtime frame */
#define LED_CC_CAPABILITY    91u  /* query when value == 0                */

#define LED_PROTOCOL_VERSION 1u

/* Sequence numbers are modulo-128 (they ride in a 7-bit MIDI CC value). */
#define LED_SEQ_MODULUS      128u

/* Host must heartbeat at least this often; firmware times the lease out
 * after LED_LEASE_TIMEOUT_MS with no commit or heartbeat. */
#define LED_HEARTBEAT_INTERVAL_MS  250u
#define LED_LEASE_TIMEOUT_MS       1000u

#endif /* STEMTAPE_LED_PROTOCOL_H_ */
