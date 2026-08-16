/*
 * ============================================================================
 *  STEM TAPE FIRMWARE — MILESTONE M0: hardware / MIDI DIAGNOSTIC TARGET
 * ============================================================================
 *  M0 is NOT a product build. It exists to prove two things on real hardware,
 *  before any of it is merged into the SP-1 Tape Looper:
 *
 *    (a) that the SP-1 control surface can be decoded correctly, INCLUDING the
 *        shared-resistor-ladder chords that stock firmware never needed, and
 *    (b) that a class-compliant USB MIDI descriptor produced by Zephyr 4.3.1
 *        is actually accepted by macOS, Chrome Web MIDI and CoreMIDI on iOS.
 *
 *  PINNED BASELINE FOR EVERY COPIED HARDWARE CONSTANT
 *  --------------------------------------------------
 *  All power, DFU, LED, GPIO and ADC-band constants below were copied from
 *  this repository's Tape Looper application source:
 *
 *      file     firmware/src/main.c
 *      commit   a8dd127ba1d595e54f92503a0bd75eabca86334d  ("Changes",
 *               2026-08-15 08:21:32 +0000)
 *
 *  Each copied constant carries an inline [looper a8dd127:<line>] citation.
 *  Constants NOT carrying such a citation are new to M0 and are marked
 *  UNMEASURED where they depend on hardware measurements that do not exist
 *  yet. The Tape Looper target (firmware/) is untouched by this milestone.
 *
 *  SHARED-LADDER REALITY
 *  ---------------------
 *  The SP-1 has no independent digital line per button. PLAY + Track 1-4 sit
 *  on one SAADC ladder (AIN0) and Vol-/Vol+/rocker sit on another (AIN1), so a
 *  chord is a SINGLE new voltage, not two readings. Only ONE chord band was
 *  ever measured by the looper (Track1+Track4, 1280..1390 — the DFU combo).
 *  Every other chord band is UNMEASURED, and this firmware refuses to guess:
 *  an unmeasured reading produces NO MIDI, holds the previous stable mask and
 *  is captured for the CDC diagnostic stream instead.
 *
 *  FUNCTION (P0.27) is a real, separate GPIO and therefore remains
 *  independently detectable in every chord.
 *
 *  SAFETY (unchanged from the pinned revision)
 *  -------------------------------------------
 *  App at 0x20000, watchdog fed < 5 s, SYSTEM_OFF returns to the bootloader,
 *  RESETREAS cleared on boot and before sleep, Track1+Track4 = UF2 bootloader
 *  recovery. This target NEVER touches eMMC: no mount, no format, no write —
 *  the sp1_emmc driver is not even compiled in, so stored Tape Looper audio
 *  cannot be altered. UAC2 audio is disabled in THIS diagnostic target only.
 * ============================================================================
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_midi2.h>
#include <zephyr/audio/midi.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/printk.h>
#include <sample_usbd.h>
#include <soc.h>
#include <string.h>
#include <errno.h>

#include "midi_protocol.h"

/* FAILSAFE: any unrecoverable fault becomes a clean reboot (never a brick).
 * [looper a8dd127: k_sys_fatal_error_handler] */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(reason);
	ARG_UNUSED(esf);
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

#define WDT_NODE DT_ALIAS(watchdog0)

/* ---------------------------------------------------------------- LEDs ---
 * Pin map and soft-PWM dimming copied from [looper a8dd127:101-108, 3906-3988].
 */
struct led { NRF_GPIO_Type *port; uint32_t pin; };

/* the 4 playback / status LEDs (center row)  [looper a8dd127:101-103] */
static const struct led leds[] = {
	{ NRF_P1, 13 }, { NRF_P0, 0 }, { NRF_P1, 12 }, { NRF_P0, 1 },
};
#define NUM_LEDS (sizeof(leds) / sizeof(leds[0]))

/* the 4 TRACK LEDs (directly above buttons 1-4)  [looper a8dd127:107-109] */
static const struct led track_leds[] = {
	{ NRF_P0, 29 }, { NRF_P0, 26 }, { NRF_P1, 15 }, { NRF_P1, 14 },
};
#define NUM_TRACK_LEDS (sizeof(track_leds) / sizeof(track_leds[0]))

#define LED_PWM_PERIOD_US 1000u   /* [looper a8dd127:3906] */
#define LED_PWM_ON_US       52u   /* [looper a8dd127:3907] track row duty  */
#define LED_STATUS_ON_US    66u   /* [looper a8dd127:3913] status row duty */
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
static void track_led_off(int i) { led_set(&track_leds[i], false); }
static void track_led_on(int i)  { led_set(&track_leds[i], true); }
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

/* ------------------------------------------------------- power button ----
 * [looper a8dd127:123-124, 131, 4231-4249] */
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

/* ---- BQ24232 battery charger control  [looper a8dd127:128, 4274-4279] ---- */
#define BQ_PORT         NRF_P0
#define BQ_NCE_PIN      21u   /* charge enable, ACTIVE-LOW */

static void charger_init(void)
{
	BQ_PORT->OUTCLR = (1u << BQ_NCE_PIN);          /* charging enabled */
	BQ_PORT->PIN_CNF[BQ_NCE_PIN] =
		(GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)   |
		(GPIO_PIN_CNF_DRIVE_S0S1    << GPIO_PIN_CNF_DRIVE_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
	BQ_PORT->OUTCLR = (1u << BQ_NCE_PIN);
}

/* ----------------------------------------------------- button ladders ----
 * [looper a8dd127:140-156] */
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

/* Oversampled ladder read (2x average)  [looper a8dd127:192-207]. */
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

/* ======================= SHARED-LADDER BITMASK DECODE =====================
 * Every control is one bit. A ladder reading maps to a MASK, never to a
 * single enum, so a measured chord band can express both of its buttons.
 *
 * MEASURED BANDS ONLY. The pinned looper revision contains exactly these
 * measured AIN0/AIN1 bands:
 *   AIN0 singles  [looper a8dd127:3879-3887] : T1 ~213, T2 ~403, T3 ~733,
 *                                              T4 ~1220, PLAY ~1823
 *   AIN0 chord    [looper a8dd127:5115-5124] : Track1+Track4 = 1280..1390
 *   AIN1 singles  [looper a8dd127:3889-3896] : RWD ~404, VOL- ~729,
 *                                              FWD ~1220, VOL+ ~1820
 * There is NO measured table for PLAY+Track, for Vol- +Vol+, or for
 * rocker+Vol. Those readings fall into the gaps below and are reported as
 * UNMEASURED: no MIDI is emitted, the previous stable mask is held, and the
 * raw code is captured for the CDC diagnostic stream so it can be measured.
 *
 * NOTE ON A DELIBERATE DIFFERENCE FROM THE LOOPER: decode_tracks() maps the
 * whole 950..1499 window to Track 4 and >=1500 to PLAY. That is safe for a
 * looper (it only ever needed singles) but it would decode the known
 * Track1+Track4 chord region's shoulders, and any PLAY+Track chord, as an
 * unrelated single button. M0 therefore narrows the single-button bands to
 * their measured neighbourhoods and calls everything else UNMEASURED.
 */
#define BTN_T1        (1u << 0)
#define BTN_T2        (1u << 1)
#define BTN_T3        (1u << 2)
#define BTN_T4        (1u << 3)
#define BTN_PLAY      (1u << 4)
#define BTN_VOL_UP    (1u << 5)
#define BTN_VOL_DOWN  (1u << 6)
#define BTN_ROCK_FWD  (1u << 7)
#define BTN_ROCK_RWD  (1u << 8)
#define BTN_FUNCTION  (1u << 9)   /* P0.27, a real independent GPIO */

#define LADDER_MASK_AIN0 (BTN_T1|BTN_T2|BTN_T3|BTN_T4|BTN_PLAY)
#define LADDER_MASK_AIN1 (BTN_VOL_UP|BTN_VOL_DOWN|BTN_ROCK_FWD|BTN_ROCK_RWD)

struct band { int lo; int hi; uint32_t mask; };

/* AIN0: PLAY + Track 1-4 (and the one measured chord). */
static const struct band bands_ain0[] = {
	{    0,  109, 0u },                    /* idle          [3880] */
	{  110,  299, BTN_T1 },                /* ~213          [3881] */
	{  300,  559, BTN_T2 },                /* ~403          [3882] */
	{  560,  949, BTN_T3 },                /* ~733          [3883] */
	{  950, 1279, BTN_T4 },                /* ~1220         [3884] */
	{ 1280, 1390, BTN_T1 | BTN_T4 },       /* measured chord [5124] */
	{ 1600, 2047, BTN_PLAY },              /* ~1823         [3885] */
};

/* AIN1: Vol-/Vol+ and the tempo rocker. */
static const struct band bands_ain1[] = {
	{    0,  199, 0u },                    /* idle          [3891] */
	{  200,  559, BTN_ROCK_RWD },          /* ~404          [3892] */
	{  560,  949, BTN_VOL_DOWN },          /* ~729          [3893] */
	{  950, 1499, BTN_ROCK_FWD },          /* ~1220         [3894] */
	{ 1600, 2047, BTN_VOL_UP },            /* ~1820         [3895] */
};

#define MASK_UNMEASURED 0xFFFFFFFFu

static uint32_t decode_bands(const struct band *tbl, size_t n, int v)
{
	if (v < 0)
		return MASK_UNMEASURED;
	for (size_t i = 0; i < n; i++)
		if (v >= tbl[i].lo && v <= tbl[i].hi)
			return tbl[i].mask;
	return MASK_UNMEASURED;   /* a real chord we have never measured */
}

/* --------------------------------------------------------- watchdog ------
 * The TE bootloader may already have STARTED the nRF52840 WDT before handing
 * over. Once running, its CONFIG/CRV/RREN are locked: wdt_install_timeout()
 * and wdt_setup() will fail and MUST NOT be assumed to have succeeded. We
 * therefore (a) detect RUNSTATUS first, (b) record every return value, and
 * (c) always feed exactly the reload channels that are ENABLED (RREN), which
 * is the set the running configuration actually requires.
 */
static int  g_wdt_install_rc = -ENODEV;   /* -ENODEV = never attempted */
static int  g_wdt_setup_rc   = -ENODEV;
static bool g_wdt_pre_running;            /* started by the bootloader */
static bool g_wdt_ours;                   /* started by this firmware  */

static void feed_wdt(void)
{
	uint32_t en = NRF_WDT->RREN;
	if (en == 0u) {
		/* No channel enabled (WDT idle, or RREN not yet latched):
		 * feeding all channels is harmless and keeps startup safe. */
		en = 0xFFu;
	}
	for (int ch = 0; ch < 8; ch++)
		if (en & (1u << ch))
			NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
}
static void wdt_prewarn(const struct device *dev, int channel_id)
{
	ARG_UNUSED(dev); ARG_UNUSED(channel_id);
}

/* Returns nothing: failure to configure the watchdog is never fatal, it is
 * recorded and reported over the CDC diagnostic banner instead. */
static void wdt_init(void)
{
	g_wdt_pre_running =
		(NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUSWDT_Msk) != 0u;
	feed_wdt();
	if (g_wdt_pre_running)
		return;                   /* locked configuration; feed only */

	const struct device *wdt = DEVICE_DT_GET(WDT_NODE);
	if (!device_is_ready(wdt))
		return;

	struct wdt_timeout_cfg cfg = {
		.window.max = 4000, .callback = wdt_prewarn,
	};
	g_wdt_install_rc = wdt_install_timeout(wdt, &cfg);
	if (g_wdt_install_rc < 0)
		return;

	g_wdt_setup_rc = wdt_setup(wdt, 0);
	if (g_wdt_setup_rc < 0)
		return;

	g_wdt_ours = true;
	feed_wdt();
}


/* ------------------------------------------------------------ USB MIDI ---
 * Zephyr 4.3.1 ships only the USB MIDI 2.0 class (CONFIG_USBD_MIDI2_CLASS);
 * the USB-MIDI 1.0 alternate setting is NOT implemented in that revision.
 * The MIDI 2.0 class descriptor declares alternate setting 0 (MIDI 1.0-style
 * legacy interface with no endpoints) and alternate setting 1 (UMP). We
 * therefore send MIDI 1.0 Channel Voice messages wrapped in UMP message
 * type 2, which hosts translate back to plain MIDI 1.0 events.
 *
 * ready_cb fires when the host SELECTS the operational alternate setting, so
 * the CC123 + full-state resend below is gated on it and never runs against
 * an interface that is merely enumerated. */
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
		g_send_reset = true;   /* CC123 + resend, emitted from main */
}

static const struct usbd_midi_ops midi_ops = {
	.rx_packet_cb = on_midi_packet,
	.ready_cb = on_midi_ready,
};

/* ------------------------------------------------- CDC ACM diagnostics ---
 * The composite device is MIDI2 + CDC ACM (no UAC2). Diagnostics print ONLY
 * when a host has the port open with DTR asserted, ONLY when one of the two
 * ladder readings actually changes, and never faster than DIAG_MIN_GAP_MS.
 */
static const struct device *const cdc = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

#define DIAG_MIN_GAP_MS  40
#define DIAG_HYST        3     /* raw counts: ignore pure ADC dither */

static bool diag_open(void)
{
	uint32_t dtr = 0;
	if (!device_is_ready(cdc))
		return false;
	(void)uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr);
	return dtr != 0;
}

static void mask_str(uint32_t m, char *out, size_t n)
{
	if (m == MASK_UNMEASURED) {
		strncpy(out, "UNMEASURED", n);
		out[n - 1] = '\0';
		return;
	}
	/* fixed field order: T1 T2 T3 T4 PLAY VOL+ VOL- FWD RWD FN */
	static const char *const names[] = { "T1","T2","T3","T4","PLAY",
					     "VOL+","VOL-","FWD","RWD","FN" };
	size_t p = 0;
	out[0] = '\0';
	for (int b = 0; b < 10; b++) {
		if (!(m & (1u << b)))
			continue;
		size_t l = strlen(names[b]);
		if (p + l + 2 >= n)
			break;
		if (p) out[p++] = '+';
		memcpy(&out[p], names[b], l);
		p += l;
		out[p] = '\0';
	}
	if (p == 0)
		strncpy(out, "-", n);
}

/* ------------------------------------------------------------ power off --
 * [looper a8dd127: power_off()] — SYSTEM_OFF hands control back to the
 * bootloader; RESETREAS is cleared first. */
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

/* RECOVERY, preserved verbatim from the pinned revision
 * [looper a8dd127:4386-4396]: Track1+Track4 held ~1.2 s -> UF2 bootloader. */
static void enter_dfu(void)
{
	all_off();
	track_all_on();
	NRF_POWER->GPREGRET = 0x57u;
	__DSB();
	NVIC_SystemReset();
	for (;;) { }
}

/* ------------------------------------------------- EARLY DFU ESCAPE ------
 * Runs immediately after controls_init(), BEFORE boot_signature(),
 * sample_usbd_init_device(), usbd_enable() and every other USB call, so the
 * recovery combo still reaches the UF2 bootloader even if USB init would
 * later hang, fault or brick the visible behaviour of the image.
 *
 * Band 1280..1390 = the ONE measured Track1+Track4 chord
 * [looper a8dd127:5115-5124]. Outside it we return instantly: no boot delay
 * is added to a normal power-up. A failed ADC read (-1) is treated as
 * "not held" and returns to normal startup.
 */
#define ESCAPE_LO       1280
#define ESCAPE_HI       1390
#define ESCAPE_HOLD_MS  1200
#define ESCAPE_STEP_MS  10

static void early_dfu_escape(void)
{
	int v = ladder_read(&adc_ladder[LAD_TRACKS]);
	if (v < ESCAPE_LO || v > ESCAPE_HI)
		return;                                   /* no boot delay */

	int64_t t0 = k_uptime_get();
	while (k_uptime_get() - t0 < ESCAPE_HOLD_MS) {
		feed_wdt();                               /* all enabled channels */
		k_msleep(ESCAPE_STEP_MS);
		v = ladder_read(&adc_ladder[LAD_TRACKS]);
		if (v < ESCAPE_LO || v > ESCAPE_HI)
			return;                           /* released / read fail */
	}
	feed_wdt();
	enter_dfu();                                      /* never returns */
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
#define POLL_MS         5
#define DEBOUNCE_PASSES 2      /* a mask must repeat before it is committed */
#define FADER_DEADBAND  8      /* raw 12-bit counts  [looper a8dd127:1865ff] */

/* bit index -> note number, same order as the BTN_* bits above */
static const uint8_t bit_notes[10] = {
	ST_NOTE_TRACK1, ST_NOTE_TRACK2, ST_NOTE_TRACK3, ST_NOTE_TRACK4,
	ST_NOTE_PLAY,   ST_NOTE_VOL_UP, ST_NOTE_VOL_DOWN,
	ST_NOTE_ROCKER_FWD, ST_NOTE_ROCKER_RWD, ST_NOTE_FUNCTION,
};
static const uint8_t fader_cc[] = {
	ST_CC_FADER1, ST_CC_FADER2, ST_CC_FADER3, ST_CC_FADER4,
};

static void all_notes_off(void)
{
	midi_cc(ST_CC_ALL_NOTES_OFF, 0);
}

/* Emit one Note On/Off per changed bit, comparing stable masks. */
static void emit_mask_delta(uint32_t prev, uint32_t now)
{
	uint32_t diff = prev ^ now;
	for (int b = 0; b < 10; b++) {
		if (!(diff & (1u << b)))
			continue;
		midi_note(bit_notes[b], (now & (1u << b)) != 0u);
	}
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

	wdt_init();          /* detects a bootloader-started WDT, checks rc */

	controls_init();
	early_dfu_escape();  /* BEFORE boot_signature() and all USB init */
	boot_signature();


	if (device_is_ready(midi_dev)) {
		usbd_midi_set_ops(midi_dev, &midi_ops);
		struct usbd_context *usbd = sample_usbd_init_device(NULL);
		if (usbd != NULL)
			(void)usbd_enable(usbd);
	}
	feed_wdt();

	/* committed physical state, as bitmasks */
	uint32_t stable = 0u;                  /* debounced, published mask */
	uint32_t cand = 0u;                    /* candidate awaiting repeats */
	int cand_cnt = 0;
	int64_t press_start = -1;
	bool press_spent = false;
	int64_t combo14_t = -1;
	int fader_last[4] = { -1, -1, -1, -1 };
	int fader_rr = 0;
	int batt_last = -1;
	int64_t batt_t = 0;

	/* diagnostics */
	int64_t diag_t = 0;
	int diag_a0 = -9999, diag_a1 = -9999;
	uint32_t unmeasured_hits = 0;
	bool banner_done = false;

	for (;;) {
		feed_wdt();

		if (g_send_reset) {
			g_send_reset = false;
			all_notes_off();
			/* Resend the current physical state so the host is in sync. */
			emit_mask_delta(0u, stable);
			for (int i = 0; i < 4; i++)
				if (fader_last[i] >= 0)
					midi_cc(fader_cc[i], (uint8_t)(fader_last[i] >> 5));
		}

		/* ---- FUNCTION (independent GPIO) + hold-to-power-off ---- */
		bool fn_now = pwr_pressed();
		bool fn_was = (stable & BTN_FUNCTION) != 0u;
		if (fn_now && !fn_was) {
			press_start = k_uptime_get();
			press_spent = false;
		} else if (!fn_now && fn_was) {
			press_start = -1;
			all_off();
		}
		if (fn_now && !press_spent && press_start >= 0) {
			int64_t held = k_uptime_get() - press_start;
			if (held >= HOLD_MS_TO_OFF) {
				emit_mask_delta(stable, 0u);
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

		/* ---- read both shared ladders ---- */
		int a0 = ladder_read(&adc_ladder[LAD_TRACKS]);
		int a1 = ladder_read(&adc_ladder[LAD_VOL]);

		/* RECOVERY combo is checked on the RAW code, before decoding, so a
		 * measured Track1+Track4 hold still reaches the bootloader.
		 * [looper a8dd127:5115-5124] */
		if (a0 >= 1280 && a0 <= 1390) {
			if (combo14_t < 0) combo14_t = k_uptime_get();
			else if (k_uptime_get() - combo14_t >= 1200) enter_dfu();
		} else {
			combo14_t = -1;
		}

		uint32_t m0 = decode_bands(bands_ain0, ARRAY_SIZE(bands_ain0), a0);
		uint32_t m1 = decode_bands(bands_ain1, ARRAY_SIZE(bands_ain1), a1);
		bool unmeasured = (m0 == MASK_UNMEASURED) || (m1 == MASK_UNMEASURED);
		if (unmeasured)
			unmeasured_hits++;

		/* An unmeasured reading must never be decoded as some unrelated
		 * single button: hold the previous stable contribution instead. */
		uint32_t raw_mask =
			((m0 == MASK_UNMEASURED) ? (stable & LADDER_MASK_AIN0) : m0) |
			((m1 == MASK_UNMEASURED) ? (stable & LADDER_MASK_AIN1) : m1) |
			(fn_now ? BTN_FUNCTION : 0u);

		if (raw_mask == cand) {
			if (cand_cnt < DEBOUNCE_PASSES) cand_cnt++;
		} else {
			cand = raw_mask; cand_cnt = 1;
		}
		if (cand_cnt >= DEBOUNCE_PASSES && cand != stable) {
			emit_mask_delta(stable, cand);
			stable = cand;
		}

		/* ---- CDC diagnostics: only on ladder change, rate limited ---- */
		if (diag_open()) {
			int64_t now = k_uptime_get();
			bool changed = (a0 > diag_a0 + DIAG_HYST) || (a0 < diag_a0 - DIAG_HYST) ||
				       (a1 > diag_a1 + DIAG_HYST) || (a1 < diag_a1 - DIAG_HYST);
			if (!banner_done) {
				printk("%s  diagnostic target: USB MIDI2 + CDC ACM, no UAC2, "
				       "eMMC never touched\n", ST_FW_VERSION);
				printk("wdt: pre_running=%d ours=%d install_rc=%d setup_rc=%d "
				       "rren=0x%02x runstatus=%lu\n",
				       (int)g_wdt_pre_running, (int)g_wdt_ours,
				       g_wdt_install_rc, g_wdt_setup_rc,
				       (unsigned)(NRF_WDT->RREN & 0xFFu),
				       (unsigned long)NRF_WDT->RUNSTATUS);
				printk("fields: AIN0 AIN1 decoded stable [unmeasured-count]\n");

				banner_done = true;
				diag_t = now;
			} else if (changed && now - diag_t >= DIAG_MIN_GAP_MS) {
				char db[64], sb[64];
				uint32_t dm = (m0 == MASK_UNMEASURED || m1 == MASK_UNMEASURED)
					      ? MASK_UNMEASURED
					      : (m0 | m1 | (fn_now ? BTN_FUNCTION : 0u));
				mask_str(dm, db, sizeof(db));
				mask_str(stable, sb, sizeof(sb));
				printk("AIN0=%4d AIN1=%4d dec=%-12s stable=%-12s unmeas=%u\n",
				       a0, a1, db, sb, (unsigned)unmeasured_hits);
				diag_t = now;
				diag_a0 = a0;
				diag_a1 = a1;
			}
		} else {
			banner_done = false;
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
				uint8_t now8 = (uint8_t)(fv >> 5);
				if (now8 > 127u) now8 = 127u;
				fader_last[fi] = fv;
				if (now8 != prev)
					midi_cc(fader_cc[fi], now8);
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
