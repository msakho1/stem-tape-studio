/*
 * led_midi.h — Stem Tape LED Feedback Protocol v1: MIDI channel-16 CC
 * dispatch.
 *
 * PURE: no Zephyr, no UMP struct, no USB. main.c extracts the plain
 * (channel, cc, value) bytes from an incoming struct midi_ump using Zephyr's
 * own UMP_MIDI_CHANNEL()/UMP_MIDI1_P1()/UMP_MIDI1_P2() macros and passes them
 * in here; this module never sees a struct midi_ump. That split is what
 * makes channel routing itself host-testable: this file has zero references
 * to midi_note()/midi_cc()/decode_bands() or any other surface-decode
 * function, so it is structurally incapable of driving surface control
 * events, and channel-1 traffic can never reach the LED protocol by
 * construction, not by a runtime check alone.
 */

#ifndef STEMTAPE_LED_MIDI_H_
#define STEMTAPE_LED_MIDI_H_

#include <stdint.h>

#include "led_protocol.h"

typedef enum {
	LED_MIDI_ACTION_NONE = 0,  /* not LED protocol traffic: ignore */
	LED_MIDI_ACTION_STAGE,
	LED_MIDI_ACTION_COMMIT,
	LED_MIDI_ACTION_HEARTBEAT,
	LED_MIDI_ACTION_RELEASE,
	LED_MIDI_ACTION_CAPABILITY_QUERY,
} led_midi_action_t;

typedef struct {
	led_midi_action_t action;
	uint8_t index; /* LED_MIDI_ACTION_STAGE only: physical index 0..7 */
	uint8_t value; /* STAGE: level 0..127. COMMIT/HEARTBEAT: sequence 0..127 */
} led_midi_decoded_t;

/*
 * Classify one Control Change event. `channel` and `cc` are the raw 0-indexed
 * wire values (channel 0 = MIDI channel 1, cc 0..127); `value` is the 7-bit
 * CC data byte, already clamped by the transport (Zephyr's UMP1_P2 masks to
 * 0x7F).
 *
 * Anything not on LED_MIDI_CHANNEL, or on it but not one of CC 80..91, or
 * CC91 with a nonzero value, decodes to LED_MIDI_ACTION_NONE: "invalid
 * channels and unrelated CCs are ignored". The caller must not act on a NONE
 * result beyond dropping the packet — in particular it must never fall
 * through into the normal SP-1 control decoder.
 */
led_midi_decoded_t led_midi_decode(uint8_t channel, uint8_t cc, uint8_t value);

#endif /* STEMTAPE_LED_MIDI_H_ */
