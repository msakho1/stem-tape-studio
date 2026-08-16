/*
 * ============================================================================
 *  STEM TAPE FIRMWARE — MILESTONE M0: raw USB-MIDI control surface
 * ============================================================================
 *  The SP-1 enumerates as a class-compliant USB MIDI device and transmits
 *  RAW PHYSICAL STATE ONLY. Nothing on the device interprets the surface:
 *  no chords, no tap/hold discrimination, no looper state, no musical LED
 *  feedback. The host (Stem Tape) owns all interpretation.
 *
 *  What is reused UNCHANGED from the verified SP-1 Tape Looper firmware
 *  (firmware/src/main.c, same repository):
 *    - the board definition (firmware/boards/teenageengineering/stem_player)
 *    - the LED pin map + the always-dim soft-PWM renderer (zero-latency IRQ)
 *    - the BTN_COM ladder rail, the oversampled ADC read and the verified
 *      voltage-band decoders for the track/PLAY and Vol/rocker ladders
 *    - the fader ADC channels + deadband
 *    - the power button (P0.27), the 2.5 s hold-to-power-off with the LED
 *      countdown, SYSTEM_OFF wake arming, and the Track1+Track4 DFU failsafe
 *    - the 4 s watchdog and the fault->reboot handler
 *    - the device_next USB stack + the shared sample_usbd bring-up helper
 *
 *  What is new in M0:
 *    - usbd_midi2 replaces the UAC2 audio class (CDC ACM console kept)
 *    - one MIDI event per physical edge (see src/midi_protocol.h)
 *    - CC123 All Notes Off whenever the host (re)enables the interface
 *    - the Stem Tape boot signature: a double all-track-LED flash
 *
 *  BOOTLOADER SAFETY (the SP-1 "BIG FIVE") is preserved: app at 0x20000,
 *  watchdog fed < 5 s, no re-init of bootloader-owned clocks, SYSTEM_OFF
 *  returns to the bootloader, RESETREAS cleared on boot and before sleep.
 * ============================================================================
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_midi2.h>
#include <zephyr/audio/midi.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#include <sample_usbd.h>
#include <soc.h>
#include <string.h>

#include "midi_protocol.h"

/* FAILSAFE: any unrecoverable fault becomes a clean reboot (never a brick). */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(reason);
	ARG_UNUSED(esf);
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

#define WDT_NODE DT_ALIAS(watchdog0)

/* ---------------------------------------------------------------- LEDs ---
 * Pin map and soft-PWM dimming reused from the looper firmware.
 */
struct led { NRF_GPIO_Type *port; uint32_t pin; };

/* the 4 playback / status LEDs (center row) */
static const struct led leds[] = {
	{ NRF_P1, 13 }, { NRF_P0, 0 }, { NRF_P1, 12 }, { NRF_P0, 1 },
};
#define NUM_LEDS (sizeof(leds) / sizeof(leds[0]))

/* the 4 TRACK LEDs (directly above buttons 1-4) */
static const struct led track_leds[] = {
	{ NRF_P0, 29 }, { NRF_P0, 26 }, { NRF_P1, 15 }, { NRF_P1, 14 },
};
#define NUM_TRACK_LEDS (sizeof(track_leds) / sizeof(track_leds[0]))

#define LED_PWM_PERIOD_US 1000u   /* 1 kHz frame */
#define LED_PWM_ON_US       52u   /* track row duty  (verified, flicker-free) */
#define LED_STATUS_ON_US    66u   /* status row duty */
#define LED_PWM_TIMER      NRF_TIMER3
#define LED_PWM_TIMER_IRQn TIMER3_IRQn
#define LED_ALL_P0 ((1u<<0)|(1u<<1)|(1u<<29)|(1u<<26))
#define LED_ALL_P1 ((1u<<13)|(1u<<12)|(1u<<15)|(1u<<14))

static volatile uint32_t g_led_p0_on, g_led_p1_on;
static uint32_t g_led_sta_p0, g_led_sta_p1;
static uint32_t g_led_trk_p0, g_led_trk_p1;

/* DIRECT ISR (IRQ_ZERO_LATENCY): pure register IO, never reschedules. */
ISR_DIRECT_DECLARE(led_pwm_isr)
{
	if (LED_PWM_TIMER->EVENTS_COMPARE[1]) {        /* period wrap: render */
		LED_PWM_TIMER->EVENTS_COMPARE[1] = 0;
		(void)LED_PWM_TIMER->EVENTS_COMPARE[1];
		uint32_t s0 = g_led_p0_on, s1 = g_led_p1_on;
		NRF_P0->OUTSET = s0;
		NRF_P0->OUTCLR = LED_ALL_P0 & ~s0;
		NRF_P1->OUTSET = s1;
		NRF_P1->OUTCLR = LED_ALL_P1 & ~s1;
	}
	if (LED_PWM_TIMER->EVENTS_COMPARE[0]) {        /* track row on-time up */
		LED_PWM_TIMER->EVENTS_COMPARE[0] = 0;
		(void)LED_PWM_TIMER->EVENTS_COMPARE[0];
		NRF_P0->OUTCLR = g_led_trk_p0;
		NRF_P1->OUTCLR = g_led_trk_p1;
	}
	if (LED_PWM_TIMER->EVENTS_COMPARE[2]) {        /* status row on-time up */
		LED_PWM_TIMER->EVENTS_COMPARE[2] = 0;
		(void)LED_PWM_TIMER->EVENTS_COMPARE[2];
		NRF_P0->OUTCLR = g_led_sta_p0;
		NRF_P1->OUTCLR = g_led_sta_p1;
	}
	return 0;
}

static void led_cfg_output(const struct led *l)
{
	l->port->PIN_CNF[l->pin] =
		(GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)   |
		(GPIO_PIN_CNF_DRIVE_S0S1    << GPIO_PIN_CNF_DRIVE_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
}

static void led_pwm_init(void)
{
	LED_PWM_TIMER->MODE      = TIMER_MODE_MODE_Timer;
	LED_PWM_TIMER->BITMODE   = TIMER_BITMODE_BITMODE_16Bit;
	LED_PWM_TIMER->PRESCALER = 4;                 /* 16 MHz/16 = 1 us tick */
	LED_PWM_TIMER->CC[0]     = LED_PWM_ON_US;
	LED_PWM_TIMER->CC[1]     = LED_PWM_PERIOD_US;
	LED_PWM_TIMER->CC[2]     = LED_STATUS_ON_US;
	LED_PWM_TIMER->SHORTS    = TIMER_SHORTS_COMPARE1_CLEAR_Msk;
	LED_PWM_TIMER->INTENSET  = TIMER_INTENSET_COMPARE0_Msk |
				   TIMER_INTENSET_COMPARE1_Msk |
				   TIMER_INTENSET_COMPARE2_Msk;
	for (int i = 0; i < NUM_LEDS; i++) {
		if (leds[i].port == NRF_P0) g_led_sta_p0 |= (1u << leds[i].pin);
		else                        g_led_sta_p1 |= (1u << leds[i].pin);
	}
	for (int i = 0; i < NUM_TRACK_LEDS; i++) {
		if (track_leds[i].port == NRF_P0) g_led_trk_p0 |= (1u << track_leds[i].pin);
		else                              g_led_trk_p1 |= (1u << track_leds[i].pin);
	}
	IRQ_DIRECT_CONNECT(LED_PWM_TIMER_IRQn, 0, led_pwm_isr, IRQ_ZERO_LATENCY);
	irq_enable(LED_PWM_TIMER_IRQn);
	LED_PWM_TIMER->TASKS_CLEAR = 1;
	LED_PWM_TIMER->TASKS_START = 1;
}

static void led_set(const struct led *l, bool on)
{
	if (l->port == NRF_P0) {
		if (on) g_led_p0_on |= (1u << l->pin);
		else    g_led_p0_on &= ~(1u << l->pin);
	} else {
		if (on) g_led_p1_on |= (1u << l->pin);
		else    g_led_p1_on &= ~(1u << l->pin);
	}
}
static void led_on(int i)        { led_set(&leds[i], true); }
static void led_off(int i)       { led_set(&leds[i], false); }
static void all_off(void)        { for (int i = 0; i < NUM_LEDS; i++) led_off(i); }
static void track_led_on(int i)  { led_set(&track_leds[i], true); }
static void track_led_off(int i) { led_set(&track_leds[i], false); }
static void track_all_on(void)   { for (int i = 0; i < NUM_TRACK_LEDS; i++) track_led_on(i); }
static void track_all_off(void)  { for (int i = 0; i < NUM_TRACK_LEDS; i++) track_led_off(i); }

/* Freeze every LED dark at the GPIO level before SYSTEM_OFF latches them. */
static void shutdown_leds(void)
{
	all_off(); track_all_off();
	LED_PWM_TIMER->TASKS_STOP = 1;
	NRF_P0->OUTCLR = LED_ALL_P0;
	NRF_P1->OUTCLR = LED_ALL_P1;
}

/* ------------------------------------------------------- power button ---- */
#define PWR_PORT        NRF_P0
#define PWR_PIN         27u
#define HOLD_MS_TO_OFF  2500

static bool pwr_pressed(void)
{
	return (PWR_PORT->IN & (1u << PWR_PIN)) == 0u;   /* low = pressed */
}
static void pwr_btn_cfg_input(void)
{
	PWR_PORT->PIN_CNF[PWR_PIN] =
		(GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
}
static void pwr_btn_arm_wake(void)
{
	PWR_PORT->PIN_CNF[PWR_PIN] =
		(GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos)|
		(GPIO_PIN_CNF_SENSE_Low     << GPIO_PIN_CNF_SENSE_Pos);
}

/* ---- BQ24232 battery charger control (verified pins) ---- */
#define BQ_PORT         NRF_P0
#define BQ_NCE_PIN      21u   /* charge enable, ACTIVE-LOW */

static void charger_init(void)
{
	BQ_PORT->OUTCLR = (1u << BQ_NCE_PIN);          /* charging enabled */
	BQ_PORT->PIN_CNF[BQ_NCE_PIN] =
		(GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)   |
		(GPIO_PIN_CNF_DRIVE_S0S1    << GPIO_PIN_CNF_DRIVE_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
}

/* ----------------------------------------------------- button ladders ---- */
#define BTN_COM_PORT    NRF_P1
#define BTN_COM_PIN     10u

static const struct adc_dt_spec adc_ladder[] = {
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0),  /* AIN0: PLAY + tracks  */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1),  /* AIN1: Vol + rocker   */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 2),  /* AIN3: Fader 1        */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 3),  /* AIN6: Fader 2        */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 4),  /* AIN2: Fader 3        */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 5),  /* AIN7: Fader 4        */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 6),  /* AIN4: battery        */
};
#define LAD_TRACKS 0
#define LAD_VOL    1
#define LAD_FADER0 2
#define LAD_BATT   6
#define NUM_LADDERS (sizeof(adc_ladder) / sizeof(adc_ladder[0]))

static int16_t adc_sample;

/* Oversampled ladder read (2x average), unchanged from the looper. */
static int ladder_read(const struct adc_dt_spec *spec)
{
	struct adc_sequence seq = {
		.buffer      = &adc_sample,
		.buffer_size = sizeof(adc_sample),
	};
	if (adc_sequence_init_dt(spec, &seq) < 0)
		return -1;
	int32_t acc = 0;
	for (int n = 0; n < 2; n++) {
		if (adc_read_dt(spec, &seq) < 0)
			return -1;
		acc += adc_sample;
	}
	return (int)(acc / 2);
}

static void controls_init(void)
{
	BTN_COM_PORT->OUTSET = (1u << BTN_COM_PIN);
	BTN_COM_PORT->PIN_CNF[BTN_COM_PIN] =
		(GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)   |
		(GPIO_PIN_CNF_DRIVE_S0S1    << GPIO_PIN_CNF_DRIVE_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
	BTN_COM_PORT->OUTSET = (1u << BTN_COM_PIN);

	for (int i = 0; i < NUM_LADDERS; i++) {
		if (device_is_ready(adc_ladder[i].dev))
			adc_channel_setup_dt(&adc_ladder[i]);
	}
}

/* ---- verified voltage-band decoders (copied from the looper firmware) ---- */
enum trk_btn { TRK_NONE = -1, TRK_1, TRK_2, TRK_3, TRK_4, TRK_PLAY };
enum vol_btn { VOL_NONE = -1, VOL_TEMPO_DOWN, VOL_DOWN, VOL_TEMPO_UP, VOL_UP };

static enum trk_btn decode_tracks(int v)
{
	if (v <  110) return TRK_NONE;
	if (v <  300) return TRK_1;     /* ~213  */
	if (v <  560) return TRK_2;     /* ~403  */
	if (v <  950) return TRK_3;     /* ~733  */
	if (v < 1500) return TRK_4;     /* ~1220 */
	return TRK_PLAY;                /* ~1823 */
}
static enum vol_btn decode_vol(int v)
{
	if (v <  200) return VOL_NONE;
	if (v <  560) return VOL_TEMPO_DOWN; /* ~404  = rocker RWD */
	if (v <  950) return VOL_DOWN;       /* ~729  */
	if (v < 1500) return VOL_TEMPO_UP;   /* ~1220 = rocker FWD */
	return VOL_UP;                       /* ~1820 */
}

/* --------------------------------------------------------- watchdog ------ */
static void feed_wdt(void)
{
	for (int ch = 0; ch < 8; ch++)
		NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
}
static void wdt_prewarn(const struct device *dev, int channel_id)
{
	ARG_UNUSED(dev); ARG_UNUSED(channel_id);
}

/* ------------------------------------------------------------ USB MIDI --- */
#define USB_MIDI_NODE DT_NODELABEL(usb_midi)
static const struct device *const midi_dev = DEVICE_DT_GET(USB_MIDI_NODE);
static volatile bool g_midi_ready;
static volatile bool g_send_reset;   /* set from the ready callback */

static void midi_note(uint8_t note, bool down)
{
	if (!g_midi_ready)
		return;
	struct midi_ump ump = UMP_MIDI1_CHANNEL_VOICE(
		ST_MIDI_GROUP, down ? UMP_MIDI_NOTE_ON : UMP_MIDI_NOTE_OFF,
		ST_MIDI_CHANNEL, note, down ? ST_VEL_DOWN : ST_VEL_UP);
	(void)usbd_midi_send(midi_dev, ump);
}

static void midi_cc(uint8_t cc, uint8_t value)
{
	if (!g_midi_ready)
		return;
	struct midi_ump ump = UMP_MIDI1_CHANNEL_VOICE(
		ST_MIDI_GROUP, UMP_MIDI_CONTROL_CHANGE, ST_MIDI_CHANNEL, cc, value);
	(void)usbd_midi_send(midi_dev, ump);
}

/* Host->device packets are ignored in M0: the device has no state to set. */
static void on_midi_packet(const struct device *dev, const struct midi_ump ump)
{
	ARG_UNUSED(dev); ARG_UNUSED(ump);
}

static void on_midi_ready(const struct device *dev, const bool ready)
{
	ARG_UNUSED(dev);
	g_midi_ready = ready;
	if (ready)
		g_send_reset = true;   /* CC123 on (re)connect, sent from main */
}

static const struct usbd_midi_ops midi_ops = {
	.rx_packet_cb = on_midi_packet,
	.ready_cb = on_midi_ready,
};

/* ------------------------------------------------------------ power off -- */
static void power_off(void)
{
	for (int i = NUM_LEDS - 1; i >= 0; i--) {
		led_off(i); track_led_off(i);
		feed_wdt();
		k_msleep(80);
	}
	shutdown_leds();

	while (pwr_pressed()) {        /* don't instantly self-wake */
		feed_wdt();
		k_msleep(20);
	}
	k_msleep(60);
	shutdown_leds();

	pwr_btn_arm_wake();
	feed_wdt();
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;
	__DSB();
	NRF_POWER->SYSTEMOFF = 1u;
	__DSB();
	for (;;) { }
}

/* Track1+Track4 held ~1.2 s -> reset into the UF2 bootloader for reflashing. */
static void enter_dfu(void)
{
	all_off();
	track_all_on();
	NRF_POWER->GPREGRET = 0x57u;
	__DSB();
	NVIC_SystemReset();
	for (;;) { }
}

/* --------------------------------------------------- Stem Tape boot sig -- */
static void boot_signature(void)
{
	for (int f = 0; f < 2; f++) {
		track_all_on();
		feed_wdt();
		k_msleep(90);
		track_all_off();
		feed_wdt();
		k_msleep(110);
	}
}

/* ----------------------------------------------------------- main loop --- */
#define POLL_MS        5
#define DEBOUNCE_PASSES 2      /* a band must repeat before it is committed */
#define FADER_DEADBAND  8      /* raw 12-bit counts (the looper's value) */

static const uint8_t trk_notes[] = {
	ST_NOTE_TRACK1, ST_NOTE_TRACK2, ST_NOTE_TRACK3, ST_NOTE_TRACK4, ST_NOTE_PLAY,
};
static const uint8_t vol_notes[] = {
	ST_NOTE_ROCKER_RWD, ST_NOTE_VOL_DOWN, ST_NOTE_ROCKER_FWD, ST_NOTE_VOL_UP,
};
static const uint8_t fader_cc[] = {
	ST_CC_FADER1, ST_CC_FADER2, ST_CC_FADER3, ST_CC_FADER4,
};

static void all_notes_off(void)
{
	midi_cc(ST_CC_ALL_NOTES_OFF, 0);
}

int main(void)
{
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;   /* BIG FIVE: clear on boot */

	pwr_btn_cfg_input();
	charger_init();
	for (int i = 0; i < NUM_LEDS; i++)       led_cfg_output(&leds[i]);
	for (int i = 0; i < NUM_TRACK_LEDS; i++) led_cfg_output(&track_leds[i]);
	all_off();
	track_all_off();
	led_pwm_init();

	const struct device *wdt = DEVICE_DT_GET(WDT_NODE);
	if (device_is_ready(wdt)) {
		wdt_install_timeout(wdt, &(struct wdt_timeout_cfg){
			.window.max = 4000, .callback = wdt_prewarn,
		});
		wdt_setup(wdt, 0);
	}
	feed_wdt();

	controls_init();
	boot_signature();

	if (device_is_ready(midi_dev)) {
		usbd_midi_set_ops(midi_dev, &midi_ops);
		struct usbd_context *usbd = sample_usbd_init_device(NULL);
		if (usbd != NULL)
			(void)usbd_enable(usbd);
	}
	feed_wdt();

	/* committed physical state */
	enum trk_btn t_committed = TRK_NONE, t_cand = TRK_NONE;
	enum vol_btn v_committed = VOL_NONE, v_cand = VOL_NONE;
	int t_cnt = 0, v_cnt = 0;
	bool fn_down = false;
	int64_t press_start = -1;
	bool press_spent = false;
	int64_t combo14_t = -1;
	int fader_last[4] = { -1, -1, -1, -1 };
	int fader_rr = 0;
	int batt_last = -1;
	int64_t batt_t = 0;

	for (;;) {
		feed_wdt();

		if (g_send_reset) {
			g_send_reset = false;
			all_notes_off();
			/* Resend the current physical state so the host is in sync. */
			if (t_committed != TRK_NONE) midi_note(trk_notes[t_committed], true);
			if (v_committed != VOL_NONE) midi_note(vol_notes[v_committed], true);
			if (fn_down) midi_note(ST_NOTE_FUNCTION, true);
			for (int i = 0; i < 4; i++)
				if (fader_last[i] >= 0)
					midi_cc(fader_cc[i], (uint8_t)(fader_last[i] >> 5));
		}

		/* ---- FUNCTION / power button (raw state + hold-to-power-off) ---- */
		bool fn_now = pwr_pressed();
		if (fn_now && !fn_down) {
			fn_down = true;
			press_start = k_uptime_get();
			press_spent = false;
			midi_note(ST_NOTE_FUNCTION, true);
		} else if (!fn_now && fn_down) {
			fn_down = false;
			press_start = -1;
			midi_note(ST_NOTE_FUNCTION, false);
			all_off();
		}
		if (fn_down && !press_spent) {
			int64_t held = k_uptime_get() - press_start;
			if (held >= HOLD_MS_TO_OFF) {
				midi_note(ST_NOTE_FUNCTION, false);
				all_notes_off();
				power_off();               /* never returns */
			}
			if (held > 400) {              /* power-off countdown */
				int lit = (int)((held * NUM_LEDS) / HOLD_MS_TO_OFF) + 1;
				if (lit > NUM_LEDS) lit = NUM_LEDS;
				all_off();
				for (int i = 0; i < lit; i++) led_on(i);
			}
		}

		/* ---- track / PLAY ladder ---- */
		int trk_raw = ladder_read(&adc_ladder[LAD_TRACKS]);
		enum trk_btn t_raw = TRK_NONE;
		if (trk_raw >= 0) {
			if (trk_raw >= 1280 && trk_raw <= 1390) {   /* Track1+Track4 = DFU */
				if (combo14_t < 0) combo14_t = k_uptime_get();
				else if (k_uptime_get() - combo14_t >= 1200) enter_dfu();
				t_raw = TRK_NONE;
			} else {
				combo14_t = -1;
				t_raw = decode_tracks(trk_raw);
			}
			if (t_raw == t_cand) {
				if (t_cnt < DEBOUNCE_PASSES) t_cnt++;
			} else {
				t_cand = t_raw; t_cnt = 1;
			}
			if (t_cnt >= DEBOUNCE_PASSES && t_cand != t_committed) {
				if (t_committed != TRK_NONE)
					midi_note(trk_notes[t_committed], false);
				t_committed = t_cand;
				if (t_committed != TRK_NONE)
					midi_note(trk_notes[t_committed], true);
			}
		}

		/* ---- Vol / rocker ladder ---- */
		int vol_raw = ladder_read(&adc_ladder[LAD_VOL]);
		if (vol_raw >= 0) {
			enum vol_btn v_raw = decode_vol(vol_raw);
			if (v_raw == v_cand) {
				if (v_cnt < DEBOUNCE_PASSES) v_cnt++;
			} else {
				v_cand = v_raw; v_cnt = 1;
			}
			if (v_cnt >= DEBOUNCE_PASSES && v_cand != v_committed) {
				if (v_committed != VOL_NONE)
					midi_note(vol_notes[v_committed], false);
				v_committed = v_cand;
				if (v_committed != VOL_NONE)
					midi_note(vol_notes[v_committed], true);
			}
		}

		/* ---- faders: one per pass (round-robin keeps ADC cost flat) ---- */
		int fi = fader_rr;
		fader_rr = (fader_rr + 1) & 3;
		int fv = ladder_read(&adc_ladder[LAD_FADER0 + fi]);
		if (fv >= 0) {
			if (fader_last[fi] < 0 ||
			    fv > fader_last[fi] + FADER_DEADBAND ||
			    fv < fader_last[fi] - FADER_DEADBAND) {
				uint8_t prev = (fader_last[fi] < 0) ? 0xFFu
						: (uint8_t)(fader_last[fi] >> 5);
				uint8_t now = (uint8_t)(fv >> 5);
				if (now > 127u) now = 127u;
				fader_last[fi] = fv;
				if (now != prev)
					midi_cc(fader_cc[fi], now);
			}
		}

		/* ---- battery, at most once a second ---- */
		if (k_uptime_get() - batt_t >= 1000) {
			batt_t = k_uptime_get();
			int b = ladder_read(&adc_ladder[LAD_BATT]);
			if (b >= 0) {
				uint8_t v = (uint8_t)(b >> 5);
				if (v > 127u) v = 127u;
				if ((int)v != batt_last) {
					batt_last = v;
					midi_cc(ST_CC_BATTERY, v);
				}
			}
		}

		k_msleep(POLL_MS);
	}
	return 0;
}
