/*
 * led_midi.c — see led_midi.h. PURE: no Zephyr, no UMP, no surface decode.
 */

#include "led_midi.h"

led_midi_decoded_t led_midi_decode(uint8_t channel, uint8_t cc, uint8_t value)
{
	led_midi_decoded_t out = { LED_MIDI_ACTION_NONE, 0u, 0u };

	if (channel != LED_MIDI_CHANNEL) {
		return out; /* channel-1 (and every other channel) never enters the LED protocol */
	}

	if (cc >= LED_CC_STAGE_FIRST && cc <= LED_CC_STAGE_LAST) {
		out.action = LED_MIDI_ACTION_STAGE;
		out.index = (uint8_t)(cc - LED_CC_STAGE_FIRST);
		out.value = value;
		return out;
	}

	switch (cc) {
	case LED_CC_COMMIT:
		out.action = LED_MIDI_ACTION_COMMIT;
		out.value = value;
		break;
	case LED_CC_HEARTBEAT:
		out.action = LED_MIDI_ACTION_HEARTBEAT;
		out.value = value;
		break;
	case LED_CC_RELEASE:
		out.action = LED_MIDI_ACTION_RELEASE;
		break;
	case LED_CC_CAPABILITY:
		/* "Capability query when value is 0." Any other value on this
		 * CC is unrelated traffic on a reserved number and is
		 * ignored, not treated as a query. */
		if (value == 0u) {
			out.action = LED_MIDI_ACTION_CAPABILITY_QUERY;
		}
		break;
	default:
		break; /* unrelated CC on channel 16: ignored */
	}
	return out;
}
