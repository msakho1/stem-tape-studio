/*
 * st_led_mvp.c — see st_led_mvp.h for what this owns and why it exists.
 *
 * PURE. No Zephyr, no hardware, no globals, no clock.
 */

#include "st_led_mvp.h"

/* ==========================================================================
 * BATTERY GAUGE
 *
 * Ported from firmware/stemtape/src/led_battery.c, which ported it from the
 * pinned Tape Looper's own standby gauge. Ported rather than linked because
 * that module includes led_duty.h, which belongs to the M0 target's PWM2/
 * PWM3 renderer and its led_channel_table[] -- neither of which exists in
 * this firmware, whose LEDs are driven by the TIMER3/GPIO soft-PWM driver
 * the product owner directed be kept. The arithmetic below is the same, and
 * is kept line-for-line comparable on purpose so the two can be diffed.
 * ========================================================================== */

static const int32_t batt_thr[3] = {
	(int32_t)ST_LED_BATT_THR_1,
	(int32_t)ST_LED_BATT_THR_2,
	(int32_t)ST_LED_BATT_THR_3,
};

void st_led_batt_reset(st_led_batt_gauge_t *g)
{
	g->ema = -1;
	g->level = 0u;
	g->ever_valid = false;
	g->last_read_ok = false;
}

void st_led_batt_update(st_led_batt_gauge_t *g, bool valid, int32_t raw_adc)
{
	uint8_t nl;
	int k;

	g->last_read_ok = valid;
	if (!valid) {
		return;   /* sticky: EMA and level unchanged on a failed read */
	}
	g->ever_valid = true;

	/* Integer DIVISION, not a right shift: the shift would truncate toward
	 * negative infinity on a falling reading, where the source truncates
	 * toward zero. Matched exactly rather than approximated. */
	g->ema = (g->ema < 0) ? raw_adc
			      : g->ema + (raw_adc - g->ema) / (1 << ST_LED_BATT_EMA_SHIFT);

	nl = 1u;
	for (k = 0; k < 3; k++) {
		if (g->ema > batt_thr[k]) {
			nl = (uint8_t)(k + 2);
		}
	}

	if (g->level == 0u) {
		g->level = nl;   /* first valid sample seeds the sticky level */
	} else if (nl > g->level &&
		   g->ema > batt_thr[g->level - 1u] + (int32_t)ST_LED_BATT_HYSTERESIS_COUNTS) {
		g->level = nl;
	} else if (nl < g->level &&
		   g->ema < batt_thr[g->level - 2u] - (int32_t)ST_LED_BATT_HYSTERESIS_COUNTS) {
		g->level = nl;
	}
}

st_led_batt_state_t st_led_batt_classify(const st_led_batt_gauge_t *g,
					  bool charger_present, bool charging_now)
{
	/* nCHG asserted while nPGOOD is deasserted means "charging with no
	 * input power", which the BQ24232 cannot legitimately report. Flag it
	 * distinctly, and never as "low" -- a wiring or driver problem must not
	 * be shown to the musician as an empty battery. */
	if (charging_now && !charger_present) {
		return ST_LED_BATT_FAULT;
	}
	if (!g->ever_valid) {
		return ST_LED_BATT_UNAVAILABLE;
	}
	if (!g->last_read_ok) {
		return ST_LED_BATT_FAULT;
	}
	if (charger_present) {
		return charging_now ? ST_LED_BATT_CHARGING : ST_LED_BATT_CHARGE_COMPLETE;
	}
	return (g->level <= ST_LED_BATT_LOW_LEVEL) ? ST_LED_BATT_LOW
						   : ST_LED_BATT_CHARGER_ABSENT;
}

/* ==========================================================================
 * THE EIGHT-LED DECISION
 * ========================================================================== */

static void decide_side(const st_led_inputs_t *in, st_led_frame_t *out)
{
	uint8_t i;

	/* ---- TRANSPORT: the side LED nearest PLAY, and nothing else --------
	 *
	 * DOCUMENTED CONFLICT. docs/stem-tape-led-feedback-v1.md section 2
	 * specifies the side row as a FOUR-step charge gauge whose bottom step
	 * is this very LED (gauge step 1, index 4). The product owner's
	 * explicit MVP decision reassigns it to transport -- "the side LED
	 * nearest PLAY is the transport indicator; the remaining three side
	 * LEDs represent battery/charging state only". The owner's decisions
	 * override all three contract documents, so transport wins here and
	 * the gauge below renders on three LEDs instead of four.
	 *
	 * Note this also removes the only reason the side row was ever a
	 * 16-song bank/position display: that was inherited Tape Looper
	 * behaviour, and on a one-song device it made a side LED blink to
	 * announce "song 1 of 16". It is gone, not merely outranked. */
	out->mode[ST_LED_SIDE_TRANSPORT] = in->playing ? ST_LED_SOLID : ST_LED_OFF;

	/* ---- BATTERY: the remaining three ---------------------------------
	 *
	 * Off unless the state is both valid and available. UNAVAILABLE and
	 * FAULT are shown as darkness rather than as a guessed charge, and a
	 * level that has never been seeded is likewise dark: the product owner
	 * was explicit that untrustworthy battery information must not be
	 * invented, and the contract's own safety rule says the same.
	 *
	 * THE THREE-LED GAUGE. The classifier keeps the documented FOUR levels
	 * unchanged -- so the thresholds, the hysteresis and, importantly, the
	 * LOW decision are all bit-for-bit the approved ones -- and only the
	 * rendering is adapted to the three LEDs that remain:
	 *
	 *     level 1 (lowest)  -> bottom battery LED GHOST
	 *     level 2           -> bottom SOLID
	 *     level 3           -> bottom + middle SOLID
	 *     level 4 (full)    -> all three SOLID
	 *
	 * Level 1 uses GHOST rather than going dark so "nearly empty" stays
	 * distinguishable from "no information", which darkness already means.
	 * That reuses the contract's existing faint/solid vocabulary rather
	 * than inventing a fifth state for the adaptation.
	 *
	 * CHARGING blinks the topmost lit step, exactly as the documented
	 * gauge does. CHARGE_COMPLETE lights all three solid, matching the
	 * document's "all four solid once complete" on the LEDs that remain. */
	for (i = 0; i < ST_LED_SIDE_BATT_COUNT; i++) {
		out->mode[ST_LED_SIDE_BATT_FIRST + i] = ST_LED_OFF;
	}

	if (in->batt_state == ST_LED_BATT_UNAVAILABLE ||
	    in->batt_state == ST_LED_BATT_FAULT ||
	    in->batt_level == 0u) {
		return;   /* no trustworthy reading: leave them dark */
	}

	{
		bool charging = (in->batt_state == ST_LED_BATT_CHARGING);
		/* CHARGE_COMPLETE fills the gauge regardless of the last
		 * sampled level, matching the documented gauge's own
		 * "all solid once complete". */
		uint8_t level = (in->batt_state == ST_LED_BATT_CHARGE_COMPLETE)
				? ST_LED_BATT_LEVEL_COUNT : in->batt_level;

		if (level == 1u) {
			out->mode[ST_LED_SIDE_BATT_FIRST] =
				charging ? (in->batt_blink_on ? ST_LED_GHOST : ST_LED_OFF)
					 : ST_LED_GHOST;
			return;
		}

		/* levels 2..4 light (level - 1) LEDs from the bottom up. */
		for (i = 0; i < ST_LED_SIDE_BATT_COUNT; i++) {
			uint8_t step = (uint8_t)(i + 2u);   /* this LED represents level `step` */

			if (step < level) {
				out->mode[ST_LED_SIDE_BATT_FIRST + i] = ST_LED_SOLID;
			} else if (step == level) {
				out->mode[ST_LED_SIDE_BATT_FIRST + i] =
					charging ? (in->batt_blink_on ? ST_LED_SOLID : ST_LED_OFF)
						 : ST_LED_SOLID;
			}
		}
	}
}

static void decide_tracks(const st_led_inputs_t *in, st_led_frame_t *out)
{
	uint8_t i;

	/* ---- NO SONG: every track light dark ------------------------------
	 * Not a standby chase. The inherited chase existed to prove an empty
	 * Tape Looper was awake; here it actively lies, because it runs the
	 * same animation whether or not a song is loaded. The product owner
	 * excluded it, and with no song there is genuinely nothing to show. */
	if (!in->song_selected) {
		for (i = 0; i < ST_LED_TRACK_COUNT; i++) {
			out->mode[i] = ST_LED_OFF;
		}
		return;
	}

	/* ---- STEM STATE ---------------------------------------------------
	 * The contract's own two-word vocabulary, nothing more:
	 *
	 *   audible          -> SOLID   ("soloed stem solid", "active stem
	 *                                 LED brightens")
	 *   loaded, silent   -> GHOST   ("non-solo stems faint", "muted head
	 *                                 faint")
	 *   not loaded       -> OFF
	 *
	 * That one rule already covers mute, solo and solo-suppression,
	 * because `stem_audible` is the MIXER'S OWN audibility decision read
	 * back -- the same function the audio thread applies -- rather than a
	 * second copy of the rule maintained here. Hold a track button for
	 * solo and the held stem stays audible (SOLID) while the others fall
	 * silent (GHOST); release and audibility returns to whatever mute
	 * state says, so the display follows within one control pass with no
	 * separate release path to get wrong.
	 *
	 * NO VU METER. The previous code drove these four lights from a
	 * per-stem peak envelope and called it a beat pulse. It was neither
	 * approved by any of the three contract documents nor a beat: it was a
	 * level meter, and a level meter is not a stable, readable indication
	 * of which stems are playing -- a quiet passage dimmed a stem that was
	 * very much loaded and audible. The product owner ruled it out by
	 * name. Transport motion lives on the transport LED; these four say
	 * what each lane IS. */
	for (i = 0; i < ST_LED_TRACK_COUNT; i++) {
		if (!in->stem_loaded[i]) {
			out->mode[i] = ST_LED_OFF;
		} else if (in->stem_audible[i]) {
			out->mode[i] = ST_LED_SOLID;
		} else {
			out->mode[i] = ST_LED_GHOST;
		}
	}
}

void st_led_mvp_decide(const st_led_inputs_t *in, st_led_frame_t *out)
{
	uint8_t i;

	decide_side(in, out);
	decide_tracks(in, out);

	/* ---- TRANSFER OVERLAY, applied last --------------------------------
	 * The one temporary animation the product owner allowed: all four
	 * Track LEDs blink together while a transfer is in progress.
	 *
	 * It is an OVERLAY on a frame that was already fully computed from
	 * live state, and it touches only the track row. That is what makes
	 * "normal state must restore immediately afterward" true by
	 * construction rather than by remembering to undo something: there is
	 * no snapshot taken when the transfer starts and no restore step when
	 * it ends. The moment transfer_active goes false, the very next call
	 * returns the frame decide_tracks() had already produced from whatever
	 * mute/solo/audibility state is live AT THAT MOMENT -- including any
	 * change the musician made mid-transfer.
	 *
	 * The side row is deliberately untouched: transport and battery remain
	 * readable throughout, and the transfer cannot leave them stale. */
	if (in->transfer_active) {
		for (i = 0; i < ST_LED_TRACK_COUNT; i++) {
			out->mode[i] = in->transfer_blink_on ? ST_LED_SOLID : ST_LED_OFF;
		}
	}
}
