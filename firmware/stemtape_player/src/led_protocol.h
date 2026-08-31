/*
 * led_protocol.h — Stem Tape standalone player: physical LED hardware
 * constants only.
 *
 * This is the PHYSICAL-LAYER SUBSET of firmware/stemtape/src/led_protocol.h
 * (the M0 diagnostic target's header), which also defines a MIDI
 * channel-16 wire protocol that does not apply here — the standalone
 * player drives its eight LEDs from LOCAL audio/gesture state
 * (st_led_pattern.h), never from a host MIDI connection. led_duty.h/.c and
 * led_render.h/.c are copied byte-for-byte from the M0 target (the same
 * physical PWM2/PWM3 hardware, unchanged) and need only the constants
 * below; keep this file's physical table in sync with the M0 target's if
 * either changes — see that header's "Evidence" section for the citation
 * trail this table is copied from.
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
 * Side-row PLAY-end/FUNCTION-end direction remains a best-effort
 * inference, not hardware-confirmed — see the M0 target's header.
 */

#ifndef STEMTAPE_PLAYER_LED_PROTOCOL_H_
#define STEMTAPE_PLAYER_LED_PROTOCOL_H_

#include <stdint.h>

#define LED_PHYSICAL_COUNT   8u

#define LED_IDX_TRACK1        0u
#define LED_IDX_TRACK2        1u
#define LED_IDX_TRACK3        2u
#define LED_IDX_TRACK4        3u
#define LED_IDX_SIDE_PLAY     4u
#define LED_IDX_SIDE_MID1     5u
#define LED_IDX_SIDE_MID2     6u
#define LED_IDX_SIDE_FUNCTION 7u

#define LED_TRACK_ROW_COUNT  4u
#define LED_SIDE_ROW_COUNT   4u

#define LED_PWM_PERIOD_US    1024u

#define LED_TRACK_MAX_PULSE_US  52u
#define LED_SIDE_MAX_PULSE_US   66u

#define LED_LEVEL_MAX        127u

#endif /* STEMTAPE_PLAYER_LED_PROTOCOL_H_ */
