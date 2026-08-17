/*
 * Stem Tape LED Feedback Protocol v1 — shared constants.
 *
 * Single source of truth for:
 *   (a) the verified 8-channel physical LED index map, and
 *   (b) the MIDI channel-16 wire protocol the website uses to drive it.
 *
 * Every other LED source file (led_frame.c, led_midi.c, led_render.c,
 * led_battery.c, main.c) and the host tests include ONLY this header for
 * these values — no magic numbers are re-declared elsewhere.
 * docs/stem-tape-led-feedback-v1.md mirrors this table for the web team;
 * keep both in sync if either changes.
 *
 * ---------------------------------------------------------------------
 * PHYSICAL INVENTORY: exactly eight MCU-controllable LEDs.
 * ---------------------------------------------------------------------
 * Four Track LEDs (indices 0-3) and four side battery/Play LEDs
 * (indices 4-7). The Function-button dots and the red triangle are STATIC
 * ENCLOSURE MARKINGS, not LEDs — there is no ninth or tenth channel, and
 * FUNCTION (P0.27) is a plain input GPIO with no emitter of its own.
 *
 * The GPIO *set* for the side row is confirmed by three independent
 * sources (see docs/stem-tape-led-feedback-v1.md "Evidence"):
 *   - timknapen/SP-1-dev stemplayer_pins.h (community pinout reference)
 *   - bnjreece/feldd-sp1-firmware app.overlay + src/led.c (hardware-tested
 *     PWM2/PWM3 implementation on this same board)
 *   - this M0 firmware's own pinned Tape Looper provenance (existing
 *     track_leds[]/leds[] GPIO arrays in main.c, commit a8dd127)
 *
 * The two community sources number that same 4-GPIO set CONTRADICTORILY:
 * Tim Knapen's LED_1..4 order is {P1.13, P0.00, P1.12, P0.01}; feldd's
 * PLAY1..4 order is {P0.01, P1.12, P0.00, P1.13} — the exact reverse. A
 * symbol named "PLAY1" or "LED1" in either source carries NO information
 * about which end of the row sits physically nearest the PLAY button.
 * Neither source documents left-to-right enclosure position, so this
 * table's PLAY-end-to-FUNCTION-end direction is a BEST-EFFORT INFERENCE
 * (this firmware's own pinned Tape Looper leds[] array order, the only
 * fixed reference point this repository actually owns), NOT a
 * hardware-confirmed fact. Confirm or correct it with the eight-step
 * diagnostic sweep (main.c's led_diag_sweep(), triggered by typing 's' into
 * the CDC console) before treating this ordering as ground truth.
 *
 *   idx | logical LED           | GPIO   | PWM instance | PWM channel
 *   ----+------------------------+--------+--------------+------------
 *    0  | Track 1                | P0.29  | PWM2         | 0
 *    1  | Track 2                | P0.26  | PWM2         | 1
 *    2  | Track 3                | P1.15  | PWM2         | 2
 *    3  | Track 4                | P1.14  | PWM2         | 3
 *    4  | Side, nearest PLAY     | P1.13  | PWM3         | 3
 *    5  | Side, PLAY-side middle | P0.00  | PWM3         | 2
 *    6  | Side, FUNCTION-side mid| P1.12  | PWM3         | 1
 *    7  | Side, nearest FUNCTION | P0.01  | PWM3         | 0
 *
 * The two Function-dot visuals and the website's "play-indicator" are
 * host-side illustration only — never physical LED channels. Do not add
 * them here.
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
#define LED_IDX_TRACK1        0u
#define LED_IDX_TRACK2        1u
#define LED_IDX_TRACK3        2u
#define LED_IDX_TRACK4        3u
#define LED_IDX_SIDE_PLAY     4u  /* side LED nearest PLAY (best-effort; confirm via sweep) */
#define LED_IDX_SIDE_MID1     5u  /* side LED, PLAY-side middle position */
#define LED_IDX_SIDE_MID2     6u  /* side LED, FUNCTION-side middle position */
#define LED_IDX_SIDE_FUNCTION 7u  /* side LED nearest FUNCTION (best-effort; confirm via sweep) */

/* idx < LED_TRACK_ROW_COUNT is the track row; the rest is the side row. */
#define LED_TRACK_ROW_COUNT  4u
#define LED_SIDE_ROW_COUNT   4u

/* Proven PWM period (feldd app.overlay, hardware-tested on this board). */
#define LED_PWM_PERIOD_US    1024u

/* M0's existing electrical brightness ceilings (unchanged from the prior
 * software-PWM implementation's LED_PWM_ON_US / LED_STATUS_ON_US). Never
 * raise these without a separately cited and reviewed electrical
 * justification. */
#define LED_TRACK_MAX_PULSE_US  52u
#define LED_SIDE_MAX_PULSE_US   66u

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

/* ------------------------------------------------ battery / Play baseline -
 * Local (unleased) stock behavior: the side row (4-7) is a 4-step battery
 * meter computed from the same battery ADC reading main.c already sends as
 * MIDI CC24 (0..127). "Low battery" == the bottom step of that same meter
 * (step 0 of 4) and is an UNMEASURED threshold (no real-battery-voltage
 * calibration has been performed) — see docs/stem-tape-led-feedback-v1.md
 * "Battery / Play baseline" for the exact composition rule with a leased
 * host frame. */
#define LED_BATTERY_STEP_COUNT     4u
#define LED_BATTERY_LOW_STEP       0u

#endif /* STEMTAPE_LED_PROTOCOL_H_ */
