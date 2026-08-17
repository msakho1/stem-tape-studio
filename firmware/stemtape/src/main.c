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
 *
 *  v1.1.0: STEM TAPE LED FEEDBACK PROTOCOL v1
 *  -------------------------------------------
 *  Adds an eight-channel physical LED renderer (hardware PWM2/PWM3 — see
 *  led_protocol.h/led_render.c) and a host-to-device brightness transport on
 *  MIDI channel 16 (led_frame.h/led_midi.h). The website remains the sole
 *  owner of all gesture/loop/scrub/FX/mixer/precedence state; this firmware
 *  only stages, atomically commits and renders an already-resolved 8-value
 *  brightness frame under a 1000 ms lease, and never invents Stem Tape
 *  behavior of its own. Every existing M0 control, safety and diagnostic
 *  behavior above is unchanged. See docs/stem-tape-led-feedback-v1.md.
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
#include "led_protocol.h"
#include "led_frame.h"
#include "led_midi.h"
#include "led_render.h"

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
 * Eight independently PWM-driven physical channels (PWM2 = track row,
 * PWM3 = playback row) replace the prior TIMER3 software-PWM row driver —
 * see led_protocol.h for the index/GPIO table and led_render.c for the
 * hardware driver. There is no LED ISR anymore at all: the nRF PWM
 * peripheral generates each channel's waveform autonomously in hardware
 * once programmed, so there is no software-PWM loop cost or flicker source
 * to preserve.
 *
 * This section holds two independent LED sources:
 *   (a) g_pattern_frame — the LOCAL safety/boot-signature pattern, the same
 *       visual behavior as the pinned Tape Looper revision
 *       [looper a8dd127:101-108, 3906-3988], now expressed as brightness
 *       levels rendered through hardware PWM;
 *   (b) g_led_state — the Stem Tape LED Feedback Protocol v1 host frame
 *       (led_frame.h) plus small irq_lock()-guarded wrappers that make
 *       committing/reading it race-safe against the USB MIDI RX path
 *       running on a different thread than the main loop.
 *
 * Precedence (docs/stem-tape-led-feedback-v1.md "Safety precedence"): DFU
 * escape, fatal/reset, shutdown and boot signature are all synchronous,
 * blocking sections of main()/enter_dfu()/power_off()/boot_signature() —
 * nothing else can render while they run, so calling led_render_apply()
 * directly from those functions on g_pattern_frame IS the precedence
 * mechanism for them. The one non-blocking case that runs interleaved with
 * the main loop — the FUNCTION hold-to-power-off countdown — is handled
 * explicitly by the `countdown_active` render-selection in main()'s loop.
 */

/* Legacy GPIO-index -> new physical-index tables, so the exact same GPIO is
 * still driven at each legacy call site. "Status" is the old 4-LED center
 * row (now physical indices 4-7, in reverse: leds[0]=P1.13 was physical
 * index 7, leds[3]=P0.01 was physical index 4); "track" is the old 4-LED
 * track row (physical indices 0-3, identity). [looper a8dd127:101-103] */
static const uint8_t STATUS_TO_PHYSICAL[4] = {
	LED_IDX_PLAYBACK4, LED_IDX_PLAYBACK3, LED_IDX_PLAYBACK2, LED_IDX_PLAYBACK1,
};
#define NUM_LEDS       4  /* old "status" row length; kept for call-site compatibility */
#define NUM_TRACK_LEDS 4  /* old "track" row length */

static uint8_t g_pattern_frame[LED_PHYSICAL_COUNT]; /* local safety/boot pattern, 0 or LED_LEVEL_MAX */

static void pat_set(uint8_t physical_idx, bool on)
{
	g_pattern_frame[physical_idx] = on ? (uint8_t)LED_LEVEL_MAX : 0u;
}
static void pat_show(void) { led_render_apply(g_pattern_frame); }

static void led_on(int i)        { pat_set(STATUS_TO_PHYSICAL[i], true); }
static void led_off(int i)       { pat_set(STATUS_TO_PHYSICAL[i], false); }
static void all_off(void)        { for (int i = 0; i < NUM_LEDS; i++) led_off(i); }
static void track_led_off(int i) { pat_set((uint8_t)i, false); }
static void track_led_on(int i)  { pat_set((uint8_t)i, true); }
static void track_all_on(void)   { for (int i = 0; i < NUM_TRACK_LEDS; i++) track_led_on(i); }
static void track_all_off(void)  { for (int i = 0; i < NUM_TRACK_LEDS; i++) track_led_off(i); }

/* Freeze every LED dark before SYSTEM_OFF latches the pin states. */
static void shutdown_leds(void)
{
	all_off();
	track_all_off();
	pat_show();
}

/* ---- Stem Tape LED Feedback Protocol v1: host frame + ownership lease ----
 * g_led_state is mutated from on_midi_packet() (USB MIDI RX context) and
 * read from the main loop's render step; every access is wrapped in a short
 * irq_lock()/irq_unlock() critical section (a handful of byte copies, no
 * allocation, no logging, no blocking call) so a commit can never be read
 * half-applied — "one logical transaction" — regardless of which thread the
 * USB stack runs its class callbacks on. led_frame.c itself stays pure and
 * Zephyr-free; this is the only place Zephyr locking is layered on top of
 * it. */
static led_frame_state_t g_led_state;

static void led_snapshot_active(uint8_t out[LED_PHYSICAL_COUNT], bool *owned)
{
	unsigned int key = irq_lock();

	*owned = g_led_state.owned;
	if (*owned)
		memcpy(out, g_led_state.active, LED_PHYSICAL_COUNT);
	irq_unlock(key);
}
static void led_state_stage(uint8_t index, uint8_t level)
{
	unsigned int key = irq_lock();

	led_frame_stage(&g_led_state, index, level);
	irq_unlock(key);
}
static led_commit_result_t led_state_commit(uint8_t seq, uint32_t now_ms)
{
	unsigned int key = irq_lock();
	led_commit_result_t r = led_frame_commit(&g_led_state, seq, now_ms);

	irq_unlock(key);
	return r;
}
static led_heartbeat_result_t led_state_heartbeat(uint8_t seq, uint32_t now_ms)
{
	unsigned int key = irq_lock();
	led_heartbeat_result_t r = led_frame_heartbeat(&g_led_state, seq, now_ms);

	irq_unlock(key);
	return r;
}
static void led_state_release(led_release_reason_t reason, uint32_t now_ms)
{
	unsigned int key = irq_lock();

	led_frame_release(&g_led_state, reason, now_ms);
	irq_unlock(key);
}
static bool led_state_check_timeout(uint32_t now_ms)
{
	unsigned int key = irq_lock();
	bool timed_out = led_frame_check_lease_timeout(&g_led_state, now_ms);

	irq_unlock(key);
	return timed_out;
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
		(NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) != 0u;
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
static volatile bool g_send_reset;           /* set from the ready callback */
static volatile bool g_send_led_capability;  /* set on a channel-16 CC91 query */

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

/* LED Feedback Protocol v1 capability response: channel 16 (LED_MIDI_CHANNEL),
 * CC91, value = LED_PROTOCOL_VERSION. Deliberately bypasses midi_cc(), which
 * hardcodes ST_MIDI_CHANNEL (channel 1) — this response must never appear on
 * the surface-control channel. */
static void led_send_capability(void)
{
	if (!g_midi_ready)
		return;
	struct midi_ump ump = UMP_MIDI1_CHANNEL_VOICE(
		ST_MIDI_GROUP, UMP_MIDI_CONTROL_CHANGE, LED_MIDI_CHANNEL,
		LED_CC_CAPABILITY, LED_PROTOCOL_VERSION);
	(void)usbd_midi_send(midi_dev, ump);
}

/* Host->device packets: M0's surface has no host-settable state, so channel-1
 * (and every channel except 16) traffic is still ignored exactly as before.
 * Channel 16 (LED_MIDI_CHANNEL) is interpreted ONLY through the pure
 * led_midi_decode() dispatcher (led_midi.c), which never calls into surface
 * decode/output — a channel-1 message can never reach the LED protocol, and
 * LED protocol traffic can never emit a surface control event, both by
 * construction, not by a runtime check alone. */
static void on_midi_packet(const struct device *dev, const struct midi_ump ump)
{
	ARG_UNUSED(dev);

	if (UMP_MT(ump) != UMP_MT_MIDI1_CHANNEL_VOICE ||
	    UMP_MIDI_COMMAND(ump) != UMP_MIDI_CONTROL_CHANGE)
		return;

	led_midi_decoded_t d = led_midi_decode(
		UMP_MIDI_CHANNEL(ump), UMP_MIDI1_P1(ump), UMP_MIDI1_P2(ump));
	uint32_t now = (uint32_t)k_uptime_get();

	switch (d.action) {
	case LED_MIDI_ACTION_STAGE:
		led_state_stage(d.index, d.value);
		break;
	case LED_MIDI_ACTION_COMMIT:
		(void)led_state_commit(d.value, now);
		break;
	case LED_MIDI_ACTION_HEARTBEAT:
		(void)led_state_heartbeat(d.value, now);
		break;
	case LED_MIDI_ACTION_RELEASE:
		led_state_release(LED_RELEASE_EXPLICIT, now);
		break;
	case LED_MIDI_ACTION_CAPABILITY_QUERY:
		g_send_led_capability = true;   /* response emitted from main */
		break;
	case LED_MIDI_ACTION_NONE:
	default:
		break;   /* invalid channel or unrelated CC: ignored, no side effect */
	}
}

static void on_midi_ready(const struct device *dev, const bool ready)
{
	ARG_UNUSED(dev);
	g_midi_ready = ready;
	uint32_t now = (uint32_t)k_uptime_get();

	if (ready) {
		g_send_reset = true;   /* CC123 + resend, emitted from main */
		/* Fresh MIDI connection: "the first commit after ... MIDI
		 * connection ... is accepted only after all eight indices
		 * have been staged" — REINIT bumps no diagnostic counter,
		 * this is not host misbehavior. */
		led_state_release(LED_RELEASE_REINIT, now);
	} else {
		/* Disconnect / interface deselected. USB suspend is covered
		 * by the 1000 ms lease timeout below (no heartbeats arrive
		 * while suspended) rather than a dedicated suspend callback:
		 * sample_usbd_init_device(NULL) registers no device-level
		 * message callback in this build, so there is no confirmed
		 * hook to add one without guessing at API surface. */
		led_state_release(LED_RELEASE_DISCONNECT, now);
	}
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
		pat_show();
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
 * [looper a8dd127:4386-4396]: Track1+Track4 held ~1.2 s -> UF2 bootloader.
 *
 * The prior GPIO-latch LED implementation drove the track row instantly and
 * electrically held that state through the reset. Hardware PWM instead
 * needs at least one full LED_PWM_PERIOD_US cycle to actually manifest a new
 * duty cycle at the pin, so a short feed-and-wait is inserted between
 * rendering the pattern and resetting — strictly more visible than the
 * previous implementation's instantaneous, unguaranteed pre-reset flash, not
 * less. Precedence: this is the top safety priority and always runs to
 * completion before the reset it triggers; nothing else in this firmware
 * runs after it starts. */
static void enter_dfu(void)
{
	all_off();
	track_all_on();
	pat_show();
	feed_wdt();
	k_msleep(2);   /* >> one 1024 us PWM period: guarantees the flash is visible */
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
		pat_show();
		feed_wdt();
		k_msleep(90);
		track_all_off();
		pat_show();
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
	led_frame_reset(&g_led_state);   /* cold-boot: no host frame, no leftover diagnostics */
	all_off();
	track_all_off();
	(void)led_render_init();         /* PWM2/PWM3; no-ops entirely if any channel isn't ready */

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

	/* LED Feedback Protocol v1 diagnostics: printed only when the tracked
	 * fields actually change (see the LED diagnostics block below), so a
	 * quiescent host frame never spams the console. */
	int64_t led_diag_t = 0;
	bool led_diag_valid = false;
	bool led_diag_owned = false;
	uint8_t led_diag_seq = 0;
	uint8_t led_diag_active[LED_PHYSICAL_COUNT] = { 0 };
	uint16_t led_diag_staged_mask = 0;
	uint32_t led_diag_valid_commits = 0, led_diag_rejected_commits = 0;
	uint32_t led_diag_duplicate_commits = 0, led_diag_lease_timeouts = 0;
	uint32_t led_diag_explicit_releases = 0, led_diag_disconnect_releases = 0;

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
		if (g_send_led_capability) {
			g_send_led_capability = false;
			led_send_capability();   /* must not alter LED/control state */
		}

		/* ---- FUNCTION (independent GPIO) + hold-to-power-off ---- */
		bool fn_now = pwr_pressed();
		bool fn_was = (stable & BTN_FUNCTION) != 0u;
		bool countdown_active = false;
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
				countdown_active = true;
			}
		}

		/* ---- LED render: safety precedence over any host frame ----
		 * DFU escape, fatal/reset, shutdown and boot signature are all
		 * blocking calls that render g_pattern_frame directly and
		 * never reach this point concurrently with a host frame (see
		 * the LED section's precedence comment above). The
		 * power-off countdown above is the one case that runs
		 * interleaved with the main loop, so it is checked explicitly
		 * here, ahead of any host frame. */
		(void)led_state_check_timeout((uint32_t)k_uptime_get());
		{
			uint8_t led_snapshot[LED_PHYSICAL_COUNT];
			bool led_owned;

			led_snapshot_active(led_snapshot, &led_owned);
			if (led_render_select(countdown_active, led_owned) == LED_RENDER_SOURCE_HOST)
				led_render_apply(led_snapshot);
			else
				pat_show();   /* safety pattern, or idle fallback (pattern is off) */
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
				printk("led_physical=%u led_protocol=%u\n",
				       (unsigned)LED_PHYSICAL_COUNT, (unsigned)LED_PROTOCOL_VERSION);
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

			/* LED Feedback Protocol v1 state: its own change-triggered,
			 * rate-limited stream, independent of the AIN stream above,
			 * so a burst of legitimate LED commits never suppresses (or
			 * is suppressed by) surface diagnostics. */
			bool owned_now;
			uint8_t active_now[LED_PHYSICAL_COUNT];
			uint8_t seq_now;
			uint16_t staged_mask_now;
			uint32_t valid_now, rejected_now, duplicate_now;
			uint32_t timeouts_now, explicit_now, disconnect_now;
			uint32_t lease_deadline_now;
			unsigned int led_key = irq_lock();

			owned_now = g_led_state.owned;
			memcpy(active_now, g_led_state.active, LED_PHYSICAL_COUNT);
			seq_now = g_led_state.last_seq;
			staged_mask_now = g_led_state.staged_mask;
			valid_now = g_led_state.valid_commits;
			rejected_now = g_led_state.rejected_commits;
			duplicate_now = g_led_state.duplicate_commits;
			timeouts_now = g_led_state.lease_timeouts;
			explicit_now = g_led_state.explicit_releases;
			disconnect_now = g_led_state.disconnect_releases;
			lease_deadline_now = g_led_state.lease_deadline_ms;
			irq_unlock(led_key);

			bool led_changed = !led_diag_valid ||
				owned_now != led_diag_owned ||
				seq_now != led_diag_seq ||
				staged_mask_now != led_diag_staged_mask ||
				memcmp(active_now, led_diag_active, LED_PHYSICAL_COUNT) != 0 ||
				valid_now != led_diag_valid_commits ||
				rejected_now != led_diag_rejected_commits ||
				duplicate_now != led_diag_duplicate_commits ||
				timeouts_now != led_diag_lease_timeouts ||
				explicit_now != led_diag_explicit_releases ||
				disconnect_now != led_diag_disconnect_releases;

			if (led_changed && now - led_diag_t >= DIAG_MIN_GAP_MS) {
				int32_t lease_age_ms = owned_now
					? (int32_t)((uint32_t)now -
						    (lease_deadline_now - LED_LEASE_TIMEOUT_MS))
					: -1;

				printk("led: owned=%d seq=%u staged_mask=0x%02x "
				       "active=%u,%u,%u,%u,%u,%u,%u,%u "
				       "lease_age_ms=%d valid=%u rejected=%u dup=%u "
				       "timeouts=%u explicit_rel=%u disc_rel=%u\n",
				       (int)owned_now, (unsigned)seq_now,
				       (unsigned)staged_mask_now,
				       active_now[0], active_now[1], active_now[2], active_now[3],
				       active_now[4], active_now[5], active_now[6], active_now[7],
				       (int)lease_age_ms,
				       (unsigned)valid_now, (unsigned)rejected_now,
				       (unsigned)duplicate_now, (unsigned)timeouts_now,
				       (unsigned)explicit_now, (unsigned)disconnect_now);

				led_diag_t = now;
				led_diag_valid = true;
				led_diag_owned = owned_now;
				led_diag_seq = seq_now;
				led_diag_staged_mask = staged_mask_now;
				memcpy(led_diag_active, active_now, LED_PHYSICAL_COUNT);
				led_diag_valid_commits = valid_now;
				led_diag_rejected_commits = rejected_now;
				led_diag_duplicate_commits = duplicate_now;
				led_diag_lease_timeouts = timeouts_now;
				led_diag_explicit_releases = explicit_now;
				led_diag_disconnect_releases = disconnect_now;
			}
		} else {
			banner_done = false;
			led_diag_valid = false;
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
