/*
 * st_led_pattern.c — see st_led_pattern.h. PURE.
 *
 * Patterns not previously documented/captured anywhere in this repository
 * (LOADING, STORAGE_ERROR, REJECTION, the exact FX-latch-flash duration)
 * are DESIGNED, not measured — flagged UNMEASURED in the same convention
 * this codebase already uses for undocumented ADC bands, so a reviewer can
 * tell them apart from patterns ported from documented/observed behavior
 * (boot flash timing from the pinned looper's boot_signature(), the
 * transfer blink from docs/stem-tape-transfer-v1.md, the battery gauge
 * shape from the M0 target's led_battery.c).
 */

#include "st_led_pattern.h"

#include <string.h>

/* [looper a8dd127 / M0 boot_signature()]: two flashes, 90 ms on / 110 ms off. */
#define BOOT_FLASH_ON_MS  90u
#define BOOT_FLASH_OFF_MS 110u
#define BOOT_FLASH_CYCLES 2u
#define BOOT_FLASH_TOTAL_MS ((BOOT_FLASH_ON_MS + BOOT_FLASH_OFF_MS) * BOOT_FLASH_CYCLES)

/* UNMEASURED: a single short flash, distinguishable from the two-cycle
 * boot flash by shape (solid, not on/off) and duration. */
#define FX_LATCH_FLASH_MS 150u

/* UNMEASURED: three rapid flashes, distinct from both of the above. */
#define REJECTION_FLASH_ON_MS  40u
#define REJECTION_FLASH_OFF_MS 40u
#define REJECTION_FLASH_CYCLES 3u
#define REJECTION_TOTAL_MS ((REJECTION_FLASH_ON_MS + REJECTION_FLASH_OFF_MS) * REJECTION_FLASH_CYCLES)

/* docs/stem-tape-transfer-v1.md §7: "all four Track LEDs blink together". */
#define TRANSFER_BLINK_PERIOD_MS 600u /* UNMEASURED exact rate; on/off split even */

/* UNMEASURED: loading breathes the Track row together. */
#define LOADING_BREATHE_PERIOD_MS 900u

/* UNMEASURED: storage error alternates track/side row. */
#define STORAGE_ERROR_PERIOD_MS 500u

static void zero_frame(st_led_frame_t *out)
{
	memset(out->level, 0, sizeof(out->level));
}

void st_led_oneshot_start(st_led_oneshot_t *o, st_led_oneshot_id_t id, uint32_t now_ms)
{
	o->id = id;
	o->started_ms = now_ms;
	switch (id) {
	case ST_LED_ONESHOT_BOOT_FLASH:     o->duration_ms = BOOT_FLASH_TOTAL_MS; break;
	case ST_LED_ONESHOT_FX_LATCH_FLASH: o->duration_ms = FX_LATCH_FLASH_MS; break;
	case ST_LED_ONESHOT_REJECTION:      o->duration_ms = REJECTION_TOTAL_MS; break;
	case ST_LED_ONESHOT_NONE:
	default:
		o->duration_ms = 0u;
		break;
	}
}

bool st_led_oneshot_active(const st_led_oneshot_t *o, uint32_t now_ms)
{
	if (o->id == ST_LED_ONESHOT_NONE) {
		return false;
	}
	return (uint32_t)(now_ms - o->started_ms) < o->duration_ms;
}

void st_led_render_oneshot(const st_led_oneshot_t *o, uint32_t now_ms, st_led_frame_t *out)
{
	uint32_t elapsed = (uint32_t)(now_ms - o->started_ms);
	uint8_t i;

	zero_frame(out);

	switch (o->id) {
	case ST_LED_ONESHOT_BOOT_FLASH: {
		uint32_t cycle_len = BOOT_FLASH_ON_MS + BOOT_FLASH_OFF_MS;
		uint32_t phase = elapsed % cycle_len;
		bool on = phase < BOOT_FLASH_ON_MS;

		for (i = 0; i < ST_LED_TRACK_ROW_COUNT; i++) {
			out->level[i] = on ? (uint8_t)ST_LED_LEVEL_MAX : 0u;
		}
		break;
	}
	case ST_LED_ONESHOT_FX_LATCH_FLASH:
		for (i = 0; i < ST_LED_TRACK_ROW_COUNT; i++) {
			out->level[i] = (uint8_t)ST_LED_LEVEL_MAX;
		}
		break;
	case ST_LED_ONESHOT_REJECTION: {
		uint32_t cycle_len = REJECTION_FLASH_ON_MS + REJECTION_FLASH_OFF_MS;
		uint32_t phase = elapsed % cycle_len;
		bool on = phase < REJECTION_FLASH_ON_MS;

		for (i = 0; i < ST_LED_TRACK_ROW_COUNT; i++) {
			out->level[i] = on ? (uint8_t)ST_LED_LEVEL_MAX : 0u;
		}
		break;
	}
	case ST_LED_ONESHOT_NONE:
	default:
		break;
	}
}

st_led_base_t st_led_select_base(bool storage_error, bool low_battery, bool transferring,
				  bool loading)
{
	if (storage_error) {
		return ST_LED_BASE_STORAGE_ERROR;
	}
	if (low_battery) {
		return ST_LED_BASE_LOW_BATTERY;
	}
	if (transferring) {
		return ST_LED_BASE_TRANSFER;
	}
	if (loading) {
		return ST_LED_BASE_LOADING;
	}
	return ST_LED_BASE_IDLE;
}

/* Same shape as the M0 target's led_battery.c gauge: quarters solid below
 * the current level, current level solid unless charging (then it
 * blinks), all four solid at charge-complete. Renders into side indices
 * 5,6,7 only -- index 4 (ST_LED_SIDE_PLAY) is owned by play/pause state,
 * not the gauge, per task section 8. `level` 0 = never seeded -> off,
 * never fabricated as empty. */
static void render_battery_gauge(uint8_t level, bool charging, bool blink_phase,
				  uint8_t out[ST_LED_COUNT])
{
	uint8_t i;

	for (i = 0; i < 3u; i++) {
		uint8_t idx = (uint8_t)(ST_LED_SIDE_PLAY + 1u + i); /* 5,6,7 */
		uint8_t step = (uint8_t)(i + 1u);                    /* map to gauge levels 2..4 */
		bool lit;

		if (level == 0xFFu || level == 0u) {
			lit = false;
		} else if (step < level) {
			lit = true;
		} else if (step == level) {
			lit = charging ? blink_phase : true;
		} else {
			lit = false;
		}
		out[idx] = lit ? (uint8_t)ST_LED_LEVEL_MAX : 0u;
	}
}

static void render_track_vu(const st_led_inputs_t *in, uint8_t out[ST_LED_COUNT])
{
	uint8_t i;

	for (i = 0; i < ST_LED_TRACK_ROW_COUNT; i++) {
		uint8_t level;

		switch (in->stem_status[i]) {
		case ST_LED_STEM_EMPTY:
			level = 0u;
			break;
		case ST_LED_STEM_MUTED:
			level = 40u; /* "ghost glow" -- looper convention: distinguishes
				      * muted-with-content from empty */
			break;
		case ST_LED_STEM_SOLOED:
			level = (uint8_t)ST_LED_LEVEL_MAX;
			break;
		case ST_LED_STEM_LINKED:
			level = in->stem_activity[i]; /* activity-driven, distinguished from
							* LOADED by the caller's activity
							* feed once the audio engine lands */
			break;
		case ST_LED_STEM_LOADED:
		default:
			level = in->stem_activity[i];
			break;
		}
		if (i == in->active_stem && level < (ST_LED_LEVEL_MAX - 50u)) {
			level = (uint8_t)(level + 50u); /* "brighter stable" active-stem indication */
		}
		out[i] = level;
	}
}

void st_led_render_base(st_led_base_t base, const st_led_inputs_t *in, uint32_t now_ms,
			 st_led_frame_t *out)
{
	uint8_t i;

	zero_frame(out);

	switch (base) {
	case ST_LED_BASE_TRANSFER: {
		uint32_t phase = now_ms % TRANSFER_BLINK_PERIOD_MS;
		bool on = phase < (TRANSFER_BLINK_PERIOD_MS / 2u);

		for (i = 0; i < ST_LED_TRACK_ROW_COUNT; i++) {
			out->level[i] = on ? (uint8_t)ST_LED_LEVEL_MAX : 0u;
		}
		return;
	}
	case ST_LED_BASE_LOADING: {
		uint32_t phase = now_ms % LOADING_BREATHE_PERIOD_MS;
		uint32_t half = LOADING_BREATHE_PERIOD_MS / 2u;
		uint32_t tri = (phase < half) ? phase : (LOADING_BREATHE_PERIOD_MS - phase);
		uint8_t level = (uint8_t)((tri * ST_LED_LEVEL_MAX) / half);

		for (i = 0; i < ST_LED_TRACK_ROW_COUNT; i++) {
			out->level[i] = level;
		}
		return;
	}
	case ST_LED_BASE_STORAGE_ERROR: {
		uint32_t phase = now_ms % STORAGE_ERROR_PERIOD_MS;
		bool track_on = phase < (STORAGE_ERROR_PERIOD_MS / 2u);

		for (i = 0; i < ST_LED_TRACK_ROW_COUNT; i++) {
			out->level[i] = track_on ? (uint8_t)ST_LED_LEVEL_MAX : 0u;
		}
		for (i = 0; i < ST_LED_SIDE_ROW_COUNT; i++) {
			out->level[ST_LED_TRACK_ROW_COUNT + i] = track_on ? 0u : (uint8_t)ST_LED_LEVEL_MAX;
		}
		return;
	}
	case ST_LED_BASE_LOW_BATTERY:
		render_battery_gauge(in->battery_level, in->charging,
				      ((now_ms / 500u) & 1u) != 0u, out->level);
		return;
	case ST_LED_BASE_IDLE:
	default:
		break;
	}

	/* ---- IDLE: stock playback/VU + overlays ---- */
	render_track_vu(in, out->level);

	if (in->scrub_active) {
		/* Directional chase across the Track row; rate follows the
		 * selected speed (also satisfies "four scrub levels: Track
		 * LEDs indicate the selected level"). */
		uint32_t period_ms = 500u >> (in->scrub_speed_index > 3u ? 3u : in->scrub_speed_index);
		uint32_t step = (period_ms > 0u) ? (now_ms / period_ms) % ST_LED_TRACK_ROW_COUNT : 0u;
		uint8_t lit = (in->scrub_direction >= 0)
				      ? (uint8_t)step
				      : (uint8_t)(ST_LED_TRACK_ROW_COUNT - 1u - step);

		for (i = 0; i < ST_LED_TRACK_ROW_COUNT; i++) {
			out->level[i] = (i == lit) ? (uint8_t)ST_LED_LEVEL_MAX : 0u;
		}
	} else {
		if (in->fx_momentary_active && in->fx_momentary_bank < ST_LED_TRACK_ROW_COUNT) {
			uint32_t period = 400u;
			uint32_t phase = now_ms % period;
			uint32_t half = period / 2u;
			uint32_t tri = (phase < half) ? phase : (period - phase);

			out->level[in->fx_momentary_bank] = (uint8_t)((tri * ST_LED_LEVEL_MAX) / half);
		}
		for (i = 0; i < ST_LED_TRACK_ROW_COUNT; i++) {
			if (in->fx_latched[i]) {
				out->level[i] = (uint8_t)ST_LED_LEVEL_MAX; /* latched FX: solid */
			}
		}
	}

	if (in->gate_active && in->gate_period_ms > 0u) {
		bool on = (now_ms % in->gate_period_ms) < (in->gate_period_ms / 2u);

		if (!on) {
			for (i = 0; i < ST_LED_TRACK_ROW_COUNT; i++) {
				out->level[i] = 0u;
			}
		}
	}

	out->level[ST_LED_SIDE_PLAY] = in->playing ? (uint8_t)ST_LED_LEVEL_MAX : 0u;
	if (!in->playing) {
		render_battery_gauge(in->battery_level, in->charging,
				      ((now_ms / 500u) & 1u) != 0u, out->level);
	}
}

void st_led_render(const st_led_oneshot_t *oneshot, st_led_base_t base,
		    const st_led_inputs_t *in, uint32_t now_ms, st_led_frame_t *out)
{
	if (st_led_oneshot_active(oneshot, now_ms)) {
		st_led_render_oneshot(oneshot, now_ms, out);
		return;
	}
	st_led_render_base(base, in, now_ms, out);
}
