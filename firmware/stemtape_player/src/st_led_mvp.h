/*
 * st_led_mvp.h — THE single semantic owner of all eight Stem Tape LEDs.
 *
 * PURE: no Zephyr, no GPIO, no timers, no clock of its own. main.c reads the
 * real runtime state, fills st_led_inputs_t, calls st_led_mvp_decide(), and
 * renders the resulting eight brightness levels through the existing
 * TIMER3/GPIO soft-PWM driver.
 *
 * PHYSICAL MAP. Index 0..3 are the Track LEDs T1..T4 above the four Track
 * buttons; index 4..7 are the side LEDs S1..S4, ordered from nearest PLAY
 * (S1, index 4) to nearest FUNCTION (S4, index 7). This matches
 * docs/stem-tape-led-feedback-v1.md section 1 and main.c's pinned arrays:
 *
 *   0..3 -> track_leds[0..3] = P0.29, P0.26, P1.15, P1.14
 *   4..7 -> leds[0..3]       = P1.13, P0.00, P1.12, P0.01
 *
 * DIRECTION CAVEAT, carried from the protocol doc rather than dropped: that
 * document states the PLAY-end/FUNCTION-end direction of the side row is NOT
 * hardware-confirmed. If the physical device shows S4's tempo pulse at the
 * PLAY end instead, the side row is reversed and the fix is to reverse the
 * side mapping in main.c's led_apply_frame(); nothing in this module depends
 * on the direction beyond the S1..S4 index order.
 *
 * BRIGHTNESS, NOT A THREE-STATE VOCABULARY. Every LED carries a 0..255
 * level. The renderer gives each pin its own sigma-delta duty, so a level is
 * a real brightness rather than a name for one of three duties. That is what
 * the beat envelope, the fade-out and the activity scaling need.
 */

#ifndef ST_LED_MVP_H_
#define ST_LED_MVP_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_beat_phase.h"

#define ST_LED_TRACK_COUNT 4u
#define ST_LED_SIDE_COUNT  4u
#define ST_LED_COUNT       (ST_LED_TRACK_COUNT + ST_LED_SIDE_COUNT)

#define ST_LED_TRACK_FIRST 0u
#define ST_LED_SIDE_FIRST  4u
#define ST_LED_S1          (ST_LED_SIDE_FIRST + 0u)   /* nearest PLAY */
#define ST_LED_S2          (ST_LED_SIDE_FIRST + 1u)
#define ST_LED_S3          (ST_LED_SIDE_FIRST + 2u)
#define ST_LED_S4          (ST_LED_SIDE_FIRST + 3u)   /* nearest FUNCTION */

#define ST_LED_MAX 255u

/* All four Track bits of a solo mask. Deliberately the same bit order as
 * st_ladder.h's ST_LADDER_T1..T4 -- bit k is Track (k+1) -- because the
 * value that arrives here IS that decoder's mask, unmodified. */
#define ST_LED_TRACK_MASK_ALL 0xFu

/* ---- centralized timing, all milliseconds ------------------------------
 * The boot and shutdown sequences are driven from firmware time, never from
 * a sleep loop: main.c passes the elapsed ms and this module decides what
 * that instant looks like. Nothing here blocks audio or control. */
#define ST_LED_TRACK_BLINK_MS   100u   /* track blink, boot and shutdown */
#define ST_LED_SIDE_HOLD_MS     100u   /* side row full before it fades */
#define ST_LED_SIDE_FADE_MS     300u   /* side row fade to dark */
#define ST_LED_BATT_PREVIEW_MS  750u   /* boot's own gauge tail; the stopped
					* state then draws the SAME gauge
					* continuously, so this is only how long
					* the BOOT SEQUENCE runs on for, never a
					* timeout after which the row darkens */

#define ST_LED_BOOT_FADE_END_MS  (ST_LED_SIDE_HOLD_MS + ST_LED_SIDE_FADE_MS)
#define ST_LED_BOOT_TOTAL_MS     (ST_LED_BOOT_FADE_END_MS + ST_LED_BATT_PREVIEW_MS)
#define ST_LED_SHUTDOWN_TOTAL_MS (ST_LED_SIDE_HOLD_MS + ST_LED_SIDE_FADE_MS)

/* Charging blink phase period is supplied by the caller as a boolean. */

/* ---- battery ------------------------------------------------------------
 * Ported from the approved local gauge. Two safety rules matter: an
 * unavailable or failed reading is NEVER "low", and a level that was never
 * seeded is NEVER rendered as a charge. The threshold constants come from
 * the pinned Tape Looper source, which labels them PLACEHOLDERS until
 * calibrated; that provisional status is inherited unchanged. */
#define ST_LED_BATT_THR_1  2020u
#define ST_LED_BATT_THR_2  2140u
#define ST_LED_BATT_THR_3  2260u
#define ST_LED_BATT_HYSTERESIS_COUNTS 18u
#define ST_LED_BATT_EMA_SHIFT          3u
#define ST_LED_BATT_LEVEL_COUNT        4u
#define ST_LED_BATT_LOW_LEVEL          1u

typedef enum {
	ST_LED_BATT_UNAVAILABLE = 0,
	ST_LED_BATT_FAULT,
	ST_LED_BATT_CHARGER_ABSENT,
	ST_LED_BATT_CHARGING,
	ST_LED_BATT_CHARGE_COMPLETE,
	ST_LED_BATT_LOW,
} st_led_batt_state_t;

typedef struct {
	int32_t ema;
	uint8_t level;        /* sticky 1..4; 0 == never seeded */
	bool    ever_valid;
	bool    last_read_ok;
} st_led_batt_gauge_t;

void st_led_batt_reset(st_led_batt_gauge_t *g);
void st_led_batt_update(st_led_batt_gauge_t *g, bool valid, int32_t raw_adc);
st_led_batt_state_t st_led_batt_classify(const st_led_batt_gauge_t *g,
					  bool charger_present, bool charging_now);

/* ---- one-shot sequences ------------------------------------------------- */
typedef enum {
	ST_LED_SEQ_NONE = 0,
	ST_LED_SEQ_BOOT,       /* power-on, including the battery preview */
	ST_LED_SEQ_SHUTDOWN,   /* power-off, before SYSTEM_OFF */
} st_led_seq_t;

/* ---- global loop display state (see st_led_inputs_t::loop_state) -------- */
typedef enum {
	ST_LED_LOOP_NONE = 0,
	ST_LED_LOOP_MOMENTARY,   /* PLAY still held */
	ST_LED_LOOP_LATCHED,     /* survives the release */
} st_led_loop_t;

/* ---- inputs -------------------------------------------------------------
 * Every field is real runtime state. Nothing here is a mode flag the LED
 * layer chose for its own convenience. */
typedef struct {
	st_led_seq_t sequence;      /* NONE unless a boot/shutdown animation runs */
	uint32_t     sequence_ms;   /* ms elapsed since that sequence started */

	bool song_selected;
	bool playing;

	bool transfer_active;
	bool transfer_blink_on;

	/* IMMEDIATE MOMENTARY SOLO, AS A CHORD MASK. Bit k set == Track (k+1)
	 * physically held right now -- set from the instant the button goes
	 * down, cleared the instant it is released. No threshold, no latch, no
	 * persistent state.
	 *
	 * THE SAME MASK THE MIXER USES. main.c drives trk[k].solo from these
	 * exact bits, so what is lit and what is heard cannot disagree: they
	 * are one value, not two that have to be kept in step. 0 == nothing
	 * held == normal playback display. */
	uint8_t solo_mask;          /* bits 0..3; see st_ladder.h */

	/* Beat phase, envelope and bar position, all from one st_beat_pulse()
	 * call on the authoritative song_frame. */
	st_beat_pulse_t beat;

	/* GLOBAL LOOP. Shown on S1 only, and only while playing, so the beat
	 * indication the player actually performs to is never disturbed: the
	 * Track row keeps its pulse and chase, and S4 keeps the tempo, exactly
	 * as they are without a loop.
	 *
	 *   NONE       S1 dark      (ordinary playback)
	 *   MOMENTARY  S1 pulses    with the same beat envelope -- "this is
	 *                           live and will end when you let go"
	 *   LATCHED    S1 SOLID     -- "this is holding on its own now"
	 *
	 * Pulsing vs solid is the distinction, which reads at a glance and
	 * costs no extra animation, no extra timer and no second clock. */
	st_led_loop_t loop_state;

	/* ---- THE FX OVERLAY -----------------------------------------------
	 * Ranked ABOVE the solo feedback and the loop chase, below transfer:
	 * while the overlay is open the eight lights answer "what is the rack
	 * doing and where is it", because that is the only question the player
	 * can act on with these buttons.
	 *
	 *   TRACK row  one LED per effect, in BUTTON order (T1 Filter,
	 *              T2 Delay/Echo, T3 Distortion, T4 Gate). LATCHED is
	 *              SOLID; MOMENTARY breathes with the shared beat
	 *              envelope. Same distinction the loop marker already
	 *              uses, so nothing new has to be learned.
	 *   SIDE row   WHERE the one rack is. STEM scope lights the target
	 *              stem's side LED alone; GLOBAL lights all four at a
	 *              lower level, because the rack is on everything.
	 *
	 * There is no error state and no new animation. Closing the overlay
	 * restores the underlying mode on the very next frame -- the beat
	 * clock and loop phase are never touched, because nothing here owns
	 * a clock. */
	bool    fx_open;
	bool    fx_global;      /* scope: true GLOBAL, false STEM */
	uint8_t fx_target;      /* 0..3, meaningful in STEM scope only */
	uint8_t fx_momentary;   /* bit per effect, button order */
	uint8_t fx_latched;     /* bit per effect, button order */
	/* THE FAST FLASH PHASE, on/off, for effects that are actually sounding.
	 * Supplied by the caller as a boolean for exactly the reason the
	 * charging blink is (see that note above): this layer is pure and owns
	 * no clock, and the LED contract forbids a second free-running one.
	 *
	 * main.c derives it from the SAME song_frame and the SAME beat timing
	 * that produce beat.envelope, at a 1/16-note rate, so the flash is
	 * locked to the music rather than free-running against it. With no
	 * trustworthy tempo the caller holds it true, which renders an active
	 * effect as SOLID -- visible and honest -- instead of inventing a grid. */
	bool    fx_flash_on;

	/* Per-stem output activity, 0..255. SCALES the shared beat pulse; it
	 * never gates or times anything. */
	uint8_t stem_activity[ST_LED_TRACK_COUNT];

	st_led_batt_state_t batt_state;
	uint8_t             batt_level;     /* 1..4; 0 == never seeded */
	bool                batt_blink_on;
} st_led_inputs_t;

typedef struct {
	uint8_t level[ST_LED_COUNT];   /* 0..255 per LED */
} st_led_frame_t;

/*
 * THE decision. Pure, total and deterministic: same inputs, same eight
 * levels, no internal state and no clock. Every LED is assigned on every
 * call, so none can retain a value from a previous frame.
 *
 * PRIORITY, highest first -- the order the product owner set:
 *   1. shutdown animation
 *   2. boot animation (whose last phase is the battery gauge, which then
 *      simply continues into the stopped state below -- the handover is
 *      seamless because both draw the identical gauge)
 *   3. transfer: TRACK row blinks; the side row keeps the gauge
 *   4. immediate Track-button solo: TRACK row only, never the side row
 *   5. playing: beat pulse + bar chase on the track row, S4 tempo pulse
 *   6. any powered-on, not-playing state: the persistent battery gauge
 *   7. an untrusted battery reading: side row dark, never a fabricated level
 *
 * THE SIDE ROW IS NEVER DARK MERELY BECAUSE THE DEVICE IS IDLE. A powered-on
 * SP-1 that is not playing shows its charge level continuously -- with no
 * song selected, with a song selected but stopped, on battery, on USB,
 * charging, full, mid-transfer, or with a Track button held. The only thing
 * that darkens it while stopped is item 7.
 */
void st_led_mvp_decide(const st_led_inputs_t *in, st_led_frame_t *out);

#endif /* ST_LED_MVP_H_ */
