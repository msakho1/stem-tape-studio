/*
 * ============================================================================
 *  STEM TAPE PLAYER — standalone four-stem firmware (release skeleton)
 * ============================================================================
 * This target is the beginning of the standalone player: it boots the
 * board and proves out, on the real hardware build, the primitives every
 * later piece depends on -- watchdog, power button hold-to-off, the early
 * Track1+Track4 DFU recovery combo (checked BEFORE any other subsystem, so
 * recovery works even if something later hangs), the charger GPIOs, and
 * the eight-LED PWM renderer driving the new local semantic pattern engine
 * (st_led_pattern.h) instead of the M0 target's MIDI-driven one.
 *
 * DEFERRED beyond this pass (see firmware/stemtape_player/README.md and
 * the top-level release report for the full list): USB audio / UAC2, the
 * eMMC driver and the transfer-protocol eMMC binding (st_transfer.h's
 * pure state machine is implemented and host-tested; its emmc_read_blocks/
 * emmc_write_blocks glue is not yet wired here), CS42L42/TAS2505 codec
 * bring-up, and the real-time decode/mix/FX/scrub audio render path. The
 * physical control scanner is not yet wired to st_gesture.h's edge/tick
 * API either -- that wiring, plus the ADC ladder band table it depends on,
 * is proven in firmware/stemtape/src/main.c and firmware/src/main.c and
 * needs porting here alongside the audio engine, not invented ahead of it.
 *
 * Everything ABOVE the deferred line is real, reused, and (for the DFU
 * combo) uses the exact measured band already proven in the pinned Tape
 * Looper and the M0 target: [looper a8dd127:5115-5124].
 * ============================================================================
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/printk.h>
#include <soc.h>
#include <errno.h>
#include <string.h>

#include "led_protocol.h"
#include "led_render.h"
#include "st_led_pattern.h"

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

/* ------------------------------------------------------- power button ----
 * [looper a8dd127:123-124, 131, 4231-4249] */
#define PWR_PORT        NRF_P0
#define PWR_PIN         27u
#define HOLD_MS_TO_OFF  2500

static bool pwr_pressed(void)
{
	return (PWR_PORT->IN & (1u << PWR_PIN)) == 0u;
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

/* ---- BQ24232 battery charger control  [looper a8dd127:126-130, 4260-4290] --
 * Charge-enable only in this skeleton; the full nCHG/nPGOOD status read
 * and local charging gauge (proven in firmware/stemtape's led_battery.c)
 * is ported alongside the rest of the deferred audio/battery integration. */
#define BQ_PORT         NRF_P0
#define BQ_NCE_PIN      21u

static void charger_init(void)
{
	BQ_PORT->OUTCLR = (1u << BQ_NCE_PIN);
	BQ_PORT->PIN_CNF[BQ_NCE_PIN] =
		(GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)   |
		(GPIO_PIN_CNF_DRIVE_S0S1    << GPIO_PIN_CNF_DRIVE_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
	BQ_PORT->OUTCLR = (1u << BQ_NCE_PIN);
}

/* ----------------------------------------------------- DFU escape ADC ----
 * Only the Track1+Track4 ladder band is needed pre-boot; the full button
 * ladder decode table is part of the deferred control-scanner port.
 * [looper a8dd127:140-156, 192-207] */
static const struct adc_dt_spec adc_tracks = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);
#define BTN_COM_PORT NRF_P1
#define BTN_COM_PIN  10u

static int16_t adc_sample;

static int ladder_read(const struct adc_dt_spec *spec)
{
	struct adc_sequence seq = { .buffer = &adc_sample, .buffer_size = sizeof(adc_sample) };
	int32_t acc = 0;
	int n;

	if (adc_sequence_init_dt(spec, &seq) < 0) {
		return -1;
	}
	for (n = 0; n < 2; n++) {
		if (adc_read_dt(spec, &seq) < 0) {
			return -1;
		}
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
	if (device_is_ready(adc_tracks.dev)) {
		adc_channel_setup_dt(&adc_tracks);
	}
}

/* --------------------------------------------------------- watchdog ------
 * [looper a8dd127: wdt_init pattern, identical to the M0 target's] */
static int  g_wdt_install_rc = -ENODEV;
static int  g_wdt_setup_rc   = -ENODEV;
static bool g_wdt_pre_running;
static bool g_wdt_ours;

static void feed_wdt(void)
{
	uint32_t en = NRF_WDT->RREN;

	if (en == 0u) {
		en = 0xFFu;
	}
	for (int ch = 0; ch < 8; ch++) {
		if (en & (1u << ch)) {
			NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
		}
	}
}
static void wdt_prewarn(const struct device *dev, int channel_id)
{
	ARG_UNUSED(dev); ARG_UNUSED(channel_id);
}
static void wdt_init(void)
{
	g_wdt_pre_running = (NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) != 0u;
	feed_wdt();
	if (g_wdt_pre_running) {
		return;
	}

	const struct device *wdt = DEVICE_DT_GET(WDT_NODE);

	if (!device_is_ready(wdt)) {
		return;
	}

	struct wdt_timeout_cfg cfg = { .window.max = 4000, .callback = wdt_prewarn };

	g_wdt_install_rc = wdt_install_timeout(wdt, &cfg);
	if (g_wdt_install_rc < 0) {
		return;
	}
	g_wdt_setup_rc = wdt_setup(wdt, 0);
	if (g_wdt_setup_rc < 0) {
		return;
	}
	g_wdt_ours = true;
	feed_wdt();
}

/* ------------------------------------------------------------ power off --
 * [looper a8dd127: power_off()] */
static void power_off(void)
{
	uint8_t off[LED_PHYSICAL_COUNT] = { 0 };

	(void)led_render_apply(off);
	while (pwr_pressed()) {
		feed_wdt();
		k_msleep(20);
	}
	k_msleep(60);
	(void)led_render_apply(off);

	pwr_btn_arm_wake();
	feed_wdt();
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;
	__DSB();
	NRF_POWER->SYSTEMOFF = 1u;
	__DSB();
	for (;;) { }
}

/* ------------------------------------------------- EARLY DFU ESCAPE ------
 * Runs immediately after controls_init(), before any USB/audio init, so
 * recovery works even if later bring-up would hang. Same measured band as
 * the pinned looper and the M0 target. [looper a8dd127:5115-5124] */
#define ESCAPE_LO       1280
#define ESCAPE_HI       1390
#define ESCAPE_HOLD_MS  1200
#define ESCAPE_STEP_MS  10
#define DFU_FLASH_HOLD_MS 300 /* human-visible hold, matching the M0 target's corrected value */

static void enter_dfu(void)
{
	uint8_t all_on[LED_PHYSICAL_COUNT];

	for (uint8_t i = 0; i < LED_TRACK_ROW_COUNT; i++) {
		all_on[i] = (uint8_t)LED_LEVEL_MAX;
	}
	for (uint8_t i = LED_TRACK_ROW_COUNT; i < LED_PHYSICAL_COUNT; i++) {
		all_on[i] = 0u;
	}
	(void)led_render_apply(all_on);
	feed_wdt();
	k_msleep(DFU_FLASH_HOLD_MS);
	NRF_POWER->GPREGRET = 0x57u;
	__DSB();
	NVIC_SystemReset();
	for (;;) { }
}

static void early_dfu_escape(void)
{
	int v = ladder_read(&adc_tracks);

	if (v < ESCAPE_LO || v > ESCAPE_HI) {
		return;
	}

	int64_t t0 = k_uptime_get();

	while (k_uptime_get() - t0 < ESCAPE_HOLD_MS) {
		feed_wdt();
		k_msleep(ESCAPE_STEP_MS);
		v = ladder_read(&adc_tracks);
		if (v < ESCAPE_LO || v > ESCAPE_HI) {
			return;
		}
	}
	feed_wdt();
	enter_dfu();
}

/* ----------------------------------------------------------- main loop --- */
#define POLL_MS 5

int main(void)
{
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;

	pwr_btn_cfg_input();
	charger_init();
	(void)led_render_init(); /* PWM2/PWM3; no-ops entirely if any channel isn't ready */

	wdt_init();
	printk("stemtape-player boot: wdt pre_running=%d ours=%d install_rc=%d setup_rc=%d\n",
	       (int)g_wdt_pre_running, (int)g_wdt_ours, g_wdt_install_rc, g_wdt_setup_rc);
	controls_init();
	early_dfu_escape(); /* before every other subsystem */

	st_led_oneshot_t boot_flash;
	uint32_t boot_now = (uint32_t)k_uptime_get();

	st_led_oneshot_start(&boot_flash, ST_LED_ONESHOT_BOOT_FLASH, boot_now);

	int64_t press_start = -1;
	bool press_spent = false;

	for (;;) {
		feed_wdt();

		uint32_t now = (uint32_t)k_uptime_get();
		bool fn_now = pwr_pressed();

		if (fn_now && press_start < 0) {
			press_start = k_uptime_get();
			press_spent = false;
		} else if (!fn_now && press_start >= 0) {
			press_start = -1;
		}
		if (fn_now && !press_spent && press_start >= 0) {
			int64_t held = k_uptime_get() - press_start;

			if (held >= HOLD_MS_TO_OFF) {
				press_spent = true;
				power_off(); /* never returns */
			}
		}

		/* Render: one-shot boot flash first, then the idle base
		 * pattern with placeholder inputs -- "playing"/battery/
		 * stem-status are all owned by the deferred audio engine;
		 * this proves the renderer and priority table on real
		 * hardware ahead of that integration. */
		st_led_inputs_t in;

		memset(&in, 0, sizeof(in));
		in.battery_level = 0xFFu; /* unavailable: not wired yet, never fabricated */

		st_led_base_t base = st_led_select_base(false, false, false, false);
		st_led_frame_t frame;

		st_led_render(&boot_flash, base, &in, now, &frame);
		(void)led_render_apply(frame.level);

		k_msleep(POLL_MS);
	}
	return 0;
}
