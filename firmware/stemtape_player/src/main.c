/*
 * ============================================================================
 *  STEM TAPE PLAYER  —  derived from the SP-1 Tape Looper (firmware/src/main.c,
 *  commit fd98a037493b2899704cc18847323addc8ba7cb4, byte-for-byte at the point
 *  of copy) — NOT a clean-room reimplementation.
 * ============================================================================
 *  PROVENANCE: this file starts as a literal copy of the proven, working
 *  Tape Looper firmware (see firmware/src/main.c, untouched, the regression
 *  baseline). Everything below this banner is inherited real, hardware-
 *  verified code (eMMC storage, xfer_service()/SP1XFER!, codec/I2S bring-up,
 *  the ADC ladder control scanner, watchdog/charger/DFU, and the GPIO+soft-
 *  PWM LED driver) unless a change is marked "STEM TAPE:" in a comment at the
 *  change site. Search this file for "STEM TAPE:" to find every deviation
 *  from the original. The two firmwares are separate Zephyr app targets
 *  (firmware/ vs firmware/stemtape_player/) built from separate binaries —
 *  changing this copy never touches firmware/src/main.c or its CI baseline.
 *
 *  STEM TAPE CONCEPT: one song = four synchronized stems (Vocal/Drums/Bass/
 *  Instrument), validated as a matched set at upload time, instead of four
 *  independently-looping tracks with no cross-track relationship. Concretely
 *  this reuses, UNCHANGED: the NUM_SLOTS=16/NTRK=4 slot×track addressing
 *  (trk_blk()), the two-copy meta_blk commit protocol (meta_write_blocks()),
 *  the classic P/R/W/F/X xfer_service() block-transfer verbs, the on-flash/
 *  ring sample representation (mono 16-bit PCM, SAMP_PER_BLK/codec_pack/
 *  codec_unpack — a stereo/24-bit storage format is a follow-up milestone,
 *  NOT part of this change), and the single-shared-transport mixing
 *  structure in looper_audio_block() (all 4 tracks already play from one
 *  shared playhead — see PASS A/B). It changes: the meta format (new magic +
 *  per-stem CRC32/frame-count/BPM/downbeat fields, additive to struct
 *  meta_blk), and adds one new xfer verb ('Z', stem-song commit) that
 *  validates all four stems (presence, matching frame count, per-stem
 *  CRC32) before it is ever possible to mark them present[]=1 and playable
 *  — see st_stem_validate.c.
 *
 * ---- ORIGINAL TAPE LOOPER HEADER BELOW (unmodified) ----
 * ============================================================================
 *  SP-1 LOOPER  —  custom firmware for the Teenage Engineering SP-1
 * ============================================================================
 *  A four-track, hold-to-record audio looper / sketchpad. Audio comes in over
 *  USB-C (the SP-1 appears as a USB sound card); you record loops by holding
 *  the track buttons, and they play back layered together out of the speaker
 *  or headphones. Loops are stored on the SP-1's internal 4 GB flash, so they
 *  survive power-off and even re-flashing the firmware.
 *
 *  ---- HOW THE AUDIO FLOWS ----
 *    USB-C in  ->  [USB ring]  ->  audio engine  ->  I2S bus  ->  speaker / HP
 *                                       |  ^
 *                              record  v  |  play
 *                                  [ eMMC flash, 1 region per track ]
 *
 *  ---- THE THREADS (highest audio priority first) ----
 *    audio_thread   : runs every I2S block (256 frames). Mixes the 4 playback
 *                     tracks + the live USB monitor, and decimates the live
 *                     input down into the track being recorded. Never blocked.
 *    streamer_thread: the only thing that touches the flash. Flushes the
 *                     track being recorded TO flash, and reads the playing
 *                     tracks back FROM flash into their ring buffers ahead of
 *                     the playhead. (sp1_emmc.c is the flash driver.)
 *    midi_thread    : (optional) MIDI clock housekeeping.
 *    main           : ~8 ms control loop — buttons, faders, LEDs, power, the
 *                     USB-serial status line (controls_diag).
 *
 *  ---- KEY DESIGN POINTS ----
 *    * Clocking: the board's 3.072 MHz oscillator drives the I2S bit clock and
 *      the CS42L42 headphone codec masters a true 48 kHz frame; the nRF and the
 *      speaker amp are clock slaves (see the "I2S audio bus" section).
 *    * Loops play at full 48 kHz; recording is mono and decimated by DECIM (see
 *      the LOOPER ENGINE section) — the flash write speed sets that ceiling.
 *    * Storage uses the nRF's SPIM3 SPI engine at 32 MHz (a calculated overclock
 *      above the 26 MHz default-speed max) with hardware CRC checking + retry,
 *      so the flash bus is fast and self-correcting. The card's internal write
 *      cache is enabled to absorb record bursts; it's flushed only at power-off.
 *
 *  ---- BOOTLOADER SAFETY (the SP-1 "BIG FIVE") ----
 *    app lives at 0x20000; watchdog fed < 5 s; we do NOT re-init bootloader-
 *    owned clocks/peripherals; SYSTEM_OFF returns to the bootloader; RESETREAS
 *    is cleared on boot and before SYSTEM_OFF. (There is no hardware reset pin
 *    on the SP-1, so a clean path back to the bootloader is mandatory.)
 *
 *  See README.md in this folder for the player's controls and a fuller tour.
 * ============================================================================
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_midi2.h>
#include <zephyr/audio/midi.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/atomic.h>
#include <sample_usbd.h>
#include "st_ab_session.h"
#include "st_bulk_xfer.h"
#include "st_crc32.h"
#include "st_midi_queue.h"
#include "st_sector_v11.h"
#include "st_stcp.h"
#include "st_beat_phase.h"
#include "st_stem_bufmbox.h"
#include "st_planar.h"
#include "st_stem_mix.h"
#include "st_stem_stream.h"
#include "st_led_mvp.h"

/*
 * BUILD IDENTITY -- printed in the boot banner AND on every LOOPER
 * diagnostic line as b=<tag>.
 *
 * This exists because of a real, expensive failure: a serial capture was
 * analysed at length as evidence about a fix, when the device was in fact
 * still running a firmware from before that fix. Nothing in the output said
 * so. The only way to tell was to notice which diagnostic lines had
 * appeared or disappeared between builds -- which is not a check anyone
 * should have to perform, and is exactly the check that was missed.
 *
 * So: bump this string in the SAME commit as any change whose effect is to
 * be judged from a capture. If a capture's b= does not match the build that
 * was handed over, the flash did not take and the capture says nothing
 * about the change -- stop and re-flash before reading another number out
 * of it.
 */
/* The temporary st16-cal ladder-capture build is GONE, switch and all. Its
 * one job was measuring the real Track/PLAY ladder on physical hardware; the
 * measurement is committed in docs/ladder-measured.json and src/st_ladder.c
 * decodes against it, so the capture has nothing left to do. A CI gate keeps
 * it from coming back. */
/* AIN1 calibration capture. OFF in every shipped image; a CI gate keeps it
 * that way, exactly as it does for the AIN0 capture. */
#ifndef ST_VOL_CAL
#define ST_VOL_CAL 0
#endif
/* THE BUILD TAG IS PART OF THE CALIBRATION IMAGE'S IDENTITY, deliberately.
 * st19 printed the same "STEMTAPE BUILD st19" banner from both the shipped
 * image and the calibration image, so seeing the banner proved the console
 * worked but proved NOTHING about which of the two binaries was running --
 * and "flashed the shipped image by mistake" is indistinguishable from "the
 * capture is broken" when the capture prints nothing. The tag now carries
 * the distinction, so the banner alone settles it. */
/* st21: FX entry works. The AIN1 volume ladder is measured (st_vol_ladder.h,
 * docs/ain1-measured.json) and the two-volume chord decodes as VOL_BOTH.
 *
 * BUMPED BECAUSE THE OPERATOR HAS TO BE ABLE TO TELL. An st20 shipped image
 * has already been flashed to the hardware and could not open FX mode. This
 * one can. If both announced "st20", "the gesture does nothing" and "the old
 * image is still on the device" would be indistinguishable -- the same
 * failure that cost several rounds before the shipped/calibration tags were
 * split apart. The tag is the only thing the operator can read. */
/* Reads per size in the 'M' read-cost sweep. Enough to average out ordinary
 * card jitter, few enough that the whole sweep stays well inside the transfer
 * session's patience. */
#define ST_RC_SWEEP_REPS 24u

#if ST_VOL_CAL
#define ST_BUILD_TAG "st55-VOLCAL"
#else
#define ST_BUILD_TAG "st55"
#endif
#include "st_track_hold.h"

/*
 * THE FOUR THREAD PRIORITIES, IN ONE PLACE, because the last time they were
 * spread across three k_thread_create() call sites one of them went stale
 * against a comment and cost the streamer half its CPU (see main()'s own note
 * where ST_PRIO_MAIN is applied). Lower number = higher priority.
 *
 *   AUDIO     outranks everything. It has a hard 5.333 ms deadline.
 *   STREAMER  level with control work, and yields to it once per read.
 *   MAIN      control loop, LEDs, watchdog feed. Self-throttled at 8 ms.
 *   MIDI      below all audio-critical work, as specified.
 *
 * Reported live on the CPU= diagnostic line as prio=, so a capture always
 * says which scheduling it was taken under.
 */
#define ST_PRIO_AUDIO     0
#define ST_PRIO_STREAMER  1
#define ST_PRIO_MAIN      1
#define ST_PRIO_MIDI      6
_Static_assert(ST_PRIO_AUDIO < ST_PRIO_STREAMER && ST_PRIO_AUDIO < ST_PRIO_MAIN,
		"audio must outrank the streamer and the control loop");
_Static_assert(ST_PRIO_MIDI > ST_PRIO_AUDIO && ST_PRIO_MIDI > ST_PRIO_STREAMER,
		"MIDI must sit below all audio-critical work");
_Static_assert(ST_PRIO_STREAMER <= ST_PRIO_MAIN,
		"the streamer must not sit below non-time-critical control work");

/* The diagnostic print interval in force right now, in ms -- the denominator
 * the CPU line's diag= percentage is taken over. Set by main(). */
static volatile uint32_t g_diag_window_ms = 500u;
#include "st_ladder.h"
#include "st_vol_ladder.h"
#include "st_latency.h"
#include "st_seam.h"
#include "st_pitch.h"
#include "st_fnplay.h"
#include "st_readcost.h"
#include "st_stem_meter.h"
#include "st_inertia.h"
#include "st_resample.h"
#include "st_fx.h"
#include "st_fx_ctl.h"
#include "st_ctl.h"
#include "st_loop.h"
#include "st_stix.h"
#include "st_v11_format.h"

/* STEM TAPE: UAC2 (USB audio class) playback is REMOVED as of this change --
 * product decision: it was added earlier under the mistaken assumption that
 * KO II cue control depended on it. Cue control actually uses incoming USB
 * MIDI only (st_midi_queue.h, below), and song loading uses the companion's
 * own transfer protocol over the CDC console (xfer_service(), unchanged).
 * Stored four-stem playback now exclusively owns the SP-1 audio output.
 * Search this file for "STEM TAPE: UAC2" to find every removal site;
 * removed entirely -- runtime callbacks, buffers, memory slab, descriptor
 * and Kconfig -- not merely disabled (see prj.conf, CMakeLists.txt, and
 * the CI symbol-presence/USB-descriptor gates, which now prove absence
 * instead of presence). USB MIDI receive and the CDC block-transfer
 * connection are both unaffected. */
#include <soc.h>
#include <math.h>
#include <string.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#include "sp1_emmc.h"
/* STEM TAPE PHASE 1: st_crc32.h/st_stem_validate.h (the 'Z'-verb validated-
 * commit gate) are not included -- this phase has no write path for them to
 * gate. They return in Phase 2 (see CMakeLists.txt). */

/* FAILSAFE: turn ANY unrecoverable fault (bad pointer, stack overflow, kernel
 * panic, failed assert) into a clean reboot instead of a dead hang, so the
 * device can never get stuck in a bricked-looking state.
 * CRASH FORENSICS: this silent reboot is also why crashes left no trail —
 * stash the fault reason + faulting PC in __noinit RAM (survives the soft
 * reboot); the next boot prints them in the diag line as flt=reason@pc. */
static __noinit uint32_t g_fault_key;            /* 0xFA17FA17 = breadcrumb valid */
static __noinit uint32_t g_fault_reason;
static __noinit uint32_t g_fault_pc;
static uint32_t g_resetreas;                     /* NRF_POWER->RESETREAS at boot */
static uint32_t g_last_fault_reason = 0xFFFFFFFFu; /* from the PREVIOUS boot (diag) */
static uint32_t g_last_fault_pc;
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	g_fault_reason = reason;
	g_fault_pc = esf ? esf->basic.pc : 0u;
	g_fault_key = 0xFA17FA17u;
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

#define WDT_NODE DT_ALIAS(watchdog0)

/* ---- the 4 playback LEDs (center row, verified pin map) ---- */
struct led { NRF_GPIO_Type *port; uint32_t pin; };
static const struct led leds[] = {
	{ NRF_P1, 13 }, { NRF_P0, 0 }, { NRF_P1, 12 }, { NRF_P0, 1 },
};
#define NUM_LEDS (sizeof(leds) / sizeof(leds[0]))

/* ---- the 4 TRACK LEDs (directly above buttons 1-4) ---- */
static const struct led track_leds[] = {
	{ NRF_P0, 29 }, { NRF_P0, 26 }, { NRF_P1, 15 }, { NRF_P1, 14 },
};
#define NUM_TRACK_LEDS (sizeof(track_leds) / sizeof(track_leds[0]))

/* 1 = dim LEDs (soft-PWM render), 0 = full brightness. Toggled by the
 * FUNCTION+PLAY double-tap; persisted in the song index tail (led_full).
 * Declared here (not with the dimmer) because xfer_commit persists it. */
static volatile uint8_t g_led_dim = 1;

static void track_led_on(int i);
static void track_all_off(void);
static bool usb_present(void);
static bool charging(void);

/* ---- power / function button: P0.27, active-low with pull-up ---- */
#define PWR_PORT        NRF_P0
#define PWR_PIN         27u

/* ---- BQ24232 battery charger control (verified pins from TimK pinout) ---- */
#define BQ_PORT         NRF_P0
#define BQ_NCE_PIN      21u   /* charge enable, ACTIVE-LOW: drive low = charging on */
#define BQ_NCHG_PIN     22u   /* charge status, open-drain, LOW = charging now      */
#define BQ_NPGOOD_PIN   24u   /* power good,    open-drain, LOW = USB power present  */

/* hold this long (ms) to power off - "a few seconds" like the real device */
#define HOLD_MS_TO_OFF  2500

/* ---- button ladders (Milestone 1: read + report the controls) ----
 * The PLAY/track and Vol/FWD/RWD buttons are resistor ladders read on the
 * SAADC. They are only powered when BTN_COM (P1.10) is driven high, so we
 * raise that rail before sampling. Raw 12-bit codes are streamed over the
 * USB serial console so we can map each button press to a voltage band. */
#define BTN_COM_PORT    NRF_P1
#define BTN_COM_PIN     10u

static const struct adc_dt_spec adc_ladder[] = {
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0),  /* AIN0: PLAY + tracks   */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1),  /* AIN1: Vol + FWD/RWD   */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 2),  /* AIN3: Fader 1 (track1 vol) */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 3),  /* AIN6: Fader 2 (track2 vol) */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 4),  /* AIN2: Fader 3 (track3 vol) */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 5),  /* AIN7: Fader 4 (track4 vol) */
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 6),  /* AIN4: battery level (divider) */
};
#define LAD_TRACKS 0
#define LAD_VOL    1
#define LAD_FADER0 2     /* faders are ladder indices 2..5 */
#define LAD_BATT   6     /* battery voltage via on-board divider (AIN4) */
#define NUM_LADDERS (sizeof(adc_ladder) / sizeof(adc_ladder[0]))

/* the USB CDC ACM serial console (chosen,console in the devicetree) */
static const struct device *const cdc =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static int16_t adc_sample;

/* ---- audio codecs over I2C (Milestone 2a: just confirm they answer) ----
 *   CS42L42 headphone codec @ 0x48   (reset: P0.15, active-low)
 *   TAS2505 speaker amp     @ 0x18   (reset: P0.09 / NFC1, active-low)
 * We release both resets, then scan the bus and report what ACKs.
 * (Verified on hardware 2026-06-05: both ACK; CS42L42 straps to 0x48.) */
#define CS42_RST_PORT   NRF_P0
#define CS42_RST_PIN    15u
#define TAS_RST_PORT    NRF_P0
#define TAS_RST_PIN     9u
#define CS42L42_ADDR    0x48u
#define TAS2505_ADDR    0x18u

static const struct device *const i2c_bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));

static uint8_t i2c_found[16];
static int     i2c_found_n;
static bool    cs42_ok, tas_ok;
static bool    i2c_scanned;

/* Oversampled ladder read: average 2 conversions. Audio/USB activity couples
 * noise into the shared BTN_COM rail, so a single 12-bit sample can land a band
 * boundary off; averaging quietens every ladder, and the sticky debounce does
 * the rest. CAREFUL with the count: blocking ADC reads run on the main thread,
 * which PREEMPTS the eMMC streamer — at 4x across 6 ladders the stolen CPU
 * slowed the bit-banged card below the ~26.6 blk/s a take produces and brought
 * back record-ring overflows (corrupt loops). 2x + round-robin faders keeps the
 * main loop's ADC cost at the level the working builds had.
 * Returns -1 on ADC error (callers treat <0 as "no change / hold last"). */
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

/* Power the ladder rail, set up the ADC channels, bring USB up. Safe to call
 * once at boot; never blocks waiting for a host. */
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

	/* USB is brought up later in main() on the device_next stack (CDC
	 * console; Phase 1 has no UAC2 audio class); nothing to enable here. */
}

/* Drive one bare-metal GPIO high (used to release the codec reset lines). */
static void gpio_drive_high(NRF_GPIO_Type *port, uint32_t pin)
{
	port->OUTSET = (1u << pin);
	port->PIN_CNF[pin] =
		(GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)   |
		(GPIO_PIN_CNF_DRIVE_S0S1    << GPIO_PIN_CNF_DRIVE_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
	port->OUTSET = (1u << pin);
}

static void gpio_drive_low(NRF_GPIO_Type *port, uint32_t pin)
{
	port->OUTCLR = (1u << pin);
}

/* Release the codec resets, then probe every I2C address once and record
 * which devices answer. Reading a single byte is a harmless presence test. */
static void codec_init(void)
{
	gpio_drive_high(CS42_RST_PORT, CS42_RST_PIN);   /* un-reset CS42L42 */
	gpio_drive_high(TAS_RST_PORT,  TAS_RST_PIN);    /* un-reset TAS2505 */
	k_msleep(20);

	if (!device_is_ready(i2c_bus))
		return;

	i2c_found_n = 0;
	cs42_ok = tas_ok = false;
	for (uint8_t a = 0x08; a <= 0x77; a++) {
		uint8_t b;
		if (i2c_read(i2c_bus, &b, 1, a) == 0) {
			if (i2c_found_n < (int)sizeof(i2c_found))
				i2c_found[i2c_found_n++] = a;
			if (a == CS42L42_ADDR) cs42_ok = true;
			if (a == TAS2505_ADDR) tas_ok = true;
		}
	}
	i2c_scanned = true;
}

/* ========================= I2S audio bus =================================
 * CLOCK TOPOLOGY (the way Teenage Engineering wired the board — see the
 * SP-1-dev wiki): the on-board 3.072 MHz oscillator (enabled via OSC_EN
 * P0.13) drives the shared I2S bit clock, and the CS42L42 headphone codec is
 * the FRAME master — it divides that oscillator by 64 to make a LRCK of
 * exactly 3.072 MHz / 64 = 48000 Hz. The nRF52840 I2S peripheral and the
 * TAS2505 speaker amp are both CLOCK SLAVES on this bus.
 *
 * (Pins: SCLK P0.12, LRCK P0.11, DOUT P1.09.)
 *
 * An earlier design had the nRF master the clocks at ~47619 Hz instead — it
 * crackled on the speaker and produced only noise on the headphones, because
 * the CS42L42 was never given the clock it was built to run from. Driving the
 * board the way TE intended fixed both, so everything below assumes a true,
 * codec-mastered 48.000 kHz. */
#define OSC_EN_PORT     NRF_P0
#define OSC_EN_PIN      13u

#define I2S_SR          48000
#define I2S_TRUE_HZ     48000u   /* real LRCK = osc / 64, CS42L42 is frame master */
#define BLK_FRAMES      256
#define BLK_BYTES       (BLK_FRAMES * 2 * (int)sizeof(int16_t))   /* stereo 16-bit slots */

K_MEM_SLAB_DEFINE(tx_slab, BLK_BYTES, 10, 4);   /* 10 blks ~106ms DMA cushion — the PROVEN WORKING.bin
                                                 * value. (A codec-era trim to 4 was never validated on
                                                 * hardware and rode along in every failed build.) */
static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));

static int  audio_cfg_rc = 1;        /* i2s_configure() result, for serial diag */
static bool tas_cfg_ok;              /* did the TAS2505 register writes all ACK?  */
static volatile bool audio_started;  /* did i2s START trigger fire?               */

/* ---- TAS2505 speaker-amp setup (ported from TimK SP-1-dev, 16-bit I2S) ---- */
static int tas_wr(uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte(i2c_bus, TAS2505_ADDR, reg, val);
}
static void tas_page(uint8_t p) { (void)tas_wr(0x00, p); }

/* Power the speaker amp on/off (page-1 reg 0x2D: 0x02 = driver up, 0x00 = off).
 * Used by the headphone auto-mute. Main-thread only (audio thread never touches
 * I2C after init), so no locking needed. */
static void tas_set_speaker(int on)
{
	tas_page(0x01);
	(void)tas_wr(0x2D, on ? 0x02 : 0x00);
	tas_page(0x00);
}

/* TAS2505 speaker bring-up, following TI Application Reference Guide SLAU472C
 * Section 5.1 ("Play Digital Data Through DAC and Headphone/Speaker Outputs").
 *
 * CLOCKING: the speaker DAC is clocked from a PLL locked to the I2S bit clock
 * (BCLK = the 3.072 MHz oscillator). The PLL multiplies BCLK so the DAC's
 * internal rates land where the sigma-delta modulator wants them:
 *   f_PLL  = BCLK x J = 3.072 MHz x 32 = 98.304 MHz
 *   DAC_FS = f_PLL / (NDAC2 x MDAC8 x DOSR128) = 48000 Hz  (exact)
 * Locking to BCLK (not a free-running MCLK) means the DAC tracks the bus
 * exactly, so the speaker never slips or crackles. BCLK must be running before
 * the PLL can lock, so the I2S stream is started before this runs. */
static bool tas2505_configure(void)
{
	int rc = 0;

	tas_page(0x00);
	rc |= tas_wr(0x01, 0x01);          /* software reset */
	k_msleep(5);

	/* Page 1: LDO output 1.8 V, analog level shifters powered up */
	tas_page(0x01);
	rc |= tas_wr(0x02, 0x00);

	/* Page 0: clocking (PLL locked to BCLK) + interface */
	tas_page(0x00);
	rc |= tas_wr(0x04, 0x07);          /* PLL_CLKIN = BCLK, CODEC_CLKIN = PLL */
	rc |= tas_wr(0x05, 0x91);          /* PLL powered, P=1, R=1 */
	/* TE-native bus: BCLK = the 3.072 MHz oscillator, WCLK = 48000 (64 SCLK per
	 * frame, CS42L42 frame master). PLL J=32 puts f_PLL = 3.072M x 32 = 98.304
	 * MHz (inside the ~80-110 MHz lock range); NDAC=2 x MDAC=8 x DOSR=128 = 2048
	 * brings DAC_FS = 98.304M/2048 = 48000 = WCLK exactly. */
	rc |= tas_wr(0x06, 0x20);          /* PLL J = 32  -> f_PLL = 98.304 MHz */
	rc |= tas_wr(0x07, 0x00);          /* PLL D = 0 (MSB) */
	rc |= tas_wr(0x08, 0x00);          /* PLL D = 0 (LSB) */
	k_msleep(15);                      /* wait for PLL to lock */
	rc |= tas_wr(0x0B, 0x82);          /* NDAC = 2, powered */
	rc |= tas_wr(0x0C, 0x88);          /* MDAC = 8, powered */
	rc |= tas_wr(0x0D, 0x00);          /* DOSR MSB */
	rc |= tas_wr(0x0E, 0x80);          /* DOSR = 128 -> DAC_FS = 48000 */
	rc |= tas_wr(0x1B, 0x00);          /* I2S, 16-bit, slave (matches nRF Philips I2S) */
	rc |= tas_wr(0x1C, 0x00);          /* data slot offset 0 */
	rc |= tas_wr(0x3C, 0x02);          /* DAC processing block PRB_P2 (mono) */

	/* DAC power + digital volume — these live on PAGE 0 */
	rc |= tas_wr(0x3F, 0x90);          /* DAC powered, left data -> left, soft-step */
	rc |= tas_wr(0x41, 0x00);          /* DAC digital gain 0 dB */
	rc |= tas_wr(0x40, 0x04);          /* DAC not muted */

	/* Page 1: analog reference, routing, speaker driver */
	tas_page(0x01);
	rc |= tas_wr(0x01, 0x10);          /* master analog reference powered ON */
	rc |= tas_wr(0x0A, 0x00);          /* output common mode 0.9 V */
	rc |= tas_wr(0x0C, 0x04);          /* Mixer P out -> output mixer (DAC routed) */
	rc |= tas_wr(0x16, 0x00);          /* HP volume 0 dB */
	rc |= tas_wr(0x18, 0x00);          /* AINL volume / mixer */
	rc |= tas_wr(0x09, 0x20);          /* power up HP driver */
	rc |= tas_wr(0x10, 0x00);          /* unmute HP, 0 dB */
	rc |= tas_wr(0x2E, 0x00);          /* speaker attenuation 0 dB (max) */
	/* Class-D driver gain, P1/R48 bits D6-D4: 000=mute 001=6dB 010=12dB
	 * 011=18dB 100=24dB. Was 6 dB — user wants a fair bit louder; 18 dB is
	 * one step below the chip's max (24 dB = 0x40 if ever needed). */
	rc |= tas_wr(0x30, 0x30);          /* speaker driver gain +18 dB */
	rc |= tas_wr(0x2D, 0x02);          /* speaker driver powered up */

	tas_page(0x00);
	k_msleep(10);

	tas_cfg_ok = (rc == 0);
	return tas_cfg_ok;
}

/* ---- HEADPHONE output (always on), SELF-SELECTING driver ---------------------
 * Probes the codec and picks the right register scheme at boot:
 *   PATH 1 (expected): a real CS42L42/CS42L83 — our chip ACKs 0x48, the genuine
 *     CS42L42 address. Full 16-bit paged init taken VERBATIM from the Linux
 *     kernel driver (sound/soc/codecs/cs42l42.c): PLL from SCLK using the
 *     1.536 MHz pll_ratio_table row {prediv 0, div_int 0x7D, frac 0, mode 3,
 *     divout 0x10 x n 2, cal 125, mclk_int 12 MHz}. Our SCLK is 1.5238 MHz
 *     (-0.8%), so every derived clock scales with the bus = self-consistent.
 *     CRITICALLY this path needs NO MCLK: the 3.072 MHz oscillator stays OFF —
 *     turning it on permanently was what made the speaker crackle (the comment
 *     in audio_init predicted exactly that).
 *   PATH 2 (fallback): TimK's 8-bit-register variant (SP-1-dev repo, forum-
 *     confirmed on his unit at 0x4A). Only this path powers the oscillator,
 *     since his CLK_CTL 0x04 is MCLK auto-detect.
 * No jack detect — headphones simply run alongside the speaker. */
static volatile int g_hp_on;     /* diag: 0=none, 1=CS42L42 16-bit, 2=TimK 8-bit */
static uint8_t g_cs42_addr = CS42L42_ADDR;
static uint8_t g_cs42_id8;       /* diag: 8-bit-scheme reg 0x01 readback */
static uint8_t g_cs42_dev[3];    /* diag: 16-bit DEVID A/B, C/D, E (0x42 0xA4 x = CS42L42) */
static uint8_t g_hp_pll;         /* diag: PLL lock status readback */
static volatile int g_hp_in = -1;   /* headphones detected in jack: 1 yes, 0 no, -1 unknown */
static bool cs42_wr8(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { reg, val };
	return i2c_write(i2c_bus, b, 2, g_cs42_addr) == 0;
}
static bool cs42_rd8(uint8_t reg, uint8_t *val)
{
	return i2c_write_read(i2c_bus, g_cs42_addr, &reg, 1, val, 1) == 0;
}
static bool cs42_wr16(uint16_t reg, uint8_t val)
{
	uint8_t b[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, val };
	return i2c_write(i2c_bus, b, 3, g_cs42_addr) == 0;
}
static bool cs42_rd16(uint16_t reg, uint8_t *val)
{
	uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
	return i2c_write_read(i2c_bus, g_cs42_addr, a, 2, val, 1) == 0;
}
/* HP_TIM_TEST 1 builds the SEPARATE headphone test binary: the exact init from
 * Tim Knapen's wiki (github.com/timknapen/SP-1-dev/wiki/I2C — proven on real
 * SP-1 hardware, and it uses the page-select protocol we independently
 * confirmed), adapted to OUR clock topology: nRF stays I2S master, the 3.072 MHz
 * oscillator stays OFF (TE's design has the osc drive the shared SCLK line —
 * enabling it against the nRF master is what caused the crackle), PLL row for
 * our 1.524 MHz SCLK, 16-bit channels. Key registers Tim has that we never
 * wrote: 0x1007 (Serial Port SRC routing), 0x2601/0x2609 (SRC rates), 0x240E
 * (EQ input unmute), 0x1121 (headset switch). The main binary keeps 0. */
#ifndef HP_TIM_TEST
#define HP_TIM_TEST 1     /* Tim-wiki headphone init is now the NORMAL build */
#endif
#if HP_TIM_TEST
static bool tpw(uint16_t reg, uint8_t val)   /* paged write: page reg 0x00 first */
{
	uint8_t p[2] = { 0x00, (uint8_t)(reg >> 8) };
	uint8_t b[2] = { (uint8_t)reg, val };
	if (i2c_write(i2c_bus, p, 2, g_cs42_addr) != 0) return false;
	return i2c_write(i2c_bus, b, 2, g_cs42_addr) == 0;
}
static bool tpr(uint16_t reg, uint8_t *val)  /* paged read */
{
	uint8_t p[2] = { 0x00, (uint8_t)(reg >> 8) };
	uint8_t o = (uint8_t)reg;
	if (i2c_write(i2c_bus, p, 2, g_cs42_addr) != 0) return false;
	return i2c_write_read(i2c_bus, g_cs42_addr, &o, 1, val, 1) == 0;
}

/* Headphone presence from the CS42L42: DET_STATUS1 (page 0x1B reg 0x77) bit7,
 * per Tim's wiki "request headphone status". 1=plugged, 0=unplugged, -1=read failed. */
static int hp_detect_connected(void)
{
	uint8_t st;
	if (!tpr(0x1B77, &st)) return -1;
	return (st >> 7) & 1;
}
#endif

static void hp_codec_init(int pllcfg)
{
	static const uint8_t addrs[2] = { 0x48u, 0x4Au };
	(void)pllcfg;                /* unused when the HP graft is compiled out */
	g_hp_on = 0;

#if HP_TIM_TEST
	/* hard reset pulse — without it the codec is wedged and NAKs everything */
	gpio_drive_low(CS42_RST_PORT, CS42_RST_PIN);
	k_msleep(5);
	gpio_drive_high(CS42_RST_PORT, CS42_RST_PIN);
	k_msleep(10);

	g_cs42_addr = 0x48u;
	(void)tpr(0x1001, &g_cs42_dev[0]);            /* DEVID_AB (0x42 = CS42L42) */
	(void)tpr(0x1002, &g_cs42_dev[1]);
	(void)tpr(0x1003, &g_cs42_dev[2]);
	if (g_cs42_dev[0] != 0x42) return;            /* not answering -> leave alone */

	/* ===== TIM'S WIKI SEQUENCE, VERBATIM — native TE topology. =====
	 * The CS42L42 is the I2S frame MASTER here (its designed role on this
	 * board): PLL referenced from the oscillator-driven 3.072 MHz SCLK, LRCK
	 * generated at exactly 48 kHz, the nRF and TAS2505 follow as slaves.
	 * Every value below is from github.com/timknapen/SP-1-dev/wiki/I2C, the
	 * config proven to play headphone audio on this exact hardware. The ONLY
	 * deviation was mixer volume -19 dB; RESTORED to his full-scale 0x00 —
	 * the -19 dB pad capped max headphone loudness ~1/9th of stock. The
	 * digital path already soft-limits before the codec, so 0 dB is safe. */
	(void)tpw(0x1508, 0x10);   /* PLL Control 3                         */
	(void)tpw(0x1504, 0x80);   /* PLL Division Fractional Byte 2        */
	(void)tpw(0x1505, 0x3E);   /* PLL Division Integer                  */
	(void)tpw(0x150A, 0x7D);   /* PLL Calibration Ratio                 */
	(void)tpw(0x1009, 0x00);   /* MCLK Control                          */
	(void)tpw(0x1201, 0x01);   /* MCLK Source Select                    */
	(void)tpw(0x120A, 0x01);   /* Input ASRC Clock Select               */
	(void)tpw(0x120B, 0x01);   /* Output ASRC Clock Select              */
	(void)tpw(0x1501, 0x01);   /* PLL Control 1: start                  */
	(void)tpw(0x1107, 0x01);   /* Oscillator Switch (SCLK is running —
	                              the 3.072 MHz osc drives it)          */
	for (int t = 0; t < 10; t++) {   /* wait for "SCLK selected" (0x02) */
		k_msleep(1);
		if (tpr(0x1109, &g_hp_pll) && g_hp_pll == 0x02) break;
	}
	(void)tpw(0x1007, 0x13);   /* Serial Port SRC Control               */
	(void)tpw(0x1203, 0x1F);   /* FSYNC Pulse Width Lower (64-SCLK frame) */
	(void)tpw(0x1205, 0x3F);   /* FSYNC Period Lower                    */
	(void)tpw(0x1207, 0x34);   /* ASP Clock Config: MASTER              */
	(void)tpw(0x1208, 0x1A);   /* ASP Frame Configuration               */
	(void)tpw(0x2A02, 0x02);   /* Channel 1: 24-bit                     */
	(void)tpw(0x2A05, 0x42);   /* Channel 2: phase + 24-bit             */
	(void)tpw(0x2601, 0x4C);   /* SRC Input Sample Rate                 */
	(void)tpw(0x2609, 0x4C);   /* SRC Output Sample Rate                */
	(void)tpw(0x2A01, 0x0C);   /* ASP Receive Enable                    */
	(void)tpw(0x240E, 0x01);   /* Equalizer Input Mute Control          */
	(void)tpw(0x2301, 0x00);   /* Mixer A vol 0 dB (Tim's full scale)   */
	(void)tpw(0x2303, 0x00);   /* Mixer B vol                           */
	(void)tpw(0x1101, 0x96);   /* power up the codec                    */
	k_msleep(10);              /* HP amp operational after 10 ms        */
	(void)tpw(0x1121, 0x41);   /* Headset switch control                */
	(void)tpw(0x1B74, 0x03);   /* Miscellaneous detect control          */
	(void)tpw(0x1129, 0x01);   /* Headset clamp disable                 */
	(void)tpw(0x2001, 0x0D);   /* HP Control: mute all                  */
	(void)tpw(0x1F06, 0x84);   /* DAC Control 2                         */
	(void)tpw(0x2301, 0x00);   /* Mixer A vol again                     */
	(void)tpw(0x2303, 0x00);   /* Mixer B vol again                     */
	(void)tpw(0x1B73, 0xC2);   /* Tip Sense Control                     */
	(void)tpw(0x1B75, 0x9F);   /* Mic detect control 1                  */
	(void)tpw(0x2001, 0x01);   /* UNMUTE headphones                     */
	g_hp_on = 1;
	return;
#endif

	for (int a = 0; a < 2; a++) {
		g_cs42_addr = addrs[a];

		/* read both ID schemes (8-bit read first: harmless on either chip) */
		uint8_t id8 = 0;
		if (!cs42_rd8(0x01, &id8)) continue;          /* nothing ACKs here */
		g_cs42_id8 = id8;
		(void)cs42_rd16(0x1001, &g_cs42_dev[0]);      /* CS42L42_DEVID_AB */
		(void)cs42_rd16(0x1002, &g_cs42_dev[1]);      /* CS42L42_DEVID_CD */
		(void)cs42_rd16(0x1003, &g_cs42_dev[2]);      /* CS42L42_DEVID_E  */

		if (g_cs42_dev[0] == 0x42) {
			/* ---- PATH 1: genuine CS42L42/L83, kernel-exact init ---- */
			/* clocking: internal-FS = 12 MHz family (mclk_int 12000000) */
			(void)cs42_wr16(0x1009, 0x00);  /* MCLK_CTL: INTERNAL_FS=0     */
			/* PLL dividers — pll_ratio_table row for SCLK 1.536 MHz      */
			(void)cs42_wr16(0x120C, 0x00);  /* PLL_DIV_CFG1: SCLK_PREDIV /1 */
			(void)cs42_wr16(0x1505, 0x7D);  /* PLL_DIV_INT   0x7D (125)    */
			(void)cs42_wr16(0x1502, 0x00);  /* PLL_DIV_FRAC0               */
			(void)cs42_wr16(0x1503, 0x00);  /* PLL_DIV_FRAC1               */
			(void)cs42_wr16(0x1504, 0x00);  /* PLL_DIV_FRAC2               */
			(void)cs42_wr16(0x151B, 0x03);  /* PLL_CTL4: mode 3            */
			(void)cs42_wr16(0x1508, 0x20);  /* PLL_CTL3: DIVOUT 0x10 * n=2 */
			(void)cs42_wr16(0x150A, 0x7D);  /* PLL_CAL_RATIO 125           */
			/* serial port: slave I2S, 50/50 frame, 1.0-cycle FSD, 16-bit  */
			(void)cs42_wr16(0x1207, 0x20);  /* ASP_CLK_CFG: SCLK_EN, slave */
			(void)cs42_wr16(0x1208, 0x0A);  /* ASP_FRM_CFG: 5050 | FSD_1_0 */
			(void)cs42_wr16(0x2A02, 0x01);  /* RX CH1: AP low,  RES 16-bit */
			(void)cs42_wr16(0x2A03, 0x00);  /* CH1 bit offset MSB          */
			(void)cs42_wr16(0x2A04, 0x00);  /* CH1 bit offset LSB          */
			(void)cs42_wr16(0x2A05, 0x41);  /* RX CH2: AP high, RES 16-bit */
			(void)cs42_wr16(0x2A06, 0x00);  /* CH2 bit offset MSB          */
			(void)cs42_wr16(0x2A07, 0x00);  /* CH2 bit offset LSB          */
			(void)cs42_wr16(0x2A01, 0x0C);  /* ASP_RX_DAI0_EN: CH1+CH2     */
			(void)cs42_wr16(0x1209, 0x03);  /* FS_RATE_EN: IASRC+OASRC 96K */
			(void)cs42_wr16(0x120A, 0x00);  /* IN_ASRC_CLK: IASRC_SEL_6    */
			(void)cs42_wr16(0x2301, 0x00);  /* MIXER_CHA_VOL: 0 dB         */
			(void)cs42_wr16(0x2303, 0x00);  /* MIXER_CHB_VOL: 0 dB         */
			/* power up: keep ASP-TX, EQ, ADC down; enable DAI+MIXER+HP    */
			(void)cs42_wr16(0x1101, 0x94);  /* PWR_CTL1                    */
			k_msleep(5);
			/* start the PLL (reference = SCLK, runs whenever I2S runs)    */
			(void)cs42_wr16(0x1501, 0x01);  /* PLL_CTL1: PLL_START         */
			for (int t = 0; t < 40; t++) {  /* poll PLL_LOCK_STATUS 0x130E */
				k_msleep(1);
				if (cs42_rd16(0x130E, &g_hp_pll) && (g_hp_pll & 1))
					break;
			}
			(void)cs42_wr16(0x1201, 0x01);  /* MCLK_SRC_SEL: PLL           */
			(void)cs42_wr16(0x1107, 0x01);  /* OSC_SWITCH: SCLK present    */
			k_msleep(2);
			(void)cs42_wr16(0x2001, 0x00);  /* HP_CTL: unmute A+B          */
			g_hp_on = 1;
			return;
		}
		if ((id8 & 0xF8) == 0x20) {
			/* ---- PATH 2: TimK's 8-bit variant (needs the MCLK osc) ---- */
			gpio_drive_high(OSC_EN_PORT, OSC_EN_PIN);
			k_msleep(5);
			(void)cs42_wr8(0x1D, 0x00);   /* out of hibernate            */
			(void)cs42_wr8(0x1B, 0x04);   /* CLK_CTL: MCLK auto-detect   */
			(void)cs42_wr8(0x2F, 0x01);   /* ASP RX: slave, I2S          */
			(void)cs42_wr8(0x30, 0x60);   /* ASP RX fmt                  */
			(void)cs42_wr8(0x1C, 0x07);   /* signal path: ASP->DAC->HP   */
			(void)cs42_wr8(0x19, 0x00);   /* power on                    */
			(void)cs42_wr8(0x1D, 0x00);   /* unmute HP                   */
			(void)cs42_wr8(0x35, 19);     /* vol A                       */
			(void)cs42_wr8(0x36, 19);     /* vol B                       */
			k_msleep(10);
			g_hp_on = 2;
			return;
		}
	}
}

static void hp_init(void)
{
	hp_codec_init(0);
}

/* ---- continuous I2S TX thread ---- */
static K_THREAD_STACK_DEFINE(audio_stack, 3072);  /* +1K margin over the historical 2048: the
                                                   * PREEMPT(0) mixer takes USB-thread
                                                   * preemptions (incl. FPU lazy-stacking
                                                   * frames) on top of its own worst case —
                                                   * the top-ranked candidate for the
                                                   * unexplained record-start crash */
static struct k_thread audio_tcb;

/* Fill one stereo I2S block with silence. Used to prime the I2S DMA at start-up
 * and after an underrun recovery, before the looper engine takes over. */
static void fill_block(int16_t *s)
{
	memset(s, 0, BLK_FRAMES * 2 * sizeof(int16_t));
}

/* STEM TAPE: UAC2 -- the USB-C audio-in ring, its diagnostics, and the
 * "GATE 1: PLAYBACK ONLY" USB-audio monitor path that used to live here are
 * REMOVED (see this file's own top-of-file comment). There is no USB-
 * sourced audio for the mixer to drain any more; looper_audio_block()'s
 * PASS A now sources its `live` term as silence, in place of the real
 * STIX-selected song stream a later Phase 2 commit wires in there. */

/* ================== LOOPER ENGINE (4 tracks, eMMC-streamed) ==============
 * Loops are mono int16 decimated from the 48000 Hz live input by DECIM and
 * stored on the eMMC (one region per track). A background streamer thread does
 * the blocking eMMC reads/writes into per-track SPSC rings; THIS audio code only
 * touches RAM. Playback is interpolated back up to the I2S rate; the 4 tracks
 * are mixed with per-track (fader) + master volume over the live monitor.
 * Recording is HOLD-to-record, UNQUANTIZED: the FIRST take you hold sets the
 * master length — exactly what you held, rounded only to the 256-sample storage
 * block (~±19 ms; works for podcasts/speech, nothing snaps or jumps). Overdubs
 * start at the next block (~38 ms = effectively instant) and record exactly one
 * loop, wrapping. "BPM" is just the varispeed label (80 = 1.0x); there is NO
 * tempo grid — the beat constants below only pace the LED pulse + MIDI clock. */
#if SP1_BUILD_24K
#define DECIM            2u                               /* 24 kHz build (see SP1_BUILD_24K) */
#else
#define DECIM            1u                               /* 48 kHz build (default) */
#endif
#define LOOP_RATE        (I2S_TRUE_HZ / DECIM)             /* 48000/DECIM Hz mono */
/* ===== STORAGE CODEC TOGGLE (compile-time) ===============================
 * Loop audio is stored COMPRESSED on flash to cut the WRITE+READ traffic that
 * is the eMMC reliability bottleneck. The audio engine is UNCHANGED: the rec
 * ring (g_rring) and play rings (trk[].pring) and the whole mix stay int16.
 * We ONLY encode on the flash write and decode on the flash read, at the three
 * flush-boundary sites (codec_pack / codec_unpack). SAMP_PER_BLK = int16
 * samples represented by ONE 512-byte flash block; it is codec-conditional.
 *   PCM   (0): 16-bit, 256 samp/blk, 1:1  (current format, memcpy-equivalent)
 *   ULAW  (1):  8-bit G.711 u-law, 512 samp/blk, 2:1
 *   ADPCM (2):  4-bit IMA, self-contained blocks: 4-byte header (predictor
 *               int16 + step-index uint8 + 1 pad) + 508 nibble-bytes = 1016
 *               samp/blk, ~4:1. Predictor RESETS at the start of every block so
 *               any block decodes standalone (random-access loop seeks work).
 * NOTE: 256 and 512 are powers of two; 1016 is NOT. The only bitmask use of
 * SAMP_PER_BLK (the prime align at the promotion site) is converted to a
 * division-based align so the non-power-of-two ADPCM value is correct. All
 * other SAMP_PER_BLK uses are already /,*,%  (block-domain). The int16 ring
 * masks (RING_MASK / RRING_MASK) are sample-domain and stay powers of two. */
#define SP1_CODEC_PCM    0
#define SP1_CODEC_ULAW   1
#define SP1_CODEC_ADPCM  2
#ifndef SP1_CODEC
#define SP1_CODEC        SP1_CODEC_PCM    /* FULL 16-BIT PCM — the proven WORKING.bin
                                           * format (magic SE4A). The u-law/ADPCM
                                           * compressed builds never worked right on
                                           * the user's hardware; do not rebase on
                                           * them again. */
#endif
#if   SP1_CODEC == SP1_CODEC_PCM
#define SAMP_PER_BLK     (EMMC_BLOCK_SIZE / 2u)            /* 256 int16 / block */
#elif SP1_CODEC == SP1_CODEC_ULAW
#define SAMP_PER_BLK     (EMMC_BLOCK_SIZE)                 /* 512 samp / block (8-bit) */
#elif SP1_CODEC == SP1_CODEC_ADPCM
#define SAMP_PER_BLK     1016u                             /* 4B hdr + 508 nibble-bytes = 1016 samp / block (4-bit IMA) */
#else
#error "SP1_CODEC must be 0 (PCM), 1 (ULAW) or 2 (ADPCM)"
#endif
/* ---- storage codec pack/unpack (full bodies just before streamer_thread) ----
 * codec_pack:   int16 ring -> flash bytes  (encode), one CMD25 burst of n blocks
 * codec_unpack: flash bytes -> int16 ring  (decode), one CMD18 burst of n blocks
 * Both take (ring, ring_mask, ring_start_sample, flashbuf, nblocks) and handle
 * the power-of-two ring wrap internally. PCM = memcpy-equivalent. */
static void codec_pack(const int16_t *ring, uint32_t ring_mask, uint32_t start,
                       uint8_t *flash, uint32_t nblk);
static void codec_unpack(int16_t *ring, uint32_t ring_mask, uint32_t start,
                         const uint8_t *flash, uint32_t nblk);
#define LOOP_BPM_BASE    80u                               /* BPM label for 1.0x varispeed */
/* FULL-RATE LOOPS: the SPIM3 hardware eMMC path measures 1333 blk/s sustained
 * REWRITE (2026-06-12 capture) — 48 kHz mono needs 187.5 blk/s write + 750
 * blk/s read (4 tracks): ~14% / ~60% of capacity. DECIM=1 also means the
 * decimator/interpolator is bit-transparent — loops record and play exactly
 * what the engine hears. Mono remains the only compromise. */
#define BEAT_SAMPLES_I2S 35840u                            /* I2S frames / beat (140 blocks ÷256) */
#define BEAT_SAMPLES_L   (BEAT_SAMPLES_I2S / DECIM)        /* 35840 = 140 blocks (÷256) */
#define BAR_SAMPLES      (BEAT_SAMPLES_L * 4u)             /* 4 beats — for display / phrasing */
#define MAX_BEATS        643u                              /* longest loop 8:00 at 1.0x (the 2.0 long-
                                                            * take bump): 16 songs x 4 tracks = 76.4% of
                                                            * the 4 GB card (7,553,024 blocks), ~912 MB
                                                            * spare. Recording follows tape speed, so a
                                                            * slowed tape holds proportionally more. */
#define MAX_LOOP_SAMPLES (BEAT_SAMPLES_L * MAX_BEATS)
/* eMMC blocks for the longest loop. At 800 beats the 4 songs × 4 tracks use ~452 MB
 * (12 kHz) / ~301 MB (8 kHz) of the 4 GB card. RAM is unchanged (always streamed). */
#define MAX_LOOP_BLOCKS  (MAX_LOOP_SAMPLES / SAMP_PER_BLK)
#define MIDI_DIV         ((BEAT_SAMPLES_L + 12u) / 24u)    /* loop samples per 24-PPQN clock (rounded) */
#define NTRK             4
/* eMMC region per track, rounded UP to a 16-block (8 KB) multiple: the card's
 * internal pages are 8 KB (TE's own format writes 8 KB sectors — see the wiki's
 * Data-Structure page). With regions 8KB-ALIGNED, every 16-block flush burst
 * lands exactly on one internal page and the card can program it without a
 * read-modify-write, which is far slower than a clean page-aligned burst. */
/* round the per-track region UP to a 4096-block (2MB) multiple so every track
 * region stays 2MB-aligned. (The original reason was a pre-erase pass that has
 * since been removed; the alignment is harmless and is kept so the on-card
 * layout / META_MAGIC do not change.) */
#define TRACK_BLOCKS     ((((MAX_LOOP_BLOCKS + 8u) + 4095u) / 4096u) * 4096u)
#define RING_SAMPLES     16384u                            /* ~341 ms read-ahead @48k (reverted 8192->16384 to give the compressed codecs comfortable play-ring margin) */
#define RING_MASK        (RING_SAMPLES - 1u)
/* Play-ring critical margin for scheduling decisions: 128 ms expressed in
 * samples at the loop rate — EXPLICIT and codec-independent (the old
 * 24u*SAMP_PER_BLK silently varied 2.5x across codec block sizes). */
#define PLAY_CRIT_SAMPLES (128u * (LOOP_RATE / 1000u))

/* ---- SONG SLOTS + eMMC layout ----------------------------------------------
 * The looper owns the whole eMMC starting at block 0: block 0 holds the slot
 * metadata (this OVERWRITES the original TE "ALBUM_PR" index, deleting the songs
 * and reclaiming the space — they couldn't be played anyway), tracks follow.
 * NUM_SLOTS independent songs, each with its own saved BPM + 4 tracks. There are
 * 16 songs shown on the 4 status LEDs with TWO lights: the POSITION LED
 * (song % 4) is solid and the BANK LED (song / 4) blinks ~2 Hz. When the two
 * roles land on the same LED (songs 1, 6, 11, 16) it flutters fast (~4 Hz). */
#define NUM_SLOTS        16u
#define META_BLOCK       0u
/* STEM TAPE: bumped 2 -> 4 blocks (972 B classic index + struct stem_meta's
 * 512 B tail array = 1484 B) to fit the new per-slot stem-tape fields
 * (struct stem_meta, appended at the meta_blk tail below) without touching
 * the classic fields' layout. GRID_EXT_BLOCK (below) moves from 2 to 4 to
 * stay clear of the enlarged region; both remain well inside the "1..4095
 * spare" range documented for SLOT0_BLOCK, so trk_blk() addressing (and the
 * Tape Looper's own SLOT0_BLOCK=4096) is completely unaffected. */
#define META_BLOCKS      4u
#define SLOT0_BLOCK      4096u  /* 2MB-aligned (block 0 = meta, 1-4095 spare) so every trk_blk stays 2MB-aligned */
/* FIXED storage signature: reflashing KEEPS the saved songs (the earlier
 * wipe-on-reflash build stamp is gone — user prefers persistence; double-tap a
 * track to delete it instead). Storage only re-formats if this constant or the
 * layout ever changes. */
/* The two sample-rate builds use different on-flash layouts (TRACK_BLOCKS
 * scales with DECIM), so each gets its own magic: switching builds is detected
 * as "unformatted" and reformats, rather than reading the other rate's data. */
/* The on-flash byte format now depends on BOTH the sample-rate build (DECIM)
 * AND the storage codec (SP1_CODEC): a different codec packs the same loop into
 * a different number of bytes/block, so the two are not interchangeable. Give
 * each (rate,codec) pair its own magic; switching either is detected as
 * "unformatted" and triggers a one-time reformat instead of mis-reading the
 * other format's bytes. */
#if DECIM == 1u
#  if   SP1_CODEC == SP1_CODEC_PCM
#define META_MAGIC       0x53383136u                       /* 'S816' — 48 kHz PCM, 16-song 2-block index,
                                                            * 643-beat (8:00) regions. TRACK_BLOCKS
                                                            * differs from earlier layouts, so this is a
                                                            * format break: any other index reads as
                                                            * unformatted and loop storage reformats on
                                                            * first boot — export loops as WAVs first,
                                                            * re-upload after. Grids (block 2) survive,
                                                            * same as site uploads today. */
#  elif SP1_CODEC == SP1_CODEC_ULAW
#define META_MAGIC       0x53455534u                       /* 'SEU4' — 48 kHz, u-law 8-bit */
#  else
#define META_MAGIC       0x53454134u                       /* 'SEA4' — 48 kHz, IMA-ADPCM 4-bit */
#  endif
#else
#  if   SP1_CODEC == SP1_CODEC_PCM
#define META_MAGIC       0x53453241u                       /* 'SE2A' — 24 kHz, PCM 16-bit */
#  elif SP1_CODEC == SP1_CODEC_ULAW
#define META_MAGIC       0x53455532u                       /* 'SEU2' — 24 kHz, u-law 8-bit */
#  else
#define META_MAGIC       0x53454132u                       /* 'SEA2' — 24 kHz, IMA-ADPCM 4-bit */
#  endif
#endif
/* STEM TAPE: a distinct format magic. Every slot in this firmware is either
 * empty or a validated 4-stem song (struct stem_meta below) -- never a
 * classic independently-recorded loop -- so this MUST differ from every
 * classic META_MAGIC above: on first boot after flashing this firmware, an
 * eMMC card carrying classic Tape Looper data reads as "unformatted" (same
 * one-time-reformat path the classic firmware already uses for every prior
 * format break) rather than being misinterpreted. 'STM1' packed the same
 * big-endian-char way as the classic magics above. */
#undef META_MAGIC
#define META_MAGIC 0x53544D31u /* 'STM1' */
static inline uint32_t trk_blk(uint32_t slot, uint32_t t)
{
	return SLOT0_BLOCK + (slot * NTRK + t) * TRACK_BLOCKS;
}
/* loop_len = this song's loop length in loop-samples (a whole number of bars,
 * 0 = empty/no loop yet). Saved so a song resumes at its own length + tempo. */
/* SEGMENT looper: each track also remembers its own length (a whole multiple of
 * the base loop_len) and its phase anchor, so a song reloads with the same
 * per-track loop lengths it was recorded with. */
struct slot_state {
	uint32_t speed_q16;
	uint32_t loop_len;
	uint8_t  present[NTRK];
	uint32_t trk_len[NTRK];      /* per-track length in eMMC blocks (0 -> base) */
	uint32_t trk_start[NTRK];    /* per-track segment-0 transport-block anchor */
};
struct meta_blk {
	uint32_t magic;
	uint32_t cur_slot;
	struct slot_state slot[NUM_SLOTS];
	uint32_t fixed_len;        /* persisted loop-length mode (0=variable, 1=fixed).
	                            * APPENDED after the slots: old metas read 0 here
	                            * (the format zeroes the block), and the transfer
	                            * site reads only the slots, so this is layout-safe. */
	uint32_t trk_content[NUM_SLOTS][NTRK]; /* per-track recorded content length in blocks; 0 = whole
	                                        * track. Also appended in the tail -> layout-safe; a website
	                                        * upload zeroes it (0 = full track = correct for uploads). */
	uint32_t led_full;         /* 0 = dim LEDs (default), 1 = full brightness.
	                            * Tail-appended like fixed_len -> layout-safe;
	                            * repaired in xfer_commit like fixed_len. */
	uint8_t  chop[NUM_SLOTS][2]; /* M7a: per-song chop window: [0]=div (0/1=none,
	                              * 2..64), [1]=offset. Zeros = unchopped. */
	uint8_t  song_mode[NUM_SLOTS]; /* LOW nibble, M7c: recorded-with mode stamp:
	                                * 0 = unset (inherit the global preference),
	                                * 1 = variable, 2 = fixed.
	                                * HIGH nibble, M7-r4: per-track MUTE bits
	                                * (bit4 = track 1 .. bit7 = track 4) — a
	                                * song's muted tracks come back muted. Old
	                                * indexes read 0 = no mutes; same 'SE16'. */
	/* STEM TAPE: appended tail field, same layout-safe convention as
	 * fixed_len/trk_content/led_full/chop/song_mode above. One entry per
	 * slot. `present[NTRK]` (in slot_state, unchanged) stays the single
	 * "does this track have audio" gate; `is_stem_song` additionally gates
	 * "was this slot committed as a VALIDATED 4-stem song" — a slot can
	 * only ever reach is_stem_song=1 via xfer_service()'s 'Z' verb (see
	 * st_stem_validate.c), which requires all 4 present[] bits set, all 4
	 * trk_len[] equal (== frame_count: the shared-transport invariant), and
	 * every declared stem_crc32[] to match a real read-back CRC. */
	struct stem_meta {
		uint8_t  is_stem_song;   /* 1 = validated, playable 4-stem song */
		uint8_t  reserved0[3];   /* explicit pad: fixed struct size regardless of ABI packing */
		uint32_t frame_count;    /* shared length in eMMC blocks, == slot_state.trk_len[0..3] */
		uint32_t stem_crc32[NTRK]; /* per-stem CRC32 (st_crc32.c), verified at commit */
		uint16_t bpm_q8;         /* Q8.8 BPM; 0 = unknown */
		uint16_t reserved1;
		uint32_t downbeat_frame; /* frame offset of the first downbeat; 0 = unknown */
	} stem[NUM_SLOTS];
};
/* The index must fit its reserved blocks. Compile error here beats storage
 * corruption there. */
BUILD_ASSERT(sizeof(struct meta_blk) <= META_BLOCKS * EMMC_BLOCK_SIZE,
	     "meta_blk outgrew its reserved index blocks");
static struct meta_blk   g_meta;
static volatile uint32_t g_slot;
static volatile int      g_slot_switch_req;   /* main -> audio: reload tracks for the new slot */
static volatile int      g_meta_save_req;     /* -> streamer: persist g_meta to eMMC */
/* ---- TAPPED GRID (M8a): per-song tempo grid taught by FN-taps ----
 * 4+ taps in rhythm set it (first tap = downbeat); independent of the tape.
 * bpm persists in flash block GRID_EXT_BLOCK — unused spare, self-validating
 * 'GRD1' tag + sum, so NO format break, the site never touches it (blocks
 * 0..META_BLOCKS-1 only), and older firmware simply ignores it. Phase is
 * session-only by design: after boot it re-anchors to the next tap run (or
 * provisionally to "now"). STEM TAPE: moved from block 2 to META_BLOCKS
 * (now 4, was 2) since META_BLOCKS grew to fit struct stem_meta above —
 * still well inside the documented "1..4095 spare" range before
 * SLOT0_BLOCK, so trk_blk() addressing is unaffected. */
#define GRID_EXT_BLOCK  META_BLOCKS
#define GRID_EXT_MAGIC  0x31445247u   /* 'GRD1' */
struct grid_ext {
	uint32_t magic;
	uint16_t bpm_q8[NUM_SLOTS];   /* Q8.8 BPM per song, 0 = no grid */
	uint16_t sum;                 /* 16-bit sum of bpm_q8[] (torn-write guard) */
};
BUILD_ASSERT(sizeof(struct grid_ext) <= 512, "grid ext must fit one block");
static volatile uint16_t g_grid_bpm_q8[NUM_SLOTS];
static volatile uint64_t g_grid_anchor;       /* sample-clock frame of a downbeat */
static volatile uint32_t g_grid_beat_frames;  /* I2S frames per grid beat (current song) */
static volatile uint64_t g_grid_next_tick;    /* next 24-PPQN tick, sample-clock domain */
static volatile uint8_t  g_grid_active;       /* current song has a live grid */
static volatile uint8_t  g_grid_save_req;     /* control -> streamer: write block 2 */
/* M8b quantized capture: with a grid, arming PUNCHES IN on the next bar line
 * (auto-start-on-sound is bypassed) and the stop rounds to the nearest grid
 * BEAT — reusing fixed mode's run-on/snap-back machinery with the tapped beat
 * as the base. Lengths quantize to a block-rounded beat so all grid takes are
 * multiples of the SAME base = mutually locked forever. */
static volatile uint64_t g_grid_punch_at;      /* sample-clock of the scheduled punch-in (0 = none) */
static volatile uint8_t  g_gridrec;            /* current take was grid-punched */
static volatile uint32_t g_gridrec_beat_samps; /* grid beat in STORED samples at punch speed */
/* M8c: performance layer. Mute/unmute WAITS for the bar line on gridded songs
 * (launch quantize); a tap run over EXISTING loops beatmatches (retunes the
 * tape + resyncs the loop start to the tapped downbeat at the next bar). */
static volatile uint64_t g_grid_next_bar;      /* next bar line, sample-clock domain */
static volatile uint64_t g_grid_resync_at;     /* pending loop-restart at this bar (0 = none) */
/* The PASS 2 forensic counters (g_p2blk/g_p2snap/g_p2yield/g_p2rfail) are
 * REMOVED along with PASS 2 itself -- see streamer_thread's own note where
 * that pass used to be. They measured the classic play-ring read-ahead, a
 * loop this firmware can never enter. */
static volatile int      g_meta_loaded;       /* streamer -> main: g_meta read at boot */

/* STEM TAPE PHASE 1: meta_write_blocks() (the classic looper's MAGIC-LAST
 * torn-write-safe song-index writer) is removed for this phase -- storage
 * fails closed (see streamer_thread()'s cold-boot load and the read-only
 * xfer_service() verb handlers), so there is no caller left. Phase 2
 * reintroduces a real, validated write path built on the same primitive. */

enum trk_state { TS_EMPTY, TS_ARMED, TS_REC, TS_DONE, TS_PLAY };

struct looptrk {
	volatile uint8_t  state;
	volatile uint16_t vol_q8;            /* fader volume, 256 = unity */
	/* pring[] (4 x 16384 x 2 = 131,072 bytes) is REMOVED. Nothing wrote it
	 * once the classic play-ring read-ahead went, and nothing read it once
	 * PASS B went -- see PASS B's own note in looper_audio_block(). Its RAM
	 * is now the stored-song ring's read-ahead depth. */
	volatile uint32_t p_w;               /*   streamer fill frontier (loop samples) */
	volatile uint32_t r_w;               /*   rec ring: audio produce (into g_rring) */
	volatile uint32_t r_r;               /*   rec ring: streamer consume */
	volatile uint32_t rec_count;         /* samples recorded so far (audio) */
	volatile uint32_t rec_target;        /* stop after this many samples (0 = open, first loop) */
	volatile uint8_t  rec_silence;       /* live phrase ended; pad silence to rec_target */
	volatile uint8_t  muted;             /* tap-to-mute: track silenced but kept */
	volatile uint8_t  solo;              /* STEM TAPE Phase 3: hold-to-solo (see
	                                      * TRACK_HOLD_SOLO_MS) -- silences every
	                                      * OTHER stem while any stem is soloed;
	                                      * classic engine never sets or reads this. */
	volatile uint8_t  starved;           /* ring underran; silent until half-refilled */
	uint16_t          fade;              /* starve-recovery fade-in position (256 = full; mixer-only) */
	uint16_t          vol_now;           /* gain actually applied last block (mixer-only; ramps toward fader/mute target) */
	uint8_t           rec_fade;          /* stop-pad fade-down remaining, of 128 (recorder-only) */
	uint8_t           rec_fstep;         /* fade decrement per sample (fits the fade inside the pad) */
	uint32_t flush_blk;                  /* streamer: next loop block to write */
	uint32_t flush_mod;                  /* wrap the flush at this many blocks (overdub = loop len) */
	/* SEGMENT looper: a track's length is a whole multiple of the base loop. The
	 * first take sets the base; an overdub records ONE base-length segment as a
	 * bounded take, and if the button is still held when the segment boundary is
	 * reached it appends another base-length segment (and another), each one a
	 * bounded take through the same proven flush path -- never the old open-ended
	 * "record until release, then figure out the length". len_blocks is the
	 * track's total length; start_blk is the transport block where its segment 0
	 * began (the phase anchor used to line playback up with where it was cut). */
	uint32_t len_blocks;                 /* this track's total LOOP length in eMMC blocks (N * base) */
	uint32_t content_blocks;             /* blocks actually recorded; [content_blocks, len_blocks) plays
	                                      * as SILENCE synthesised on read (never written to flash), so a
	                                      * fixed-mode take finalises INSTANTLY instead of real-time-
	                                      * padding a bar of zeros. 0 == whole track (old/variable/uploaded). */
	uint32_t start_blk;                  /* transport block of this take's segment 0 (playback anchor) */
	/* AUTO-START-ON-SOUND: a take ARMS on the button hold and the recorder only
	 * begins capturing at the first input past SOUND_THRESHOLD (armed waits
	 * as a fallback), so dead air before the first note never lands in the loop. */
	volatile int32_t  wait_peak;
	volatile uint32_t wait_ticks;
};
static struct looptrk trk[NTRK];

/* ONE SHARED record ring. Only one take is ever in flight (the press handler
 * refuses to arm while any track is ARMED/REC/DONE), so the four per-track rec
 * rings were waste: one ring TWICE the size costs 32 KB less RAM and absorbs
 * twice the eMMC-write transient (~2.4 s at the loop rate) before overflowing.
 * Overflow = a permanently corrupted take, so headroom here is what matters. */
#define RRING_SAMPLES    16384u   /* ~341 ms record backlog (reverted 32768->16384): the compressed codecs cut flush traffic, so the doubled rec ring is no longer needed; this reclaims RAM for the play-ring revert */
#define RRING_MASK       (RRING_SAMPLES - 1u)
/* STEM TAPE PHASE 1: g_rring (the int16 record ring the classic looper
 * decimates live USB input into) is removed -- there is no UAC2 input in
 * this phase and recording is unreachable (see looper_audio_block()), so
 * nothing ever writes it. This reclaims 32 KB of RAM. RRING_SAMPLES/
 * RRING_MASK stay defined: PASS 2's rec-ring-pressure check (now always
 * false, since trk[].state can never be TS_REC/TS_DONE) still references
 * them, and struct looptrk's r_w/r_r fields are unchanged. */
static volatile uint32_t g_rec_overruns;         /* diag: rec ring overflow events */
static volatile uint32_t g_starve_cnt[NTRK];     /* diag: per-track play-ring underrun episodes */
static volatile uint32_t g_stored_glitch_cnt;    /* diag: wfail advance-anyway commits — a STORED glitch
                                                  * replays at the same loop spot every pass (vs a live
                                                  * underrun, which is one-shot). Separating the two is
                                                  * what previous crackle hunts were missing. */
static volatile uint32_t g_i2s_wfail_cnt;        /* diag: I2S write failures (audio-path exoneration) */
static volatile uint32_t g_audio_us_max;         /* diag: worst looper_audio_block exec time, us (DWT, session) */
/*
 * THE SAME QUANTITY, PER WINDOW, and it exists because the session watermark
 * above cannot answer the question anyone asks of it.
 *
 * aus= reported 5052 us and then 4978 us across two builds and never moved
 * again inside either run -- so "the audio block occasionally takes 5 ms" was
 * never actually established: ONE early block can set a session maximum and
 * every later sample simply repeats it. A recurring spike and a single startup
 * transient are indistinguishable in that number, and they have completely
 * different fixes.
 *
 * Read-and-cleared by each print, so auswin= is the worst block in THIS
 * window. Steady near 5000 => a real recurring spike. Steady near the mean
 * while only aus= stays high => the watermark was one event and there is
 * nothing there to chase.
 */
static volatile uint32_t g_audio_us_win;
/*
 * THE COST OF WATCHING, SEPARATED FROM THE COST OF WORKING.
 *
 * main() measured 8% of the CPU during four-stem playback and the streamer
 * needs about 3 of those points. Some unknown share of the 8 is the diagnostic
 * block itself -- seven printk lines over USB CDC, twice a second -- which
 * exists only while somebody is attached and is not part of what the device
 * does. Optimising main() without knowing the split would mean optimising the
 * act of measuring.
 *
 * DWT cycles spent inside controls_diag() + its feed_wdt(), accumulated by
 * main() and read-and-cleared by the NEXT print. So diag= reports the PREVIOUS
 * window's cost, which is the only order that does not have the print trying
 * to include itself. */
static volatile uint32_t g_diag_cyc_win;
static volatile int32_t  g_play_lowat = 0x7FFFFFFF; /* diag: window MIN play-ring margin, samples */
static volatile uint32_t g_rec_hiwat;            /* diag: window MAX rec-ring fill, samples */
static volatile uint8_t  g_extcsd_dump[9];       /* diag: EXT_CSD[167,166,231,502,503,198,246,192,175] */
static volatile uint8_t  g_hpi_on;               /* 1 = HPI enabled (abort lever for maintenance ops; also proves
                                                  * the card's HPI works, for a possible future write-path V4) */
static volatile uint8_t  g_emmc_quiesce;         /* 1 = shutdown flush done: park the eMMC bus */
/* eMMC internal write cache: enabled at boot if the card has one. It absorbs the
 * record write-bursts so an overdub doesn't overflow the rec ring. The cache is
 * volatile, so it is flushed to NAND once at power-off (via g_cache_flush_req) to
 * keep the loops -- never during play, which would stall the bus. */
static volatile uint8_t  g_cache_on;           /* 1 = card write cache enabled */
static volatile uint32_t g_cache_kb;           /* diag: EXT_CSD CACHE_SIZE (KB) the card reports */
static volatile int      g_cache_flush_req;    /* power-off: streamer, flush the cache now */

/* ---- USB block-transfer mode (the file-transfer website talks to this) -----
 * A tiny binary protocol over the CDC serial console lets a WebSerial page
 * read/write raw eMMC blocks, so loops can be up/downloaded as WAV. The host
 * sends an 8-byte magic to ENTER; the streamer (the only eMMC user) then pauses
 * audio and services one command at a time. Auto-exits on 'X' or a 15 s idle. */
#define SP1_XFER_ENABLE 1                      /* 1 = USB loop-transfer (website upload/download) enabled */
#if SP1_XFER_ENABLE
static volatile uint8_t  g_xfer_mode;          /* 1 = in block-transfer mode (audio paused) */
/*
 * THE QUIESCE HANDSHAKE, and why a timing argument is not good enough.
 *
 * Transfer mode lets the upload path share storage with the playback ring,
 * which is only safe while NEITHER the audio thread nor the streamer can still
 * be touching it. Setting g_xfer_mode and assuming they noticed is a race: the
 * flag is set and xfer_service() returns, and nothing proves either thread has
 * observed it before the first command arrives and starts writing.
 *
 * So each side ACKNOWLEDGES. The audio thread sets its bit at the same point it
 * decides to emit silence; the streamer sets its bit where it skips its pass.
 * Commands that touch shared storage do not dispatch until both are set, which
 * makes the exclusion a fact rather than an estimate. Cleared on the way in, so
 * a stale acknowledgement from a previous transfer cannot stand in for a fresh
 * one.
 *
 * Failure mode if this is wrong: the upload silently overwrites audio being
 * played. Nothing errors; it just sounds wrong. That is the class of bug this
 * codebase spends effort making impossible, which is why it gets a handshake
 * instead of a comment about how unlikely it is.
 */
static atomic_t g_xfer_audio_quiesced;
static atomic_t g_xfer_stream_quiesced;

static inline bool xfer_quiesced(void)
{
	return atomic_get(&g_xfer_audio_quiesced) != 0 &&
	       atomic_get(&g_xfer_stream_quiesced) != 0;
}
/* Sized to hold ONE COMPLETE largest-possible host request, so the ISR can
 * never be forced to drop a byte no matter how the consumer is scheduled.
 *
 * This was 1024 bytes -- a size inherited from the classic Tape Looper 'W'
 * path, where the largest thing the host ever sent in one burst was a
 * 512-byte block and 1024 was therefore comfortable. A bulk 'U' request is
 * 1 command byte + a 17-byte header + an 8192-byte payload = 8210 bytes,
 * EIGHT TIMES the entire old ring, and the host writes all of it in one
 * un-chunked stream write. Real physical measurement (companion-side
 * writeMs/ackMs instrumentation, three separate failed uploads): the host
 * hands all 8210 bytes to the OS in ~30ms, i.e. bursts of up to roughly a
 * full-speed USB frame's worth (~1216 B/ms), while cdc_rx()'s consumer loop
 * polls at k_msleep(1) granularity. A 1024-byte ring therefore overflowed
 * DETERMINISTICALLY -- within a single poll interval, before the consumer
 * could possibly drain it -- and cdc_rx_isr() dropped the excess on the
 * floor. cdc_rx() then waited the full payload timeout for bytes that had
 * already been discarded and could never arrive, so every real upload
 * failed at sector 0 with ERR_TIMEOUT_PAYLOAD after a full 64s stall.
 *
 * The host cannot have more than one request in flight (it waits for the
 * 14-byte response before sending the next sector -- see the wire contract's
 * own sequencing rules), so a ring that holds one whole request plus a
 * single 64-byte USB packet of slack is not a "bigger, hopefully enough"
 * buffer: it is a hard upper bound on everything that can be outstanding at
 * once. No scheduling, priority or drain-rate assumption is left in the
 * design -- overflow becomes structurally impossible rather than unlikely.
 *
 * Costs (ST_CDC_RX_RING_BYTES - 1024) bytes of additional static RAM,
 * verified against the fail-closed RAM budget gate in CI. */
#define ST_CDC_RX_RING_BYTES (1u + ST_BULK_REQ_HEADER_BYTES + ST_BULK_PAYLOAD_BYTES + 64u)
RING_BUF_DECLARE(g_cdc_rx, ST_CDC_RX_RING_BYTES); /* CDC serial RX bytes, filled by the ISR */
static atomic_t g_cdc_rx_dropped_bytes;        /* bytes the ISR could not queue because the ring was
						 * already full -- see cdc_rx_isr()'s own comment */
#else
#define g_xfer_mode 0u                         /* transfer out: constant 0 so every g_xfer_mode branch drops */
#endif

#if SP1_XFER_ENABLE
/* ---- Stem Tape v1.1 A/B storage (docs/stem-tape-transfer-v1.1.md is the
 * current, authoritative contract; docs/stem-tape-transfer-v1.md is the
 * RETIRED v1.0 doc). Replaces the old single-index Gate 2 transfer
 * contract (command verbs V/B/S/K/C/A/D/I) ENTIRELY: that contract's own
 * command handlers, write adapters (xfer_staging_write/xfer_header_write/
 * xfer_songdata_write), and commit path (xfer_do_commit) are DELETED from
 * this file, not merely disabled behind a flag -- st_transfer.c/
 * st_library_io.c/st_xfer_wire.c/st_storage_layout.c stay in the tree and
 * keep their own host tests (tests/test_stemtape_player.c) for regression
 * evidence, but nothing in this firmware image calls them, and no path
 * from this image's command dispatch reaches emmc_write_blocks() through
 * them anymore -- v1.0 and v1.1 can never both hold a live write path in
 * the same build, by construction. The classic looper's own
 * meta_blk/trk[]/NUM_SLOTS format above is untouched, exactly as before.
 *
 * v1.1's region layout is computed ONCE at boot (streamer_thread(), right
 * after the real EXT_CSD-detected device capacity is known -- see its own
 * comment) and FROZEN for the life of the image: never relocated, even
 * once real songs may exist on the card. g_v11_layout_ready stays 0 (fail
 * closed) if the detected device cannot provide two equal, aligned song
 * regions plus two index regions; 'Q' then stays silent (docs section 2:
 * "Silence = stock firmware = read-only") and no v1.1 write path is ever
 * reachable. The functions that actually use this state
 * (xfer_v11_refresh_session(), xfer_v11_send_caps()) are defined further
 * down, after cdc_tx()/cdc_rx(), which they need. */
/* Same proven-safe first data block the classic looper's own on-flash
 * format already uses (this file's SLOT0_BLOCK=4096) [looper a8dd127:796]:
 * 2 MiB in, past every bootloader/stock-firmware-reserved block (0..4095).
 * v1.1 gets the FULL remaining device capacity from here on -- there is no
 * v1.0 write path left to share it with (see this block's own comment). */
#define ST11_STORAGE_BASE_BLOCK 4096u

static st11_region_layout_t g_v11_layout;
static volatile uint8_t     g_v11_layout_ready;
static uint32_t             g_v11_device_blocks_total; /* the real, EXT_CSD-detected device size in
							  * 512-byte blocks -- the exact value the boot-
							  * time layout computation was run against; kept
							  * separately because it can be a few blocks larger
							  * than g_v11_layout's own song B end (the even-split
							  * remainder is never allocated to either region) */

/* The one open v1.1 write-safety session, if any -- see st_ab_session.h.
 * Refreshed on every 'Q' (the natural "about to transfer" signal: docs
 * section 5 step 1, "re-query Q immediately before writing... or nothing
 * is written"), which re-reads both real index blocks and re-freezes the
 * active/inactive pair from that fresh read -- exactly the required
 * correction: the destination pair is snapshotted once per session, not
 * re-derived per write. The real 'W' handler (xfer_v11_write(), defined
 * near xfer_service() below) routes every v1.1-region write through
 * st_ab_session_check_write() against this session. */
static st_ab_session_t g_v11_session;

/* Bulk upload (docs/stem-tape-bulk-upload-v1.md), Slice C2: the ONE open
 * session's own per-sector sequence/destination tracker for the 'U'
 * command -- st_bulk_xfer.h's own pure state machine. Reset every time
 * g_v11_session itself (re)opens (xfer_v11_refresh_session(), same
 * cadence, same call site) so a bulk upload always starts a fresh
 * session expecting seq 0 at the frozen inactive song region's own
 * start -- never carries stale sequence state across sessions. */
static st_bulk_seq_t g_v11_bulk_seq;

/* Slice C3: post-commit runtime reload without reboot. Set by streamer_
 * thread the instant xfer_v11_write() detects a magic-committing write
 * genuinely landed (see that function's own comment) -- consumed by the
 * NEXT real 'F' (docs section 5 step 18: the flush always immediately
 * follows step 17's magic write), which performs the actual re-select +
 * reload handoff (see xfer_service()'s own 'F' handler and g_stem_
 * reload_req below, near g_stem_song_selected). */
static bool g_v11_commit_pending;

/* The ST11_SECTOR_BYTES (8192-byte) upload verify scratch -- for st_ab_
 * session_verify_song_before_commit()'s real read-back, and for the bulk 'U'
 * handler's payload and read-back -- NO LONGER HAS AN ALLOCATION OF ITS OWN.
 * It is the last loop-pin buffer; see xfer_scratch() at that pool's own
 * declaration for the whole argument, including why the read-ahead RING was
 * the wrong donor and stayed that way.
 *
 * Still the same "no 8192-byte automatic stack buffer" discipline the retired
 * v1.0 code's own s_commit_copy_buf followed, and still reached only one
 * command at a time (xfer_service() services one command per call), so there
 * is no concurrent use among the transfer verbs themselves to guard against. */

/* ============================================================
 * STEM TAPE Phase 2 slice 3B (continuous streaming) / 3B.1 (concurrency
 * correction): validated stored song -> the pure st_stem_stream.h state
 * machine -> real STSC decode -> st_stem_mix -> the existing physical
 * I2S output, fed by a TWO-BUFFER prefetch so the whole song plays
 * gaplessly. See streamer_thread()'s own boot-time A/B-selection block
 * AND its own per-pass prefetch step (where these buffers are filled/
 * validated from real flash) and looper_audio_block()'s own PASS C
 * (where they are consumed, RAM-only, every I2S block) for the two ends
 * of this path.
 *
 * DOUBLE BUFFER, exactly two total, per this slice's own explicit RAM
 * budget -- g_stem_sector_buf is the SAME buffer slice 2 already spent
 * (kept, unchanged in size/name); g_stem_sector_buf_b is the ONE new
 * buffer slice 3B added. Not a reuse of the upload verify scratch
 * (xfer_scratch(), declared with the loop pins below): that scratch is
 * transient, overwritten on every REPLACE-upload song-verification pass
 * inside xfer_v11_write(), and these two have to stay resident, one of them
 * mid-flight to the audio thread at any given moment, for as long as a song
 * plays. The scratch went to the PINS rather than here for exactly that
 * reason -- and see xfer_scratch()'s own comment for why this ring, which
 * keeps publishing a slot it can no longer vouch for, was the wrong donor.
 *
 * OWNERSHIP (Slice 3B.1 correction -- see st_stem_bufmbox.h's own full
 * protocol specification and correctness argument): streamer_thread is
 * the PRODUCER -- the ONLY thread that ever fills a buffer, calls
 * st_stream_validate_sector() (read-only geometry access), or calls
 * st_stem_mbox_publish_ready()/st_stem_mbox_producer_*(). audio_thread
 * is the CONSUMER -- the ONLY thread that ever reads buffer bytes for
 * playback, or calls st_stem_mbox_try_acquire()/st_stem_mbox_set_
 * requested_sector(), and the ONLY thread that ever mutates g_stem_
 * stream (song_frame/state/ready_sector -- see that struct's own doc
 * comment for why plain, non-atomic fields are now correct: each has
 * exactly one writer thread for its whole lifetime). The actual cross-
 * thread handoff -- which sector is ready, which physical buffer holds
 * it, which buffer the producer may safely refill -- lives entirely in
 * g_stem_mbox (st_stem_bufmbox.h), a formally specified lock-free SPSC
 * mailbox using real atomics with acquire/release ordering (Zephyr's
 * own atomic_t, unconditionally sequentially consistent -- see
 * ST_STEM_BUFMBOX_ZEPHYR in CMakeLists.txt), not `volatile` fields and a
 * documented write order.
 */
/* The sector ring itself. ST_STEM_MBOX_SLOTS buffers, one per mailbox
 * slot, with sector s always living in buffer (s % SLOTS) -- see
 * st_stem_bufmbox.h. Indexed directly by slot, so the count has exactly
 * one definition (the mailbox's own) and cannot drift.
 *
 * WHY THIS GREW FROM TWO: two buffers is one sector of read-ahead, i.e.
 * 7.08 ms of audio, against a measured worst-case eMMC read of 16.1 ms --
 * permanently negative margin, so the consumer could not stay fed even in
 * principle. And a starved consumer here does not drop audio, it
 * TIME-STRETCHES it (st_stream_advance_frame() freezes song_frame while
 * the needed sector is missing), which is why an under-fed stream played
 * back slow and distorted rather than merely glitching.
 *
 * DEPTH IS DERIVED, NOT CHOSEN. ST_STEM_MBOX_SLOTS comes from
 * ST_LAT_RING_SLOTS in st_latency.h: the slot the consumer holds,
 * ST_LAT_READAHEAD_SECTORS ahead of it, and one the producer may never
 * target because the consumer holds it. It was twelve, from a claim that
 * eleven sectors of read-ahead covered the driver's 80 ms start-bit
 * allowance -- 77.9 ms does not, and no depth this part can hold ever
 * would. See st_latency.h for what the firmware does guarantee, and
 * tests/test_stem_playback_gate.c for the continuous-playback proof at
 * this depth: bit-identical output hash, zero underruns. */
static uint8_t g_stem_group_bufs[ST_PL_STEMS][ST_STEM_MBOX_SLOTS][ST_PL_GROUP_BYTES];

/* RAM-NEUTRAL, and that is the whole reason G is 6. 4 x 6 x 2048 = 49,152 is
 * byte for byte what [ST_STEM_MBOX_SLOTS][ST11_SECTOR_BYTES] cost -- the ring
 * is RESHAPED by the format change, not grown. */
_Static_assert(sizeof(g_stem_group_bufs) ==
		ST_STEM_MBOX_SLOTS * ST11_SECTOR_BYTES,
		"the per-stem ring costs exactly what the v1.1 sector ring did");
_Static_assert((ST_STEM_MBOX_SLOTS % ST_PL_REFILL_GROUPS) == 0u,
		"R must divide G or a refill batch straddles the ring's end and "
		"becomes two reads -- see st_stem_mbox_producer_next_run()");
/* READ-AHEAD RUNWAY, IN MILLISECONDS -- not in groups.
 *
 * This used to require G-R >= 4 groups. That was the right quantity in the
 * wrong unit: a v1.3 group holds 510 frames where a v1.2 group held 340, so
 * "4 groups" means 28.3 ms before the migration and 42.5 ms after, and a
 * count cannot notice the difference. R rose from 2 to 3 here and the runway
 * still went UP, from 28.3 ms to 31.9 ms -- which a group-count assertion
 * would have reported as a regression from 4 to 3.
 *
 * The bound is the worst SINGLE read ever observed on this hardware during
 * live playback: rdusmx = 9316 us. Three times that is the floor, so a
 * stall of nearly ten milliseconds cannot empty the ring even if it lands
 * immediately after a refill. */
_Static_assert(((ST_STEM_MBOX_SLOTS - ST_PL_REFILL_GROUPS) *
		 ST_PL_FRAMES_PER_GROUP * 1000u) / ST11_SAMPLE_RATE_HZ >= 28u,
		"read-ahead runway must stay at or above 3x the 9.3 ms worst "
		"observed single read");

/* The lock-free SPSC handoff (st_stem_bufmbox.h) -- see this block's own
 * comment above for the producer/consumer role split. */
/* ONE PER STEM. Each stem's timeline is its own contiguous region on storage
 * and its own ring in RAM, so each gets its own mailbox -- four instances of
 * the same struct, 32 bytes each. The protocol is untouched: every instance
 * still maps group g to slot g % SLOTS, so the consumer is still wait-free at
 * one index computation per stem.
 *
 * Until reverse exists all four track the SAME group, so they fill and drain
 * in lockstep. They are separate instances rather than one shared one because
 * per-track reverse is exactly "one stem's head is elsewhere", and that is a
 * change of the numbers passed in, not of this structure. */
static st_stem_mbox_t g_stem_mbox[ST_PL_STEMS];

/* ===================================================================
 * THE LOOP KIT: ST_LAT_RESIDENCY_SECTORS pinned at each of the window's ends.
 * ===================================================================
 * A global loop must be able to exit to loop_start_frame with no inserted
 * silence, at an arbitrary instant. The ring cannot supply that, and the
 * reason is arithmetic rather than opinion:
 *
 *   - The ring maps sector s to slot (s % ST_STEM_MBOX_SLOTS), so it holds
 *     a SLIDING window of ST_LAT_READAHEAD_SECTORS sectors around the live
 *     playhead and nothing else.
 *   - The SHORTEST musical division this firmware offers is 1/8 bar, which
 *     at the measured 93.71 BPM is 15366 frames == 45.2 sectors. Every
 *     other division is longer.
 *   - So the loop always spans more sectors than the ring holds, and the
 *     sector containing loop_start_frame is ALWAYS evicted while the
 *     playhead is away from it. Reserving its slot instead of pinning a
 *     buffer does not work either: sectors S+12, S+24 ... map to that same
 *     slot and are inside the loop, so forbidding it would guarantee an
 *     underrun at the playhead.
 *   - And an underrun here is not a small blemish. stem_audio_block()
 *     writes SILENCE for the remainder of the block when the needed sector
 *     is missing, which is exactly the inserted silence a loop exit must
 *     never produce.
 *
 * SIZING, from st_latency.h. One sector is 7.083 ms of audio. The guarantee
 * is ST_LAT_GUARANTEE_US -- one worst-case MEASURED read (16.1 ms) behind one
 * typical in-flight read (5.073 ms) = 21.17 ms -- and the worst target
 * position is the last frame of its sector, which yields only (n-1)*340 + 1
 * frames of runway:
 *
 *      1 sector   8192 B ->  0.02 ms   CAN MISS
 *      2 sectors 16384 B ->  7.10 ms   CAN MISS
 *      3 sectors 24576 B -> 14.19 ms   CAN MISS  (what st17 shipped)
 *      4 sectors 32768 B -> 21.25 ms   COVERS 21.17 ms
 *
 * The margin at four sectors is 76 us. That is deliberate and it is not where
 * the safety lives: the conservatism is in the GUARANTEE (the worst read ever
 * measured, not a typical one), not in sectors piled on top of it. Piling on
 * sectors to buy visible margin against an already-worst-case figure is how
 * this pool reached a size the device cannot afford.
 *
 * The depth is derived in st_latency.h from the one measured worst-case read
 * the whole firmware sizes against; see the table below.
 *
 * TWO REGIONS, not one. A loop has two places the playhead can arrive at
 * without warning, and neither is reachable from the ring:
 *
 *   ENTRY. The window opens at the frame where PLAY went DOWN, which by the
 *   time the hold expires is ST_LOOP_HOLD_MS of song BEHIND the playhead --
 *   about 63 sectors. The entry seek jumps back to it. The same region also
 *   feeds every WRAP, which returns to exactly that frame.
 *
 *   EXIT. Every exit lands on loop_end, which is a whole window ahead of the
 *   start -- 361 sectors at one bar. An exit can happen on the pass right
 *   after entry, so this cannot be left until the release; it is fetched
 *   while the loop is still being armed.
 *
 * NEITHER duplicates the ring. The ring holds a 12-sector sliding window
 * around the LIVE playhead; both of these are far outside it for any window
 * longer than 12 sectors, which every musical division at this tempo is (the
 * shortest, 1/8 bar, is 45 sectors).
 *
 * Sector s and sector s+ST_STEM_MBOX_SLOTS share a ring slot, so a window
 * wider than the ring cannot hold both ends resident at once -- which is
 * exactly why these two are pinned OUTSIDE the ring rather than prefetched
 * into it. 2 x 24576 = 49152 bytes.
 *
 * CONCURRENCY. Far simpler than the ring, deliberately. The producer fills
 * the pin ONCE per loop entry while it is invalid, publishes the base with
 * one release store, and never rewrites it while valid; the consumer only
 * ever reads a published pin. There is no refill-while-reading hazard to
 * guard against, so this needs no slot protocol of its own.
 */
/* DEPTH COMES FROM st_latency.h, NOT FROM A CONSTANT CHOSEN HERE.
 *
 * Both regions are based EXACTLY on their seek target's sector, so both take
 * the residency depth: the target frame can sit on the LAST frame of its
 * sector, so n pinned sectors cover only (n-1) whole sectors of audio.
 * ST_LAT_RESIDENCY_SECTORS is that arithmetic, applied once, to the one
 * measured worst-case read the whole firmware now sizes against.
 *
 * WHAT THIS CORRECTS. st17 hard-coded 3 here against "10.15 ms", built by
 * doubling slice T0's TYPICAL 5.073 ms read. A depth exists for the atypical
 * case, so the typical read was the wrong quantity to double: against the
 * 16.1 ms worst read st_stem_bufmbox.h has recorded since the ring was sized,
 * three sectors (14.19 ms) do not cover the guarantee. The gate injected
 * 10.15 ms and therefore never tested the real bound. Both are fixed: the
 * depth is derived, and the gate injects ST_LAT_GUARANTEE_US.
 *
 * These two regions are the LAST separately-allocated sector pool. They exist
 * only because the read-ahead ring maps sector s to slot s % SLOTS, so a
 * window wider than the ring cannot hold both of its ends -- an artefact of
 * the ring's addressing, not a property of the problem. A unified cache with
 * associative lookup and pinnable slots removes them entirely; see
 * docs/stem-tape-ram-v1.md.
 */
#define ST_LOOP_PIN_REGIONS       2u
#define ST_LOOP_PIN_ENTRY         0u   /* loop_start: the entry seek and every wrap */
#define ST_LOOP_PIN_EXIT          1u   /* loop_end:   where every exit seek lands   */
#define ST_LOOP_PIN_ENTRY_SECTORS ST_LAT_RESIDENCY_SECTORS
#define ST_LOOP_PIN_EXIT_SECTORS  ST_LAT_RESIDENCY_SECTORS
#define ST_LOOP_PIN_SECTORS       (ST_LOOP_PIN_ENTRY_SECTORS + ST_LOOP_PIN_EXIT_SECTORS)

static const uint8_t st_loop_pin_off[ST_LOOP_PIN_REGIONS] = {
	0u, ST_LOOP_PIN_ENTRY_SECTORS
};
static const uint8_t st_loop_pin_depth[ST_LOOP_PIN_REGIONS] = {
	ST_LOOP_PIN_ENTRY_SECTORS, ST_LOOP_PIN_EXIT_SECTORS
};

/* STEM-MAJOR, and the order matters. A pinned REGION is st_loop_pin_depth[]
 * consecutive spans, so with the stem index outermost one stem's whole region
 * is contiguous both on storage and in RAM -- one read per stem per region, 8
 * reads to arm a loop where the v1.1 layout took 10. Span-major would have
 * made it 4 reads per span, 40 in total. */
static uint8_t g_stem_loop_pin_bufs[ST_PL_STEMS][ST_LOOP_PIN_SECTORS][ST_PL_GROUP_BYTES];

_Static_assert(sizeof(g_stem_loop_pin_bufs) ==
		ST_LOOP_PIN_SECTORS * ST11_SECTOR_BYTES,
		"the pins cost exactly what they did before the format change");
/* First pinned sector index per region, or negative when it holds nothing. */
static atomic_t g_stem_loop_pin_base[ST_LOOP_PIN_REGIONS] = {
	ATOMIC_INIT(-1), ATOMIC_INIT(-1)
};
/* How many of the three are filled and validated (published after the
 * bytes, read before them -- the same ordering rule the ring documents). */
static atomic_t g_stem_loop_pin_count[ST_LOOP_PIN_REGIONS] = {
	ATOMIC_INIT(0), ATOMIC_INIT(0)
};
/* Set by the control thread when the gesture ARMS, long before the loop can
 * run; the streamer picks them up. -1 means "drop this region". */
static atomic_t g_stem_loop_pin_want[ST_LOOP_PIN_REGIONS] = {
	ATOMIC_INIT(-1), ATOMIC_INIT(-1)
};

/* True, and *idx set to the FLAT buffer index, when `sector` is pinned in
 * either region. Consumer-side and wait-free: at most four acquire loads and
 * no branching into the ring. */
static bool stem_loop_pin_lookup(uint32_t sector, uint32_t *idx)
{
	uint32_t r;

	for (r = 0u; r < ST_LOOP_PIN_REGIONS; r++) {
		int32_t base = (int32_t)atomic_get(&g_stem_loop_pin_base[r]);
		int32_t cnt  = (int32_t)atomic_get(&g_stem_loop_pin_count[r]);

		if (base < 0 || cnt <= 0) {
			continue;
		}
		if (sector < (uint32_t)base ||
		    sector >= (uint32_t)base + (uint32_t)cnt) {
			continue;
		}
		*idx = (uint32_t)st_loop_pin_off[r] + (sector - (uint32_t)base);
		return true;
	}
	return false;
}

/* Drop BOTH pinned regions: nothing is resident any more, so the lookup above
 * misses and the ring alone feeds playback. The streamer's own fill step
 * refills whatever is still wanted on its very next pass, because its
 * condition is `want >= 0 && base != want` and this leaves base at -1.
 *
 * Same primitive the loop's own end already uses ("drop the pin so the ring
 * alone feeds playback again and no stale sector can ever be preferred over a
 * fresh one"), reused rather than reinvented. Streamer-thread only, which
 * includes xfer_service() -- it runs ON the streamer thread, so this needs no
 * concurrency argument beyond the one the pins already carry. */
static void stem_loop_pins_drop(void)
{
	uint32_t r;

	for (r = 0u; r < ST_LOOP_PIN_REGIONS; r++) {
		atomic_set(&g_stem_loop_pin_count[r], 0);   /* invalidate first */
		atomic_set(&g_stem_loop_pin_base[r], -1);
	}
}

/*
 * THE UPLOAD VERIFY SCRATCH IS THE LAST PIN BUFFER -- 8192 bytes reclaimed.
 *
 * WHY NOT THE READ-AHEAD RING, which is the obvious donor and was tried
 * first. Aliasing a ring slot is sound on the two ends that stop -- both
 * threads now provably quiesce, see the handshake at g_xfer_audio_quiesced --
 * but not on what is left behind. The SPSC mailbox keeps publishing the slot
 * it last published, and leaving transfer mode by 'X' commits nothing and so
 * reloads nothing: playback resumes with the ring still claiming a slot whose
 * bytes the upload overwrote. Making that claim true again needs a way to say
 * "nothing in this ring is resident any more", and the mailbox API cannot say
 * it -- st_stem_mbox_init() requires a quiescent ring AND asserts one named
 * sector is ALREADY resident and adopted, which is exactly what is false after
 * a transfer. That reclamation waits for the unified cache, which carries
 * per-slot validity and can represent "void" directly.
 *
 * THE PINS CAN SAY IT. `base = -1` IS that state, it is already part of the
 * published protocol, and the loop's own end already uses it. So entering
 * transfer mode drops both pins (see xfer_service()) and the invariant holds
 * without inventing anything:
 *
 *   1. Entry drops both pins, so no valid claim on any pin buffer survives.
 *   2. Neither thread can be mid-access: the audio thread acknowledges where
 *      it silences the block, the streamer where it skips its pass, and no
 *      command dispatches until both have.
 *   3. The streamer's pin-fill step sits BELOW its own `if (g_xfer_mode)`
 *      skip, so it cannot refill a pin during a transfer either.
 *   4. On exit, base(-1) != want, so the next streamer pass refills from
 *      flash -- the same path a re-arm takes, and playback is stopped
 *      throughout a transfer anyway (g_playing = 0).
 *
 * Which pin: the LAST one, deliberately. Index 0 is a region base and reads
 * as special; the last is not referenced anywhere except through
 * st_loop_pin_off[] + k like every other. It stays a full member of its
 * region -- the pin depth does not shrink, only its contents are dropped
 * across a transfer.
 */
#define ST_XFER_SCRATCH_STEM  (ST_PL_STEMS - 1u)
#define ST_XFER_SCRATCH_GROUP (ST_LOOP_PIN_SECTORS - ST_PL_GROUPS_PER_SECTOR)

/* Under the stem-major pool the last ST_PL_GROUPS_PER_SECTOR groups of the
 * LAST stem are 8192 contiguous bytes, which is exactly what the upload
 * verify path needs. Four groups of one stem rather than one span of four,
 * but the safety argument is unchanged and does not depend on which: entering
 * transfer mode drops BOTH regions, so no pin buffer carries a residency
 * claim across a transfer. */
_Static_assert(ST_LOOP_PIN_SECTORS >= ST_PL_GROUPS_PER_SECTOR,
		"the pin pool must be able to spare one sector's worth of "
		"contiguous bytes for the upload verify scratch");

static inline uint8_t *xfer_scratch(void)
{
	return &g_stem_loop_pin_bufs[ST_XFER_SCRATCH_STEM][ST_XFER_SCRATCH_GROUP][0];
}

/* ===================================================================
 * LOOP WINDOW, published control thread -> audio thread.
 * ===================================================================
 * The control thread owns st_loop_t and decides the grammar; the audio
 * thread owns the playhead. These three atomics are the whole handoff.
 *
 * PUBLICATION ORDER MATTERS and is the same discipline the mailbox uses:
 * the bounds are written FIRST and `active` LAST, so the audio thread can
 * never observe an active loop whose window is half-written. Clearing goes
 * the other way -- `active` first -- for the same reason.
 *
 * g_stem_loop_enter_req and g_stem_loop_exit_req are one-shot requests, not
 * levels: the audio thread consumes each with an atomic clear so one PLAY
 * gesture can only ever cause one seek, however many blocks it takes to
 * notice. Their frames are written BEFORE the request, and read after it.
 *
 * HALF-OPEN, EVERYWHERE. start_fr is inclusive, end_fr is EXCLUSIVE. The last
 * frame the window contains is end_fr - 1, and resume_fr -- the first frame
 * of ordinary playback after any exit -- is end_fr itself. The control
 * thread, this file's audio path, st_loop.c and the tests all use that one
 * convention; nothing anywhere treats end_fr as inclusive.
 */
static atomic_t g_stem_loop_active    = ATOMIC_INIT(0);
static atomic_t g_stem_loop_start_fr  = ATOMIC_INIT(0);
static atomic_t g_stem_loop_end_fr    = ATOMIC_INIT(0);
/* THE entry seek: back to the frame where PLAY went down. */
static atomic_t g_stem_loop_enter_req = ATOMIC_INIT(0);
/*
 * PER-TRACK REVERSE, as a one-shot request: (track + 1), or 0 for none.
 *
 * Track+1 rather than a bitmask, and a single slot rather than a queue,
 * because the gesture NAMES ONE TRACK and only one track can be reversed at a
 * time. Two toggles landing inside one 5.3 ms audio block is a player
 * double-tapping two different Track buttons inside one block, which the
 * gesture's own 450 ms window makes impossible; if it ever did happen the
 * later one wins, which is the same answer the surface would give.
 *
 * The audio thread consumes it with an atomic EXCHANGE to zero, so the
 * request cannot be seen twice and cannot be lost between the read and the
 * clear -- the same one-shot discipline g_stem_loop_enter_req uses, with the
 * exchange doing what its atomic_cas does.
 */
static atomic_t g_stem_reverse_req = ATOMIC_INIT(0);

/*
 * THE SCRATCH GESTURE, CONTROL THREAD -> AUDIO THREAD, IN ONE WORD.
 *
 * The control thread publishes what the hand is asking -- a target and a
 * signed drive -- and the audio thread owns the integrator that turns it into
 * a rate. That split is deliberate on both sides.
 *
 * WHY THE AUDIO THREAD INTEGRATES. The rate has to change on block boundaries,
 * because that is where the render reads it; integrating on the control thread
 * would let it move mid-block and make the two disagree about what a block was
 * rendered at. The audio thread also has the only exact clock -- one block is
 * BLK_FRAMES/48000, a constant -- while the control loop's ~8 ms is nominal.
 *
 * WHY ONE WORD AND NOT TWO ATOMICS. Target and drive must be read as a pair. A
 * torn read that took a new target with the old drive would push the wrong
 * head for one block, which at 2x is about 11 ms of the wrong stem moving --
 * audible, and exactly the kind of fault that is impossible to reproduce on
 * demand. Packing removes the question rather than reasoning about how narrow
 * the window is.
 *
 * Drive is +/-65536 and fits in 20 signed bits with room to spare; the target
 * is 0..5. Bits 0..19 drive, 20..23 target.
 */
#define ST_SCR_T_MASTER 4u
#define ST_SCR_T_NONE   5u
#define ST_SCR_PACK(tgt, drive) \
	((atomic_val_t)(((uint32_t)(tgt) << 20) | ((uint32_t)(drive) & 0xFFFFFu)))
#define ST_SCR_TGT(v)   ((uint8_t)(((uint32_t)(v) >> 20) & 0xFu))
/* Sign-extend the low 20 bits. */
#define ST_SCR_DRIVE(v) ((int32_t)((uint32_t)(v) << 12) >> 12)

static atomic_t g_stem_scratch_req =
	ATOMIC_INIT((atomic_val_t)(((uint32_t)ST_SCR_T_NONE << 20)));

/* One audio block, in microseconds -- the integrator's tick. */
#define ST_SCR_BLOCK_US ((BLK_FRAMES * 1000000u) / ST11_SAMPLE_RATE_HZ)
/* THE exit target: loop_end, the first frame after the looped section. */
static atomic_t g_stem_loop_exit_req  = ATOMIC_INIT(0);
/* Latched vs momentary, for the LED marker only -- never a decision input. */
static atomic_t g_stem_loop_latched  = ATOMIC_INIT(0);
/* Diagnostics only: how many wraps and exits the audio path actually
 * performed. Never read by any decision. */
static atomic_t g_stem_loop_wraps    = ATOMIC_INIT(0);
static atomic_t g_stem_loop_exits    = ATOMIC_INIT(0);


/* ===================================================================
 * THE STEM TAPE CONTROL DISPATCHER (st_ctl.h)
 * ===================================================================
 * Control-thread-only state, plus the single output struct everything
 * downstream reads. Serviced ONCE per control pass, at the top of the loop,
 * ABOVE the FUNCTION branch -- see st_ctl.h for why that position is
 * load-bearing rather than stylistic.
 */
static st_ctl_t     g_stem_ctl;
static st_ctl_out_t g_stem_ctl_out;

/* The pure streaming state machine (st_stem_stream.h) -- the ONE
 * authoritative song position, local sector-readiness, and underrun/
 * end-of-song/loop bookkeeping for the whole song. Slice 3B.1: now
 * EXCLUSIVELY owned and mutated by audio_thread; streamer_thread only
 * ever reads its immutable geometry fields (via st_stream_validate_
 * sector()), never its mutable ones -- see that struct's own doc
 * comment. */
/*
 * FOUR PLAYHEADS, ONE PER STEM -- and, until a track is reversed, four
 * copies of the same number.
 *
 * docs/stem-tape-per-track-reverse-spec.md names this directly: "main.c:1677
 * static st_stream_t g_stem_stream; <- ONE playhead. Four heads means four of
 * these." The struct makes it cheap. Everything above the mutable line
 * (song_start_block, song_block_count, frames, sector_count, loop_enabled) is
 * IDENTICAL for every stem -- it describes the song, not the head -- so the
 * array costs only the four mutable fields plus the direction flag, on the
 * order of 128 bytes rather than a redesign.
 *
 * BECAUSE THE GEOMETRY IS IDENTICAL, every reader that wants geometry rather
 * than position reads it through ST_STEM_GEOM below. That includes
 * streamer_thread, which touches these structs from the OTHER thread: the
 * geometry fields are written once, by st_stream_init(), before either
 * steady-state loop starts, and never reassigned -- so a concurrent read of a
 * value nobody writes again is not a race. That was already the rule when
 * there was one stream (see st_stem_stream.h's ownership section); making it
 * four does not change it, and routing those reads through one name means a
 * future field that is NOT identical cannot be read this way by accident.
 */
static st_stream_t g_stem_stream[ST_PL_STEMS];

/* THE GEOMETRY, which every head shares. Never use this to read a position. */
#define ST_STEM_GEOM (g_stem_stream[0])

/*
 * THE TRANSPORT CLOCK IS A HEAD, BUT NOT NECESSARILY STEM 0.
 *
 * The loop window, the seam duck, the beat phase and the published song frame
 * belong to the SONG, and a reversed head must not drag the song's clock
 * backwards with it. So they are all read from this head, which is always one
 * that is still going forward. There is always at least one, because only one
 * track reverses at a time -- asserted where the gesture sets it, not assumed
 * here.
 *
 * Audio-thread-owned, like the streams themselves.
 */
static uint8_t s_stem_transport;

/*
 * THE FOUR HEADS ARE INITIALISED, PLAYED AND STOPPED TOGETHER, ALWAYS.
 *
 * Not because they must stay together -- the whole point of the array is that
 * they need not -- but because a song load, a PLAY and a STOP are facts about
 * the SONG, and a version of any of them that reached three heads and missed
 * one would leave the fourth pointed at a song that no longer exists. These
 * exist so that is unrepresentable rather than merely avoided, the same
 * argument stem_rs_drop() already makes for the resampler's carried state.
 *
 * A load also puts every head forward and hands the transport back to stem 0:
 * a new song has no reversed track, and a stale direction surviving a load
 * would be a track playing backwards for no reason the player could see.
 */
static bool stem_streams_init(uint32_t song_start_block, uint32_t song_block_count,
			       uint32_t frames, uint32_t sector_count, bool loop_enabled)
{
	uint32_t k;
	bool ok = true;

	for (k = 0; k < ST_PL_STEMS; k++) {
		if (!st_stream_init(&g_stem_stream[k], song_start_block, song_block_count,
				     frames, sector_count, loop_enabled)) {
			ok = false;
		}
		/* st_stream_init() memsets, so `reverse` is already false --
		 * stated rather than relied on, because "a new song has no
		 * reversed track" is a product rule and not an artefact of how
		 * that function happens to clear its struct. */
		g_stem_stream[k].reverse = false;
	}
	s_stem_transport = 0u;
	atomic_set(&g_stem_reverse_req, 0);
	return ok;
}

static void stem_streams_play(void)
{
	uint32_t k;

	for (k = 0; k < ST_PL_STEMS; k++) {
		st_stream_play(&g_stem_stream[k]);
	}
}

static void stem_streams_stop(void)
{
	uint32_t k;

	for (k = 0; k < ST_PL_STEMS; k++) {
		st_stream_stop(&g_stem_stream[k]);
	}
}

/* The largest underrun episode count across the four heads. See its use. */
static uint32_t stem_underrun_worst(void)
{
	uint32_t k;
	uint32_t worst = g_stem_stream[0].underrun_count;

	for (k = 1; k < ST_PL_STEMS; k++) {
		if (g_stem_stream[k].underrun_count > worst) {
			worst = g_stem_stream[k].underrun_count;
		}
	}
	return worst;
}

/*
 * ONE GROUP OF SILENCE, SHARED, CONST, IN FLASH.
 *
 * A head whose group is not resident has to render SOMETHING, and the
 * alternatives are all worse than this: a per-frame "is this stem live" test
 * costs a branch 48,000 times a second on the deadline thread; skipping the
 * stem in the mixer means a second code path through the hottest loop in the
 * firmware; and reading its stale buffer is the wrong audio, played
 * confidently.
 *
 * Pointing that head's group pointer here instead makes silence fall out of
 * the arithmetic the loop already does: decoding zeros yields zeros. No
 * branch, no second path, 2 KiB of .rodata, and the starved head is silent
 * for exactly as long as it is starved.
 */
static const uint8_t g_stem_silent_group[ST_PL_GROUP_BYTES];

/* Set at boot, by streamer_thread(), only after g_stem_stream above is
 * validly initialized AND g_stem_group_bufs[..][0]/g_stem_mbox[] all hold a real,
 * header-validated sector 0 -- the same "one-shot release fence" idiom
 * this file already uses for g_v11_layout_ready/g_meta_loaded, now a
 * real atomic (Slice 3B.1) rather than a `volatile` flag, since it is
 * exactly the kind of "shared ready flag" st_stem_bufmbox.h's own
 * protocol note requires be atomic. Distinct from g_stem_stream[].state
 * (STOPPED/PLAYING/..., which toggles with transport): this flag means
 * "a valid song is currently selected and geometry-validated". Slice C3:
 * no longer "never cleared again" -- audio_thread's own post-commit
 * reload (g_stem_reload_req below) briefly clears it to 0 for the
 * duration of a runtime song-swap (during which g_playing is already 0
 * for the whole transfer session, so nothing depends on a song being
 * selected at that exact moment) before setting it back to 1 once the
 * NEW song's geometry has been validated the same way boot validates it. */
static atomic_t g_stem_song_selected;

/* Slice C3: the post-commit reload handoff, mirroring g_stem_beat_timing's
 * own already-proven "write plain fields, then an atomic release fence"
 * pattern. streamer_thread (the only thread that touches flash) fills
 * g_stem_reload_pending with the freshly re-selected STIX record AND (if
 * it names a song) reads+resides that song's real sector 0 into g_stem_
 * bufs[0] BEFORE publishing g_stem_reload_req -- audio_thread (g_stem_
 * stream/g_stem_mbox/g_stem_beat_timing's sole legitimate owner after
 * boot) performs the actual st_stream_init()/st_stem_mbox_init()/
 * st_beat_timing_init() reconstruction the next time it observes this
 * flag set (its own PASS C, once per audio block), then clears it back to
 * 0 -- see looper_audio_block()'s own comment at its check site. This is
 * safe specifically because g_playing is already 0 for the WHOLE duration
 * of any transfer session (set the instant the SP1XFER! magic is
 * detected), so stem_active is already false and audio_thread is already
 * not touching g_stem_group_bufs[]/the mailboxes for the entire window a reload
 * can ever be pending in -- confirmed by inspection, not merely assumed. */
static st_stix_record_t g_stem_reload_pending;
static atomic_t g_stem_reload_req;

/* ---- Slice 3B.1: internal runtime diagnostics (no LEDs, no user-
 * facing indication -- see this codebase's own controls_diag() USB-
 * serial diagnostic convention, e.g. g_audio_us_max/g_starve_cnt[]/
 * g_p2rfail, which these follow exactly). All atomic: streamer_thread
 * (the producer) is the sole writer of the read/corrupt counters,
 * audio_thread (the consumer) is the sole writer of the underrun
 * counter; both may be read from main()'s own diagnostic thread. */

static atomic_t g_stem_diag_bytes_total;   /* total bytes physically read (successful reads only) */
static atomic_t g_stem_diag_read_calls;    /* total emmc_read_blocks() attempts for stem sectors, success or fail */
static atomic_t g_stem_diag_read_us_last;  /* most recent read attempt's wall time, us (DWT, any outcome) */
static atomic_t g_stem_diag_read_us_max;   /* worst read attempt's wall time this session, us */
/* The same quantity, but READ-AND-CLEARED by the planar gate each pass so it
 * can attribute a worst fetch to the window it happened in. Separate from the
 * session max above precisely so clearing it cannot quietly change what the
 * STEMIO rdusmx= line has always meant. */
static atomic_t g_stem_diag_read_us_win;
static atomic_t g_stem_diag_read_us_total; /* cumulative wall time of SUCCESSFUL reads only, us -- paired with
					     * g_stem_diag_bytes_total for the sustained-rate calculation below */
/* THE PHASE BREAKDOWN, SUMMED OVER A PRINT WINDOW rather than sampled from one
 * read. sp1_emmc.c publishes hunt/spin/dma/crc for the LATEST call only, and a
 * single call is not evidence: the first capture off real hardware showed
 * hunt=3892us of a 5683us read while the session average read was 3790us, so
 * the one sample the line printed was 50% worse than typical and every ratio
 * taken from it was wrong by that much. These accumulate every read in the
 * window and are read-and-cleared by the diagnostic printer, so STEMRD states
 * an average over ~200 reads instead of whichever read happened to be last.
 * Streamer-thread-only writers, like every counter above. */
static atomic_t g_stem_diag_ph_reads;      /* reads contributing to the sums below */
static atomic_t g_stem_diag_ph_us;         /* their total wall time, us */
static atomic_t g_stem_diag_ph_hunt_us;    /* of that, the start-bit hunts */
static atomic_t g_stem_diag_ph_spin_us;    /* of the hunts, spent issuing clock pulses */
static atomic_t g_stem_diag_ph_dma_us;     /* SPIM payload transfer */
static atomic_t g_stem_diag_ph_crc_us;     /* copy-out + CRC16 */
static atomic_t g_stem_diag_ph_clks;       /* clock pulses the hunts issued */
static atomic_t g_stem_diag_ph_pulse_ns;   /* worst single clock pulse in the window, ns (max, not sum) */
/*
 * FRAMES SILENCED BY UNDERRUNS, which is the number that says whether a
 * listener can hear anything. g_stem_underrun_count above records episodes --
 * once on the transition, nothing about duration -- so it cannot distinguish
 * 21 us from 5.3 ms and must never be reported as a dropout count on its own.
 * Written only by the audio thread at its one underrun site.
 */
static atomic_t g_stem_underrun_frames;

static atomic_t g_stem_underrun_count;     /* mirrors the worst head's underrun_count, atomically, for cross-thread
					     * reads (g_stem_stream itself is audio-thread-exclusive -- see above) */
/*
 * PER-STEM FAULT COUNTERS, because the global ones cannot answer the
 * question the symptom asks.
 *
 * sil=/und=/corr= say a span was not playable, or that a fetch was rejected.
 * Under v1.1 that was the whole story: one sector carried all four stems, so
 * a fault was necessarily a fault for all four. v1.2 gives every stem its own
 * ring, its own read and its own mailbox -- so "the vocal disappeared and came
 * back while the other three kept playing" became a state the hardware can be
 * in and the diagnostics could not describe.
 *
 *   g_stem_miss[k]    the consumer needed stem k's group for the span it was
 *                     about to render and the mailbox did not have it.
 *   g_stem_badhdr[k]  a fetch for stem k came back with a group header that
 *                     was not the stem and span it asked for, so
 *                     stem_read_groups() published nothing.
 *
 * Two counters, four stems, eight words. They are the difference between
 * "something is wrong with playback" and "stem 2's ring is the one failing".
 */
static atomic_t g_stem_miss[ST_PL_STEMS];
static atomic_t g_stem_badhdr[ST_PL_STEMS];

static atomic_t g_stem_corrupt_count;      /* validated-but-wrong sectors (st_stream_validate_sector() == false);
					     * distinct from a failed physical read, which is not itself proof the
					     * sector's DATA is bad (see the prefetch step's own comment) */
static atomic_t g_stem_reload_fail_count;  /* Slice C3: post-commit runtime reloads audio_thread's own second
					     * validation pass rejected (looper_audio_block()'s own reload-
					     * consumption block) -- should be ~impossible in practice (streamer_
					     * thread already validated the identical sector0 moments earlier,
					     * nothing else writes flash in between), but counted rather than
					     * assumed never to happen: "if runtime reload fails, report
					     * explicitly, do not claim ready" -- see controls_diag()'s own print
					     * line, the SAME internal-diagnostic channel g_stem_underrun_count/
					     * g_stem_corrupt_count already use (no LEDs, no user-facing signal,
					     * observable only via the USB-serial diagnostic monitor). On this
					     * path g_stem_song_selected is left/set to 0, so the device
					     * correctly shows "no song selected" rather than a partial one. */

/* STEM TAPE Phase 3 control-matrix (beat-sync LED slice). g_stem_beat_
 * timing: written EXACTLY ONCE, by streamer_thread()'s own boot block,
 * strictly before the existing atomic_set(&g_stem_song_selected, 1)
 * publish below it -- the SAME established happens-before idiom
 * g_stem_stream's own construction already relies on (plain fields,
 * written before the release fence, safe to read by anyone who first
 * observes the fence via atomic_get()); never written again afterward,
 * so plain (non-atomic) is correct here for the exact same reason it is
 * correct for g_stem_stream's own immutable geometry fields. g_stem_
 * song_frame_pub: the transport head's song_frame's real, ongoing atomic
 * mirror, refreshed every audio block (see looper_audio_block()'s own
 * comment at its publish site) -- audio-thread-exclusive g_stem_stream
 * itself is never read from led_service() (control thread); this
 * published copy is the only safe cross-thread source for "where is
 * playback right now", the SAME one-writer/many-reader atomic pattern
 * g_stem_underrun_count above already uses. */
static st_beat_timing_t g_stem_beat_timing;
static atomic_t g_stem_song_frame_pub;
/* BEAT PULSE: per-stem peak magnitude of the most recent audio block, in
 * the STORED domain -- signed at ST11_PCM_BIT_DEPTH, so full scale is 32767
 * at v1.3's 16 bits. st_stem_meter.h derives its own full scale, reference
 * and noise floor from that same constant; the two must not drift, and
 * tests/test_stem_meter.c has a case that decodes a real stored frame and
 * asserts they have not. Written once per stem per block by the audio
 * thread (single producer), read by led_service() on the control thread
 * (single consumer). Purely observational -- nothing in the audio path
 * ever reads these back, so a torn or stale read could at worst make one
 * LED frame slightly wrong, and atomic_t already rules even that out. */
static atomic_t g_stem_peak_pub[ST11_STEM_COUNT];

/*
 * THE FOUR ENVELOPE FOLLOWERS, one per stem. Control-thread-only state --
 * led_service() is the single caller and st_stem_meter.c touches no atomic --
 * so these need no synchronisation of their own. Twenty bytes in total.
 */
/*
 * HOW OFTEN THE AUDIO THREAD SAMPLES A STEM for metering: one frame in this
 * many, so 32 is one sample per 0.67 ms. Must be a power of two -- the test
 * in the render loop is a mask, not a modulo.
 *
 * MEASURED, NOT ASSUMED. The obvious way to make the lights livelier is to
 * look at the audio more often, and it was tried: re-running the animation
 * gate with this at 8 and at 1 (every single frame, 32x the work) moved the
 * drum LED's standard deviation from 58.4 to 58.2 and its travel from 170 to
 * 172 out of 255. Nothing. A transient's rise spans 1-5 ms, which is 1.5 to 7
 * samples even at this stride, so the peak is already being caught within a
 * dB or so. What made the row static was never the sampling rate -- it was
 * having a single envelope with no transient accent.
 *
 * It stays at 32 because these cycles are spent in the 48 kHz audio thread,
 * where this file's own comments record that CPU converts one-for-one into
 * lost stream throughput and a starving stream is what made stored songs play
 * slow and crushed. Paying that for no measurable visual gain would be a bad
 * trade; the knob is here so the measurement can be repeated rather than
 * re-argued.
 */
#define ST_STEM_METER_STRIDE 32u

/*
 * PER-STEM SILENCE DETECTION -- the measurement that settles "one stem drops
 * out while the others keep playing".
 *
 * That symptom cannot be an underrun: residency is all-four-or-none and a miss
 * silences the WHOLE block, all four stems together. Every other firmware-side
 * cause has been eliminated on hardware too -- corr=0 across 27,544 reads,
 * rerr=0/werr=0, mut=0000 and stv=[0 0 0 0] in every sample of two full
 * captures, and the device's own commit check re-read the committed song off
 * eMMC and matched all four per-stem checksums. What that leaves is the
 * uploaded audio itself, and arguing about it is worse than measuring it.
 *
 * So the render loop watches what it actually decodes. A stem whose decoded
 * samples are exactly zero for a long stretch is reported with the song frame
 * the stretch began at. Two readings, two different conclusions, no debate:
 *
 *   zero run in the vocal plane at the timestamp it disappears  -> the stem
 *       that was uploaded has a gap in it; no firmware change can fill it.
 *   NO zero run while it is audibly missing -> the bytes are fine and
 *       something downstream of the decoder is silencing it, which is a real
 *       firmware bug and a completely different hunt.
 *
 * Sampled on the METER stride, not per frame: this rides the 1-in-32 test the
 * meters already pay for, so it adds a compare to a branch that is already
 * taken and nothing to the other 31 frames. A dropout worth hearing is tens of
 * thousands of frames long; 0.67 ms resolution is four orders of magnitude
 * finer than it needs to be. Audio-thread-exclusive, published atomically for
 * the diagnostic printer, never printed from the audio path.
 *
 * A stem whose prepared gain is zero is SKIPPED, not counted: muted and
 * solo-silenced stems are supposed to be silent and are not dropouts.
 */
#define ST_STEM_ZERO_RUN_MIN 1500u   /* ~1 s of stride samples before reporting */
static uint32_t s_stem_zero_run[ST11_STEM_COUNT];      /* audio thread only */
static atomic_t g_stem_zero_worst[ST11_STEM_COUNT];    /* longest run, stride samples */
static atomic_t g_stem_zero_at[ST11_STEM_COUNT];       /* song frame it began at */

static st_stem_meter_t s_stem_meters[ST11_STEM_COUNT];
static uint32_t        s_meter_last_ms;   /* 0 == no previous pass */

/* Sustained read throughput, bytes/sec, computed from the two CUMULATIVE
 * counters above (successful-read bytes and successful-read time only --
 * deliberately excludes idle time between reads and failed-read time, so
 * this reflects the eMMC path's own real transfer rate, not an idle-
 * inclusive average). Returns 0 before any successful read. Called only
 * from the low-rate USB-serial diagnostic line, never from a real-time
 * path. */
static uint32_t stem_diag_sustained_read_bytes_per_sec(void)
{
	uint32_t us = (uint32_t)atomic_get(&g_stem_diag_read_us_total);

	if (us == 0u) {
		return 0u;
	}
	uint64_t bytes = (uint64_t)atomic_get(&g_stem_diag_bytes_total);

	return (uint32_t)((bytes * 1000000ull) / us);
}
#endif /* SP1_XFER_ENABLE */

static volatile uint32_t g_consume_pos;          /* shared playhead (loop samples, free-running) */
static volatile uint8_t  g_loop_active;          /* a loop exists / master clock running */
static volatile uint32_t g_loop_len;             /* master loop length, loop-samples (0 = unset) */
static volatile uint32_t g_loop_blocks;          /* g_loop_len / SAMP_PER_BLK (streamer wrap) */
static volatile int      g_rec_track = -1;       /* the one track currently recording, or -1 */
/* Master volume Q8 (256 = unity). The VOL +/- buttons step a perceptual curve
 * (~3 dB/step) so each press is an equal-loudness change, smooth from full down
 * to silence. g_vol_idx = current position. (Per-track faders set vol_q8 directly.) */
static const uint16_t g_vol_table[] = {
	0, 2, 3, 4, 6, 8, 11, 16, 23, 32, 45, 64, 90, 128, 181, 256,
};
#define VOL_STEPS ((int)(sizeof(g_vol_table) / sizeof(g_vol_table[0])) - 1)  /* 15 */
static volatile int      g_vol_idx = 10;          /* -> 45 */
static volatile uint16_t g_master_vol_q8 = 45;
static volatile int      g_arm_req[NTRK];         /* main -> engine: track i pressed (start rec) */
static volatile int      g_stop_req;               /* main -> engine: track released (stop rec) */
static volatile int      g_del_req[NTRK];          /* main -> engine: double-tap = delete track i */
static volatile int      g_restart_req;            /* main -> engine: hold PLAY = jump to song start */
/* GLOBAL LOOP CHOP (performance window, scheme A'): play only 1/div of every
 * track's loop — the off'th slice. Non-destructive playback-window remap in
 * the streamer's fill math only: recorded audio, loop lengths, beat grid and
 * MIDI clock are untouched; div=1/off=0 is bit-identical to the original
 * math. SESSION-ONLY: resets on song switch and power-off. */
static volatile uint32_t g_chop_div = 1;           /* 1,2,4,... 64 (1 = full loop) */
static volatile uint32_t g_chop_off = 0;           /* window index: 0..div-1 */
static volatile int      g_chop_req;               /* main -> engine: window changed, snap rings */
static volatile uint32_t g_beat_phase;            /* phase within a beat (loop samples), for LEDs */
static volatile int      g_emmc_ready;
/* STEM TAPE PHASE 1: set at cold boot when block 0 is absent, unreadable, or
 * doesn't carry a recognized magic (unknown, stock/factory, classic Tape
 * Looper, corrupt, or simply blank media) -- storage fails closed in this
 * phase: never format-fresh, never write. Surfaced read-only via
 * controls_diag()'s "stg=" field. Clears only on a future recognized-and-
 * loaded index (there is no write path in this phase that could ever set
 * one). */
static volatile uint8_t  g_storage_unrecognized;
static volatile int      g_dbg_beat;              /* current beat number (diag) */
static volatile int      g_dbg_btn = -1;          /* committed track button (diag) */
static uint64_t          g_sample_clock;          /* free-running I2S frames (idle metronome) */
static int64_t           g_dec_acc;                /* live accumulator for record decimation (int64: cannot overflow when the transport is stopped / step rounds to 0) */
static uint32_t          g_frames_since;           /* I2S frames since the last loop-sample tick */
static uint32_t          g_pphase;                 /* Q16 playback phase */
static volatile uint32_t g_play_speed_q16 = 65536; /* tape speed when playing (Q16, 65536=1.0x); rocker sets */
static volatile uint8_t  g_fixed_len;              /* EFFECTIVE mode of the CURRENT song (M7c):
                                                    * 0 = variable (independent loop lengths),
                                                    * 1 = fixed (overdubs snap to track 1's base).
                                                    * = the song's recorded-with stamp when set,
                                                    * else the global preference below. */
static volatile uint8_t  g_mode_pref;              /* M7c: global working preference — what empty
                                                    * songs inherit. Toggling FUNCTION+PLAY on an
                                                    * EMPTY song sets this; on a RECORDED song it
                                                    * stamps that song only. Persisted in the
                                                    * index's fixed_len field. */
/* Tempo as an INTEGER BPM (rocker steps it 1 BPM per click for fine control).
 * Speed is derived exactly: speed = bpm * 65536 / LOOP_BPM_BASE, so 80 BPM is
 * exactly 1.0x — no detent/snap logic needed. Range 40..120 = 0.5x..1.5x. */
#define BPM_MIN 40
#define BPM_MAX 120
static volatile int g_play_bpm = 80;
/* auto-start thresholds (loop-sample domain @ LOOP_RATE) */
#define SOUND_THRESHOLD  1000              /* int16 level (~ -30 dBFS) */
#define SOUND_WAIT_TICKS (LOOP_RATE * 4u)  /* ~4 s fallback */
/* PERFECT-LOOP R2: the stop gesture's CONSTANT pipeline latency — ladder
 * debounce (~24 ms) + sustained-commit gate (~24 ms) + control pass (~8 ms)
 * ~= 55 ms — backdated out of every take so the captured end lands where the
 * finger did, not where the pipeline noticed. */
#define STOP_COMP_SAMPLES 2600u             /* ~55 ms at 48 kHz */
/* track-button gesture timing */
#define HOLD_RECORD_MS   180   /* physical button-down this long (ms) => RECORD; shorter => TAP */
#define DTAP_GAP_MS      420   /* 2nd tap within this of the 1st tap's release => DOUBLE-TAP delete */
/* STEM TAPE Phase 3 control-matrix (solo): docs/FIRMWARE_CONTRACT_V1.md
 * specifies PLAY+Track (<stemSoloLinkThresholdMs=700ms overlap) as the
 * solo gesture, but PLAY and TRACK1-4 share one resistor ladder (see
 * decode_tracks() -- only one of them is ever readable as pressed at a
 * time), so that chord cannot be read on this hardware as wired -- see the
 * fader/mute wiring commit's own note. Product decision: a track held
 * physically down at least this long activates SOLO for as long as it
 * stays held -- MOMENTARY, not a toggle: release always clears it, and
 * release sooner (never crossing this threshold) is the existing tap-to-
 * mute gesture, unchanged and never suppressed by a hold that never
 * qualified. See st_track_hold.h for the corrected state machine that
 * implements this exactly (an earlier version wrongly toggled solo once,
 * at release -- a latch, not what this comment or the product decision
 * ever specified). This constant reuses the contract's own
 * stemSoloLinkThresholdMs value verbatim rather than inventing a new one,
 * even though the gesture SHAPE (hold vs. a second control) deviates from
 * the documented chord. HOLD_RECORD_MS above (180 ms) is a different,
 * much shorter threshold for a capability Stem Tape does not have
 * (recording) and does not apply here. */
#define TRACK_HOLD_SOLO_MS 700

/* BEAT GRID for the LED pulse + MIDI clock — defaults to the nominal beat, but
 * the first-track TEMPO ESTIMATOR replaces it with the detected beat period so
 * the lights/clock track the music. It does NOT change playback speed/pitch
 * (the rocker still does tape varispeed); it's the metronome grid only. */
static volatile uint32_t g_beat_samples = BEAT_SAMPLES_L;
static volatile int      g_det_bpm;       /* diag: last detected BPM (0 = none) */
/* PRECOMPUTED MIDI-clock divisor: loop-samples per 24-PPQN tick = g_beat_samples/24.
 * Recomputed ONLY when the tempo is (re)detected, NOT per audio sample -- so the
 * detected tempo costs one divide once, not a runtime divide 48000x/sec on every
 * track (that per-sample divide was a big part of why this build lost v2's
 * headroom). The per-sample path just runs a cheap counter (g_midi_cnt). */
static volatile uint32_t g_midi_div = (BEAT_SAMPLES_L + 12u) / 24u;
static uint32_t          g_midi_cnt;      /* counts loop-samples toward the next MIDI tick */

/* Lightweight integer onset/tempo estimator, run only over the FIRST take of an
 * empty song. Envelope follower flags onsets (energy past half the running
 * peak); the median inter-onset gap is the beat period, folded to a musical
 * range. No FFT. */
#define TEMPO_MAX_ONSETS 48u
static struct {
	int      active;
	int32_t  env;
	int32_t  peak;
	int      above;
	uint32_t last_onset;
	uint32_t ioi[TEMPO_MAX_ONSETS];
	uint32_t n;
} g_tempo;
static void tempo_reset(void)
{
	memset((void *)&g_tempo, 0, sizeof(g_tempo));
	g_tempo.active = 1;
}
static inline void tempo_feed(int16_t sv, uint32_t pos)
{
	if (!g_tempo.active) return;
	int32_t a = sv < 0 ? -sv : sv;
	g_tempo.env += (a - g_tempo.env) >> 6;
	if (g_tempo.env > g_tempo.peak) g_tempo.peak = g_tempo.env;
	int32_t thr = g_tempo.peak >> 1;
	if (!g_tempo.above && g_tempo.env > thr && thr > 200) {
		g_tempo.above = 1;
		if (g_tempo.last_onset && g_tempo.n < TEMPO_MAX_ONSETS) {
			uint32_t d = pos - g_tempo.last_onset;
			if (d > LOOP_RATE / 8u) g_tempo.ioi[g_tempo.n++] = d;
		}
		g_tempo.last_onset = pos;
	} else if (g_tempo.above && g_tempo.env < (thr * 3 >> 2)) {
		g_tempo.above = 0;
	}
}
static void tempo_finish(void)
{
	g_tempo.active = 0;
	if (g_tempo.n < 2u) return;
	for (uint32_t i = 1; i < g_tempo.n; i++) {
		uint32_t v = g_tempo.ioi[i]; int j = (int)i - 1;
		while (j >= 0 && g_tempo.ioi[j] > v) { g_tempo.ioi[j + 1] = g_tempo.ioi[j]; j--; }
		g_tempo.ioi[j + 1] = v;
	}
	uint32_t beat = g_tempo.ioi[g_tempo.n / 2];
	uint32_t lo = (uint32_t)((uint64_t)LOOP_RATE * 60u / 176u);
	uint32_t hi = (uint32_t)((uint64_t)LOOP_RATE * 60u / 70u);
	while (beat > hi) beat >>= 1;
	while (beat && beat < lo) beat <<= 1;
	if (beat < lo || beat > hi) return;
	g_beat_samples = beat;
	g_midi_div = (beat + 12u) / 24u;          /* precompute once: no per-sample divide */
	g_det_bpm = (int)(((uint64_t)LOOP_RATE * 60u + beat / 2u) / beat);
}
/* Boot STOPPED (no auto-play): the saved song loads paused; PLAY (tap=resume,
 * hold=from the top) or recording starts the tape. The device used to blast the
 * last loop the instant it powered up — annoying after a flash or plug-in. */
static volatile uint8_t  g_playing = 0;            /* PLAY/STOP: target speed ramps to 0 when stopped */
static uint32_t          g_cur_speed_q16 = 0;      /* smoothed actual speed Q16 (audio thread only) */
static volatile int      g_midi_stop_pending;      /* send MIDI Stop on pause */
/* 24-PPQN clock: SINGLE-WRITER counters (audio produces, midi consumes its own
 * count). A shared pending counter with ++/-- from two threads loses pulses on
 * ARM (volatile is not atomic), drifting any synced external gear. */
static volatile uint32_t g_midi_clk_produced;      /* audio thread writes ONLY */
static volatile int      g_midi_start_pending;     /* send MIDI Start on loop activation */

static inline int16_t clamp16(int32_t x)
{
	if (x > 32767) return 32767;
	if (x < -32768) return -32768;
	return (int16_t)x;
}

/* SOFT LIMITER for the mix bus: 4 tracks at unity + the live monitor easily sum
 * past full-scale, and a hard clamp turns every peak into harsh square-wave
 * crunch ("bit-crushing" / distortion when channels stack). Below TH the signal
 * is untouched; above it the excess is compressed along a hyperbolic knee that
 * asymptotes to full-scale, so loud sums round off smoothly instead of clipping.
 * Integer, branch-light, ~no cost. */
static inline int16_t soft_limit(int32_t x)
{
	const int32_t TH = 26000;        /* ~0.8 FS linear region */
	const int32_t HEAD = 32767 - TH; /* room above the knee   */
	int32_t s = (x < 0) ? -1 : 1;
	int32_t a = x * s;               /* |x| */
	if (a > TH) {
		int32_t over = a - TH;       /* compress: y = TH + HEAD*over/(over+HEAD) */
		a = TH + (int32_t)(((int64_t)HEAD * over) / (over + HEAD));
	}
	return (int16_t)(s * a);         /* a <= 32767 by construction */
}
/* mix-only -O2: the audio hot path. Safe here (unlike global -O2): the two signed-
 * overflow UB sites are fixed with int64 casts, -fno-strict-aliasing is global, and
 * this function contains NO flash-write code -- same per-function -O2 already proven
 * werr-safe on the eMMC read path. Speeds the per-frame interp/volume/limit work. */
__attribute__((optimize("O2")))
#if SP1_XFER_ENABLE
/*
 * THE 48 kHz STORED-PLAYBACK INNER LOOP, lifted out of looper_audio_block()
 * into its own small -O2 function.
 *
 * WHY IT IS SEPARATE. It used to be written inline, inside a 1100-line
 * function compiled at -Os and carrying dozens of volatile globals. That
 * costs far more than the arithmetic does: with that much live state and
 * that many volatiles, the compiler spills and reloads around every
 * statement, and -Os will not unroll or keep values in registers across the
 * loop. Measured on hardware, the audio thread was burning ~680 CPU cycles
 * per output frame to do work that is about 180 cycles of actual
 * computation. On this device that is not merely wasteful: the eMMC read
 * path is CPU-bound, so audio-thread cycles convert one-for-one into lost
 * stream throughput, and the stream running short is exactly what made
 * stored songs play slow and crushed.
 *
 * Here there are no volatiles at all, nothing global, and few enough live
 * values to stay in registers -- and the same -O2 precedent already applied
 * to st_stem_mix_frame_prepared(), st_pl_decode_frame_shared() and
 * sp1_emmc.c's crc16().
 *
 * WHAT IT IS ALLOWED TO ASSUME (the caller establishes all of it before
 * calling, and it is exactly what st_stream_advance_frames() requires too):
 * `buf` holds the sector these frames live in, and all `n` frames lie
 * inside that one sector, inside the song, and inside this output block. So
 * there is no sector-index division here, no residency test, and no mailbox
 * traffic -- all three are invariant across the run and are done ONCE by
 * the caller, instead of 48000 times a second.
 *
 * Output and metering are byte-for-byte what the per-frame version
 * produced; the whole-song equivalence is host-tested (see
 * tests/test_stem_stream.c's run-form equivalence case).
 */
/*
 * THE SEAM. Audio-thread-exclusive state: a phase, a step counter, and the
 * jump that is waiting for the gain to reach zero. Sixteen bytes; no buffer.
 *
 * st17 performed all three loop transitions as bare st_stream_seek() calls.
 * Every one of them joined two unrelated points of the waveform with a step
 * edge, which is what the physical SP-1 reported as a blip at entry, a "seek"
 * at every wrap, and an outage on release -- with zero missing frames and zero
 * underruns, because a discontinuity is not starvation and no depth can hide
 * it.
 *
 * The three seeks are now REQUESTS. The gain ducks to zero, the seek happens
 * on the frame st_seam_jump_due() reports it has got there, and the gain comes
 * back from the new position. That is the base SP-1's BOUNDARY FADE
 * (firmware/src/main.c:1962) applied to this stream: no second playhead, no
 * overlap, no scratch buffer, no I2S transport state touched. The full
 * argument and both call chains are in docs/loop-seam-root-cause.md.
 *
 * File scope rather than function-local so the song-reload path can clear a
 * jump armed against the song that is being replaced.
 */
#define ST_SEAM_JUMP_WRAP  2u
static st_seam_t s_stem_seam;

/*
 * THE RESAMPLER'S ONE FRAME OF MEMORY. Audio-thread-exclusive, like the seam:
 * the source frame BEHIND the cursor, which the variable-rate reader blends
 * with the frame AT the cursor. Thirty-two bytes and no buffer.
 *
 * It is invalidated wherever the playhead is moved rather than advanced -- a
 * loop wrap, an exit, a song reload -- because the frame behind the cursor is
 * then a frame from somewhere else entirely, and blending across that would be
 * the discontinuity the seam exists to prevent. Invalid simply means "start
 * from the frame at the cursor", which costs one frame of hold at a seam whose
 * gain is already ducked to zero.
 */
/*
 * THE SONG'S PITCH, in half semitones, owned by the control thread and read
 * once per audio block. A plain int rather than an atomic for the same reason
 * the loop window is not: it is a single aligned 16-bit value, the audio
 * thread only ever reads it, and a block that catches an old value simply
 * renders 5 ms at the previous pitch -- which is what "the rocker took effect
 * on the next block" means anyway.
 */
static st_pitch_t s_stem_pitch;

/*
 * SLOW PLAYBACK (FX + PLAY), split across the two threads on purpose.
 *
 * The REQUEST is a single atomic boolean written by the control thread. The
 * GLIDE is a Q16 multiplier owned entirely by the audio thread, advanced from
 * the audio clock so it cannot drift against the audio it is bending. That
 * split is the same one tape inertia uses, and it means the only thing the two
 * threads share is one bool -- no torn multi-word state, and nothing for the
 * audio thread to lock.
 *
 * The multiplier is SEPARATE from s_stem_pitch and never written into it. That
 * is what makes the two controls independent: the player's semitone setting
 * survives the slow toggle untouched, and changing the rocker while slow is
 * engaged just moves the other factor of the product.
 */
static atomic_t g_stem_slow_req;
static uint32_t s_stem_slow_q16 = ST_PITCH_ONE;   /* audio-thread-only */

/* The FUNCTION + PLAY tap gesture (x1 slow, x2 snap home). Control-thread
 * only: it never touches audio state, it only decides which of the two
 * actions below to call. */
static st_fnplay_t s_stem_fnplay;

/*
 * THE TWO FUNCTION + PLAY OUTCOMES, in one place so they cannot drift apart.
 *
 * Both are CONTROL-THREAD writes to a request the audio thread reads; neither
 * touches s_stem_slow_q16, which is the audio thread's own glide state. That
 * is what keeps the transition smooth without any cross-thread handshake: the
 * control side moves a boolean, the audio side glides toward it.
 */
static void stem_slow_toggle(void)
{
	const bool want = atomic_get(&g_stem_slow_req) == 0;

	atomic_set(&g_stem_slow_req, want ? 1 : 0);
	printk("STEMTAPE slow: %s (FUNCTION+PLAY x1)\n",
	       want ? "ON, half speed" : "OFF");
}

/*
 * x2 -- "tap to match, double-tap to come home". Everything the player has
 * done to the transport speed is undone at once: the rocker's semitones AND
 * slow playback, because in a varispeed both of those ARE the speed.
 *
 * The classic tape speed is reset too. It is not what the stem transport
 * reads -- st_pitch owns that now -- but g_play_speed_q16 still drives the
 * inherited engine's own ramp, and leaving the two disagreeing would be a
 * trap for whoever reads this next.
 */
static void stem_snap_home(void)
{
	st_pitch_reset(&s_stem_pitch);
	atomic_set(&g_stem_slow_req, 0);
	g_play_speed_q16 = 65536u;
	g_play_bpm       = 80;
	printk("STEMTAPE snap home: 0.0 st, slow OFF (FUNCTION+PLAY x2)\n");
}


/*
 * THE RESAMPLER'S CARRIED STATE, PER STEM.
 *
 * s_rs_prev holds, for each stem, the source frame immediately BEHIND that
 * stem's cursor -- the other half of every variable-rate blend. It was always
 * a four-stem frame; what is new is that the four halves no longer have to
 * have come from the same source index.
 *
 * That is the whole reason these are per-stem. Per-track reverse gives one
 * stem its own cursor and its own direction, so "the frame behind the cursor"
 * is a different frame for it than for the other three -- and travelling
 * backward it is the frame at a HIGHER index. A single shared prev/frac pair
 * is correct exactly while all four stems advance together, and silently
 * wrong the moment one does not. It would not sound like an error either: it
 * would sound like reverse being subtly off only when the pitch rocker is
 * away from centre, which is the hardest kind of bug to attribute.
 *
 * Cost is four small scalars per stem, not buffers. Ordinary four-forward
 * playback still takes the unity fast path in stem_render_run(), which never
 * touches any of this -- the full-playback gate's output hash is the proof.
 */
static st11_audio_frame_t s_rs_prev;
static bool               s_rs_prev_valid[ST11_STEM_COUNT];
static uint32_t           s_stem_rate_frac[ST11_STEM_COUNT]; /* cursor fraction, Q16 [0,1) */

/* THE ONE PLACE the carried state is dropped. Five call sites drop it -- a
 * seek, a loop wrap, the wrap backstop, a song reload and a stop -- and each
 * one is a case where the frame behind the cursor belongs to a position the
 * playhead no longer occupies. Now that these are arrays, forgetting one stem
 * at one of those sites would blend one stem across a join the other three
 * ducked; a single function makes that unrepresentable. */
static void stem_rs_drop(void)
{
	uint32_t k;

	for (k = 0; k < ST11_STEM_COUNT; k++) {
		s_rs_prev_valid[k]  = false;
		s_stem_rate_frac[k] = 0u;
	}
}

/* True while every stem's cursor sits exactly on a source frame. */
static bool stem_rs_all_aligned(void)
{
	uint32_t k;

	for (k = 0; k < ST11_STEM_COUNT; k++) {
		if (s_stem_rate_frac[k] != 0u) {
			return false;
		}
	}
	return true;
}
static st_inertia_t       s_stem_inertia;

/*
 * THE ONE FX RACK. One instance, ~36 KiB, of which 36,000 B is the single
 * echo delay line. There is deliberately no per-stem array: STEM scope points
 * this rack at s_fx_target, GLOBAL scope inserts the same rack after the mix,
 * and walking the target moves it rather than creating another.
 *
 * The two scope flags are audio-thread-visible copies of the control state,
 * refreshed once per block by stem_ctl_apply(). They are separate booleans
 * rather than an enum so the two insertion points in stem_render_run() are one
 * predictable branch each, and so "no scope" costs nothing at all.
 */
static st_fx_t   g_stem_fx;
static uint8_t   s_fx_target;         /* 0..3, STEM scope */
/*
 * WHERE THE RACK IS INSERTED -- and NOT whether the overlay happens to be
 * open. That distinction is the whole of a real defect, so it is recorded
 * here rather than at the assignment.
 *
 * THE CONTRACT. st_fx_ctl.c closes the overlay with this comment, in its own
 * words: "Latches and scope are NOT cleared: latched effects keep sounding in
 * the rack's last scope, and reopening restores them." bank_service() backs it
 * up -- closing clears `momentary` and deliberately leaves `latch` standing,
 * and `active_mask` is momentary|latch.
 *
 * WHAT THE WIRING DID INSTEAD. These two flags were computed as
 * `fx_out.fx_open && scope == ...`, so the instant the overlay closed the rack
 * stopped being called at all -- while s_fx_active_mask still carried the
 * latch bit. Four things followed, every one of them audible:
 *
 *   1. A latched effect went from FULL WET to FULLY DRY in one sample, with no
 *      ramp. The 12 ms engage ramp exists precisely so that transition is not
 *      a step; skipping the call skips the ramp. That is a click.
 *   2. Reopening re-inserted the rack with wet[] still latched at
 *      ST_FX_WET_UNITY, so it snapped back to full wet. Another click.
 *   3. wet[] could never reach zero, so st48's "clear the biquad state at full
 *      disengage" reset never fired: the filter and the distortion's taming
 *      lowpass resumed from frozen history belonging to an unrelated moment in
 *      the song, ringing through the re-entry.
 *   4. The echo's release tail never ran.
 *
 * AND IT IS NOT ONE CLICK. The overlay is toggled by a VOLUME CHORD decoded
 * off an ADC ladder with a 120 ms arrival window and a 600 ms release window.
 * Every toggle of fx_open with an effect latched is a full-scale wet<->dry
 * step. A chord that decodes intermittently is a TRAIN of those steps -- which
 * is heard as crackling, on every effect equally, regardless of which effect
 * is held, because the discontinuity is in the insertion and not in any
 * effect's DSP. That is exactly the reported symptom, and it is why the -O2
 * pass (st47) and the stale-state resets (st48) could not touch it.
 *
 * THE FIX IS TO ASK THE RACK. st_fx_running() is true while anything is
 * active, while any wet leg is still ramping, and while the echo tail is
 * circulating -- so the insertion now follows the rack's own life, and the
 * scope is the one the control module remembered. With nothing latched and the
 * overlay shut, active_mask is 0, wet[] ramps down over 12 ms, st_fx_running()
 * goes false and the rack costs one test per block again. A ramped release
 * instead of a step, which is what the ramp was written for.
 */
static bool      s_fx_stem_scope;     /* rack runs on s_fx_target, pre-mix */
static bool      s_fx_global_scope;   /* rack runs on the mix, post-mix */
static uint8_t   s_fx_active_mask;    /* momentary | latch, button order */
static uint8_t   s_fx_track_claim;    /* Track bits the overlay owns this pass */
static st_fx_ctl_t g_stem_fx_ctl;
static st_fx_out_t g_stem_fx_out;

static uint32_t  s_stem_jump_to;     /* song frame to land on */
static uint32_t  s_stem_seam_lo;     /* window latched at arm time... */
static uint32_t  s_stem_seam_hi;     /* ...so a release can duck inside it */
static uint8_t   s_stem_jump_pend;   /* 0 none, else ST_SEAM_JUMP_* */

/* noclone for the same reason as stem_audio_block()'s own attribute: the
 * symbol gate requires this name exactly, and an anchored nm grep does not
 * match a .constprop/.isra clone suffix. */
__attribute__((optimize("O2"), noinline, noclone))
static void stem_render_run(const uint8_t *const grp[ST_PL_STEMS],
			     const uint32_t frame_in_group[ST_PL_STEMS],
			     const st_stem_mix_prepared_t *prep,
			     int32_t m0, int32_t md, int32_t mv,
			     uint32_t f0, uint32_t n, int16_t *s,
			     uint32_t peak[ST11_STEM_COUNT],
			     st_seam_t *seam, uint32_t transport,
			     const uint32_t rate_q16[ST_PL_STEMS],
			     uint32_t frac_io[ST_PL_STEMS],
			     const uint32_t src_avail[ST_PL_STEMS],
			     uint32_t used_out[ST_PL_STEMS],
			     const st_stream_t heads[ST_PL_STEMS],
			     const int8_t dirs[ST_PL_STEMS])
{
	/* THE SONG'S CLOCK is the TRANSPORT head's position, never stem 0's --
	 * the same number until stem 0 is the track being reversed, and then
	 * stem 0 is running backwards while the song is not. Taken by index so
	 * that the cursor offset added to it below belongs to the same head. */
	const uint32_t fx_clock_stem = transport;
	const uint32_t song_frame    = heads[transport].song_frame;

	/* Hoisted out of the 48 kHz loop: with the overlay never opened this is
	 * false and the whole rack costs one test per block. */
	const bool fx_on = st_fx_running(&g_stem_fx);
	/*
	 * THE TRANSPORT IS AT NOMINAL SPEED almost always -- inertia bends it
	 * only for the few hundred milliseconds of a spin-up or spin-down. That
	 * case is hoisted to one test per block and takes the decode call this
	 * function has always made, with the argument it has always passed, so
	 * ordinary playback is bit-identical to a build without inertia and the
	 * full-playback gate's output hash still holds.
	 */
	/*
	 * PER-STEM CURSORS, AND PER-STEM DIRECTIONS.
	 *
	 * dirs[] is +1 for a head reading forward and -1 for the one reading
	 * back. Everything below is written in terms of it, so there is exactly
	 * ONE cursor implementation rather than a forward one and a mirrored
	 * copy that can drift from it: the read index is
	 *
	 *     idx = frame_in_group + dir * cur
	 *
	 * and the frame BEHIND the cursor -- behind in the direction of travel,
	 * which for a reversed head is the frame at a HIGHER index -- is that
	 * same expression at (cur - 1).
	 *
	 * IT COMES FROM THE CALLER, not from heads[].reverse, and the
	 * difference matters exactly once: a head with no resident group is
	 * pointed at the all-zero group, where every index reads zero, so its
	 * direction is meaningless -- and giving it the transport's own index
	 * and direction is what keeps the fast path below available for the
	 * three heads that ARE reading real bytes. Deriving the sign here
	 * instead would drop all four onto the interpolating path for the
	 * duration of one head's starvation, which is a one-sample shift on
	 * stems that never lost anything.
	 */
	uint32_t frac[ST_PL_STEMS];
	uint32_t cur[ST_PL_STEMS];      /* source frames consumed within this run */
	uint32_t idx[ST_PL_STEMS];      /* where each stem reads, inside its group */
	uint32_t sp;
	/*
	 * THE FAST PATH, and the two things it now requires.
	 *
	 * The transport is at nominal speed almost always -- inertia bends it
	 * only for the few hundred milliseconds of a spin-up or spin-down --
	 * and every head reads the same frame of the same group unless a track
	 * is reversed. When both hold, this takes the decode this function has
	 * always made, with the argument it has always passed, so ordinary
	 * four-forward playback is bit-identical to a build without any of
	 * this: the full-playback gate's output hash is the proof.
	 */
	/*
	 * `together` now also asks whether the heads share a SPEED, because the
	 * rate is no longer one number for the whole block. The shared decode
	 * walks ONE cursor for all four stems, so it is correct only when they
	 * agree on direction, position AND rate -- one stem scratching drops
	 * the whole block onto the variable-rate path, which is right, and is
	 * why an isolated scratch costs CPU on the other three as well as on
	 * itself.
	 *
	 * Folded into the loop that already walks the stems rather than added
	 * as a second pass: this runs once per run, on the deadline thread.
	 */
	bool together = true;

	for (sp = 0; sp < ST_PL_STEMS; sp++) {
		frac[sp] = frac_io[sp];
		cur[sp]  = 0u;
		if (dirs[sp] < 0 || frame_in_group[sp] != frame_in_group[0] ||
		    rate_q16[sp] != rate_q16[0]) {
			together = false;
		}
	}

	const bool unity = (rate_q16[0] == ST_RS_ONE) && stem_rs_all_aligned() && together;

	for (uint32_t k = 0; k < n; k++) {
		const uint32_t f = f0 + k;
		st11_audio_frame_t frame;
		int16_t stem_l, stem_r;

		if (unity) {
			st_pl_decode_frame_shared(grp, frame_in_group[0] + k, &frame);
			/* at 1x source and output are the same count */
			for (sp = 0; sp < ST_PL_STEMS; sp++) {
				cur[sp] = k;
			}
		} else {
			/*
			 * VARIABLE RATE. The output frame sits between the
			 * source frame BEHIND the cursor and the one AT it, and
			 * is a straight linear blend of the two. Backward-
			 * looking on purpose: a forward reader would need the
			 * frame after the cursor, which at the end of a run
			 * lives in a sector that is not resident. The frame
			 * behind is one this loop already decoded and kept.
			 *
			 * PITCH AND TIME ARE THE SAME THING HERE. Nothing
			 * corrects the pitch, because the position advancing at
			 * `rate_q16` IS the pitch change -- reading the tape
			 * slowly is what makes it sound slow. There is no
			 * time-stretch anywhere in this path.
			 */
			st11_audio_frame_t nxt;

			/* THE HARD BOUND. st_rs_out_frames() floors at one
			 * output frame, and above 1x that single forced frame
			 * can ask for a source frame the run does not contain.
			 * Holding the last available frame is a degenerate
			 * corner measured in single frames; reading past the
			 * group buffer is memory corruption in a real-time
			 * thread. */
			for (sp = 0; sp < ST_PL_STEMS; sp++) {
				const uint32_t c = (cur[sp] >= src_avail[sp])
						   ? (src_avail[sp] - 1u) : cur[sp];

				idx[sp] = (uint32_t)((int32_t)frame_in_group[sp] +
						      dirs[sp] * (int32_t)c);
			}
			/* THE ARRAY FORM, one index per stem -- but only when the
			 * four indices actually differ, which today they never
			 * do.
			 *
			 * st_pl_decode_frame() lives in st_planar.c, so this was
			 * a call across a translation unit once per output frame
			 * at 48 kHz, on the deadline thread, for the ENTIRE
			 * variable-rate path. Unity playback stopped paying it in
			 * st45 and this path kept paying it, which is exactly
			 * what the hardware reported: ordinary playback clean,
			 * the pitch rocker off centre crackling again.
			 *
			 * One rate and one direction means all four cursors are
			 * the same number, so the equal case is 100% of today's
			 * traffic and takes the inline decode. The array call
			 * REMAINS, reached the moment a stem's cursor genuinely
			 * diverges -- which is per-track reverse, and is why the
			 * array form exists at all. Three compares per frame buy
			 * back a call plus a four-element array construction, and
			 * tests/test_planar.c already asserts the two forms agree
			 * when the indices are equal. */
			if (idx[0] == idx[1] && idx[1] == idx[2] &&
			    idx[2] == idx[3]) {
				st_pl_decode_frame_shared(grp, idx[0], &nxt);
			} else {
				st_pl_decode_frame(grp, idx, &nxt);
			}
			for (sp = 0; sp < ST_PL_STEMS; sp++) {
				if (!s_rs_prev_valid[sp]) {
					s_rs_prev.stem_l[sp] = nxt.stem_l[sp];
					s_rs_prev.stem_r[sp] = nxt.stem_r[sp];
					s_rs_prev_valid[sp] = true;
				}
			}
			for (sp = 0; sp < ST11_STEM_COUNT; sp++) {
				const int32_t pl = s_rs_prev.stem_l[sp];
				const int32_t pr = s_rs_prev.stem_r[sp];

				frame.stem_l[sp] = pl +
					(int32_t)(((int64_t)(nxt.stem_l[sp] - pl) *
						    (int32_t)frac[sp]) >> 16);
				frame.stem_r[sp] = pr +
					(int32_t)(((int64_t)(nxt.stem_r[sp] - pr) *
						    (int32_t)frac[sp]) >> 16);
			}
			/*
			 * Advance the cursor by one output frame's worth of
			 * source. ABOVE 1x this can cross more than one frame,
			 * so it WALKS them rather than jumping: `prev` must end
			 * up holding the frame immediately behind the new
			 * cursor, or the next output frame would blend across a
			 * gap it never looked at. Below 1x the loop runs at
			 * most once and this is what it always was.
			 */
			for (sp = 0; sp < ST_PL_STEMS; sp++) {
				frac[sp] += rate_q16[sp];
				while (frac[sp] >= ST_RS_ONE) {
					frac[sp] -= ST_RS_ONE;
					cur[sp]++;
					if (cur[sp] >= src_avail[sp]) {
						/*
						 * OUT OF RUN. Reachable only
						 * from the floored corner in
						 * st_rs_out_frames() -- one
						 * forced output frame that at a
						 * rate above 1x wants more
						 * source than the run holds.
						 *
						 * The whole frames the rate
						 * asked for beyond the run are
						 * not there, so they are
						 * dropped; the SUB-FRAME phase
						 * is kept, because throwing it
						 * away would be a position step
						 * and leaving frac above 1.0
						 * would make the next blend
						 * extrapolate past both its
						 * samples. ST_RS_ONE is a power
						 * of two, so the mask is
						 * exactly "the fractional
						 * part".
						 */
						cur[sp] = src_avail[sp];
						s_rs_prev.stem_l[sp] = nxt.stem_l[sp];
						s_rs_prev.stem_r[sp] = nxt.stem_r[sp];
						frac[sp] &= (ST_RS_ONE - 1u);
						break;
					}
					{
						/* ONE STEM'S frame behind its
						 * OWN new cursor. Decoding all
						 * four here would be three
						 * stems' work thrown away and,
						 * once directions differ, three
						 * stems read at a position that
						 * is not theirs. */
						uint32_t pc = cur[sp] - 1u;
						uint32_t pidx;

						if (pc >= src_avail[sp]) {
							pc = src_avail[sp] - 1u;
						}
						/* BEHIND IN THE DIRECTION OF
						 * TRAVEL: one step back along
						 * this head's own path, which
						 * for a reversed head is a
						 * HIGHER index in the group. */
						pidx = (uint32_t)((int32_t)frame_in_group[sp] +
								   dirs[sp] * (int32_t)pc);
						/*
						 * THE FRAME BEHIND THE NEW CURSOR
						 * IS USUALLY THE ONE ALREADY IN
						 * HAND.
						 *
						 * On the first step of this walk
						 * the cursor moves from c to c+1,
						 * so the frame behind it is c --
						 * which is exactly where `nxt`
						 * was just decoded. Re-reading it
						 * from the group was four decodes
						 * per output frame thrown away,
						 * and at any rate at or above 1x
						 * that is EVERY frame: it roughly
						 * doubled the cost of the
						 * variable-rate render against
						 * the unity one, which is why the
						 * pitch rocker pushed the audio
						 * block past its 5.333 ms
						 * deadline while unity playback
						 * sat comfortably inside it.
						 *
						 * The compare is against the index
						 * `nxt` was actually decoded at,
						 * including the clamp, so it is
						 * correct rather than merely
						 * usually correct -- and a second
						 * or later step of the walk (rates
						 * above 2x) still decodes for
						 * real. Identical bytes either
						 * way: it is the same frame of
						 * the same group.
						 */
						if (pidx == idx[sp]) {
							s_rs_prev.stem_l[sp] = nxt.stem_l[sp];
							s_rs_prev.stem_r[sp] = nxt.stem_r[sp];
						} else {
							st_pl_decode_stem_inline(
								grp[sp], pidx,
								&s_rs_prev.stem_l[sp],
								&s_rs_prev.stem_r[sp]);
						}
					}
				}
			}
		}

		/* ---- THE FX RACK, STEM SCOPE ------------------------------
		 * Before the mix, so it lands where the contract puts it:
		 *   decoded stem -> RACK -> stem fader -> solo -> stem mix.
		 * One rack, one stem: g_stem_fx_target names it, and moving the
		 * target moves this same rack, which is why the stem it leaves
		 * returns to dry with nothing to tear down. The decoder's Q23
		 * domain is the rack's domain, so nothing is scaled here. */
		if (s_fx_stem_scope && fx_on) {
			/* INTO THE RACK'S OWN DOMAIN, AND BACK.
			 *
			 * The rack is Q23 throughout (ST_FX_SHIFT) -- the
			 * distortion's tanh table is indexed against 2^23 full
			 * scale, the gate's edge ramp and the echo's damping
			 * coefficient are all fixed point against it. A v1.2
			 * stem sample WAS Q23, so the stem-scope insertion could
			 * hand it over untouched; a v1.3 sample is Q15, which is
			 * 256x smaller.
			 *
			 * Handing that straight to the rack would not fail
			 * loudly. It would work, quietly and wrongly: every
			 * sample lands in the bottom 1/256 of the shaper's
			 * curve, where tanh(15x) is indistinguishable from a
			 * straight line, so "distortion" would become a gain
			 * stage that does nothing. The GLOBAL scope below has
			 * always done this shift, because the mixer has always
			 * handed it int16 -- v1.3 simply makes both insertion
			 * points the same shape. */
			int32_t fl = frame.stem_l[s_fx_target] << ST_FX_STEM_SHIFT;
			int32_t fr = frame.stem_r[s_fx_target] << ST_FX_STEM_SHIFT;

			/* THE RACK'S TIME INDEX IS ITS TARGET'S OWN POSITION.
			 * STEM scope processes exactly one stem, so the echo's
			 * clock is that stem's cursor -- which is the right
			 * answer whether or not it is the one running
			 * backwards, and was indistinguishable from the shared
			 * cursor while all four moved together. */
			st_fx_process(&g_stem_fx, &fl, &fr,
				       (uint32_t)((int32_t)heads[s_fx_target].song_frame +
						   dirs[s_fx_target] * (int32_t)cur[s_fx_target]));
			/* st_fx_process() clamps its output to the Q23 range, so
			 * the shift back cannot exceed the stored sample's own
			 * range and needs no second saturation. */
			frame.stem_l[s_fx_target] = fl >> ST_FX_STEM_SHIFT;
			frame.stem_r[s_fx_target] = fr >> ST_FX_STEM_SHIFT;
		}

		/* INLINE, from st_stem_mix.h -- the last out-of-line
		 * cross-translation-unit call this loop made. The decode
		 * (st45) and the seam were already inline; what was left was
		 * eight int32_t being spilled to the stack purely so a pointer
		 * to them could cross a TU boundary 48,000 times a second.
		 * Identical arithmetic, same single implementation: the
		 * out-of-line st_stem_mix_frame_prepared() now calls this. */
		st_stem_mix_frame_prepared_inline(&frame, prep, &stem_l, &stem_r);

		/* BEAT PULSE: per-stem peak, sampled one frame in 32. The
		 * meters are read by led_service() at ~40 Hz, so 1.5 kHz is
		 * already two orders of magnitude finer than anything the
		 * lights can show. A zero prepared gain is the same value the
		 * mixer just multiplied by, so a muted, solo-silenced or
		 * faded-out stem meters dark with no second rule to keep in
		 * sync. INT32_MIN cannot occur in a sign-extended 24-bit
		 * value, so plain negation is safe. */
		if ((f & (ST_STEM_METER_STRIDE - 1u)) == 0u) {
			for (uint32_t sp = 0; sp < ST11_STEM_COUNT; sp++) {
				int32_t l, r;
				uint32_t mag;

				if (prep->gain_q8[sp] == 0) {
					continue;
				}
				l = frame.stem_l[sp];
				r = frame.stem_r[sp];
				if (l < 0) l = -l;
				if (r < 0) r = -r;
				mag = (uint32_t)((l > r) ? l : r);
				if (mag > peak[sp]) {
					peak[sp] = mag;
				}
				/* SILENCE RUN -- see ST_STEM_ZERO_RUN_MIN.
				 * mag is already the magnitude this branch
				 * computed for the meter, so the whole check
				 * is one compare against a value in hand. */
				if (mag == 0u) {
					if (s_stem_zero_run[sp] == 0u) {
						/* remember where it began */
						atomic_set(&g_stem_zero_at[sp],
							   (atomic_val_t)(song_frame + cur[sp]));
					}
					s_stem_zero_run[sp]++;
					if (s_stem_zero_run[sp] >= ST_STEM_ZERO_RUN_MIN &&
					    s_stem_zero_run[sp] >
					    (uint32_t)atomic_get(&g_stem_zero_worst[sp])) {
						atomic_set(&g_stem_zero_worst[sp],
							   (atomic_val_t)s_stem_zero_run[sp]);
					}
				} else {
					s_stem_zero_run[sp] = 0u;
				}
			}
		}

		/* MASTER VOLUME, on the same per-frame ramp the classic path
		 * uses: a VOL step is ~3 dB and applying it as a hard jump at
		 * a block boundary is an audible click. st_stem_mix_frame_
		 * prepared() has already saturated into int16 range and master
		 * volume only attenuates (Q8 unity = 256 is its clamped
		 * maximum), so the product cannot leave int16 range. */
		/* ---- THE FX RACK, GLOBAL SCOPE ----------------------------
		 * After the audible stem mix -- so it carries every stem, the
		 * faders and the solo result the contract already ordered -- and
		 * before the master-volume/seam output stage. The SAME rack
		 * instance as the stem path above; only its insertion point
		 * differs, which is what "one rack" means.
		 *
		 * The mixer hands back int16; the rack works in Q23, so the pair
		 * is shifted up 8 and saturated back. Two shifts, no scaling
		 * decision, one processing path for both scopes. */
		if (s_fx_global_scope && fx_on) {
			int32_t gl = (int32_t)stem_l << 8;
			int32_t gr = (int32_t)stem_r << 8;

			/* GLOBAL scope carries the whole mix, so its clock is
			 * the TRANSPORT's own position -- `song_frame` is the
			 * transport head's, and cur[] advances by the same
			 * count for every forward head, so stem 0's cursor
			 * offset is the transport's whenever stem 0 is going
			 * forward. It is the transport itself whenever stem 0
			 * is the one reversed, because then the transport is
			 * some other stem and this offset would be backwards --
			 * so the offset is taken from the transport's own
			 * direction, which is always forward. */
			st_fx_process(&g_stem_fx, &gl, &gr, song_frame + cur[fx_clock_stem]);
			gl >>= 8;
			gr >>= 8;
			if (gl > INT16_MAX) gl = INT16_MAX;
			if (gl < INT16_MIN) gl = INT16_MIN;
			if (gr > INT16_MAX) gr = INT16_MAX;
			if (gr < INT16_MIN) gr = INT16_MIN;
			stem_l = (int16_t)gl;
			stem_r = (int16_t)gr;
		}

		{
			const int32_t m = md ? (m0 + ((md * (int32_t)(f + 1)) >> 8)) : mv;
			/* THE SEAM, applied exactly where the base SP-1 applies
			 * its BOUNDARY FADE: one more Q8 multiply on a value the
			 * mixer is already multiplying. At unity (256) this is
			 * the identity for every int16, so ordinary playback is
			 * bit-identical to what shipped -- the full-playback
			 * gate's output hash is the proof. */
			const int32_t sg = (int32_t)st_seam_gain(seam);

			s[2 * f]     = (int16_t)(((((int32_t)stem_l * m) >> 8) * sg) >> 8);
			s[2 * f + 1] = (int16_t)(((((int32_t)stem_r * m) >> 8) * sg) >> 8);
		}
		st_seam_tick(seam);
	}

	/* The SOURCE frames this run actually consumed, PER STEM, which is what
	 * each stream must be advanced by. Reported rather than recomputed by
	 * the caller: two derivations of the same count is exactly how a
	 * playhead and the audio it reads drift apart -- and once the four
	 * counts can differ, re-deriving them outside this loop would mean
	 * re-deriving the direction and rate logic too.
	 *
	 * All four are equal today. The caller may still rely on that; what it
	 * may not do is compute the number itself. */
	if (unity) {
		for (sp = 0; sp < ST_PL_STEMS; sp++) {
			used_out[sp] = n;
		}
		return;
	}
	for (sp = 0; sp < ST_PL_STEMS; sp++) {
		frac_io[sp] = frac[sp];
		/* Never report more than the run held, whatever the arithmetic
		 * did. */
		used_out[sp] = (cur[sp] > src_avail[sp]) ? src_avail[sp] : cur[sp];
	}
}
#endif /* SP1_XFER_ENABLE */

/* Master-volume ramp for ONE audio block, shared by both engines.
 *
 * The VOL buttons step ~3 dB at a time; applying that as a hard gain jump at a
 * block boundary is an audible click, so the gain ramps linearly across the
 * block. mv_prev lives here, in the single place that computes the ramp,
 * precisely so the Stem Tape fast path and the classic engine cannot drift:
 * whichever one renders a block, the ramp advances exactly once, from exactly
 * where the previous block left it. */
static void master_vol_ramp(int32_t *m0, int32_t *md, int32_t *mv_out)
{
	static int32_t mv_prev;
	const int32_t mv = (int32_t)g_master_vol_q8;

	*md = mv - mv_prev;
	*m0 = mv_prev;
	*mv_out = mv;
	mv_prev = mv;
}

/* The per-block tail that belongs to NEITHER engine: the free-running I2S
 * sample clock, the tapped-grid MIDI tick (wall time, not tape time), the beat
 * phase the LEDs read, and the stem song-frame mirror. Both paths call it
 * exactly once per block, so neither can drop a MIDI tick or stall the LED
 * phase by virtue of being the one that rendered. */
static void audio_block_epilogue(void)
{
g_sample_clock += BLK_FRAMES;
/* TAPPED GRID: MIDI clock in wall (I2S) time, produced block-wise, even
 * with the transport stopped — the grid is the decks' clock, not the
 * tape's. Bounded catch-up: a block is ~5 ms, ticks are >=10 ms. */
if (g_grid_active && g_grid_beat_frames) {
	uint32_t gtick = g_grid_beat_frames / 24u;
	if (!gtick) gtick = 1u;
	while (g_sample_clock >= g_grid_next_tick) {
		g_grid_next_tick += gtick;
		g_midi_clk_produced++;
	}
	/* M8c: BAR-LINE service — launch-quantized mutes apply here, and a
	 * pending beatmatch resync restarts the loops on the tapped "1".
	 * Bars are ~2 s and blocks ~5 ms: one crossing per block, max.
	 * (v2.0.0: launch-quantized MUTES were removed after live testing —
	 * a bar is up to ~5 s of felt lag; mutes are instant everywhere
	 * now, like 1.x. The bar service keeps only the beatmatch resync;
	 * recording punch-ins stay bar-quantized via g_grid_punch_at.) */
	if (g_grid_next_bar && g_sample_clock >= g_grid_next_bar) {
		if (g_grid_resync_at && g_sample_clock >= g_grid_resync_at) {
			g_grid_resync_at = 0;
			g_restart_req = 1;      /* loops from the top, ON the "1" */
		}
		g_grid_next_bar += (uint64_t)g_grid_beat_frames * 4u;
	}
}
/* Beat-phase display computed ONCE per block now (was per loop-sample). It
 * only feeds the LED + MIDI-grid diag, so block granularity (~5 ms) is plenty
 * -- this lifts three runtime divides off the per-sample hot path. */
if (g_loop_active) {
	uint32_t bs = g_beat_samples ? g_beat_samples : BEAT_SAMPLES_L;
	if (g_loop_len > 0u) {
		uint32_t lp = g_consume_pos % g_loop_len;
		g_beat_phase = lp % bs;
		g_dbg_beat = (int)(lp / bs);
	} else {
		g_beat_phase = g_consume_pos % bs;
		g_dbg_beat = (int)(g_consume_pos / bs);
	}
} else {
	g_beat_phase = (uint32_t)((g_sample_clock % BEAT_SAMPLES_I2S) / DECIM);
}
#if SP1_XFER_ENABLE
/* STEM TAPE Phase 3 control-matrix (beat-sync LED slice): publishes
 * the TRANSPORT head's song_frame -- "the ONE authoritative absolute song
 * frame" (st_stem_stream.h's own words), audio-thread-EXCLUSIVE -- to
 * its atomic cross-thread mirror ONCE per block, the SAME "computed
 * once per block, ~5 ms granularity is plenty for an LED" rationale
 * the classic engine's own g_beat_phase above already uses (see that
 * block's own comment), not a new convention. led_service() (control
 * thread, st_beat_phase.c) reads this mirror -- never g_stem_stream
 * itself -- to derive on-beat phase; this is the ONE clock stem beat-
 * sync is derived from (see st_beat_phase.h's own doc comment on why
 * a loop wrap or a future variable-speed change can never desync a
 * second clock: there is no second clock, only this published copy
 * of the real one, refreshed every block). Unconditional (not gated
 * on stem_active): when no stem song is active this mirror simply
 * holds whatever the transport head's song_frame last was (0 if never
 * selected) -- harmless, since led_service() only reads it behind
 * its own g_stem_song_selected gate. */
/* THE TRANSPORT'S position, not stem 0's -- they are the same number until
 * a track is reversed, and after that this is the one the song's clock is
 * made of. A reversed head must never drag the beat phase backwards. */
atomic_set(&g_stem_song_frame_pub,
	   (atomic_val_t)g_stem_stream[s_stem_transport].song_frame);
#endif
}

#if SP1_XFER_ENABLE
/*
 * ============================ THE STEM TAPE FAST PATH ======================
 *
 * Renders one whole I2S block of stored four-stem playback, and nothing else.
 *
 * WHY IT EXISTS. looper_audio_block() was ~1070 lines of engine inherited from
 * the Tape Looper, and it did not ask whether a stem song was playing until
 * roughly 780 lines in. Before reaching the stem output, EVERY 5.33 ms block
 * unconditionally executed:
 *
 *   - the recorder failsafe scan, provisional auto-confirm and double-tap
 *     delete (three NTRK loops), then ~280 lines of hold-to-record, take
 *     stop/quantize/trim-back, arm/punch and grid-take machinery -- every bit
 *     of it gated on trk[].state being ARMED/REC/DONE, states this firmware
 *     cannot reach because recording is removed;
 *   - the tape-effect speed smoothing and resampler phase update, whose only
 *     consumer is the classic playhead;
 *   - the per-track fader snapshot feeding PASS B;
 *   - PASS A: a 256-ITERATION LOOP writing mix32[]/posb[]/fracb[] -- 2560
 *     bytes of array traffic per block, every value of which is discarded;
 *   - PASS B: per-track accumulation, including a second 256-iteration loop.
 *
 * None of that can affect a single stem sample. On this device it is not
 * merely untidy: the eMMC read path is CPU-bound, so audio-thread cycles
 * convert one-for-one into lost stream throughput, and a short stream is
 * exactly what freezes song_frame and turns a storage deficit into
 * time-stretched, gated audio.
 *
 * The dispatch therefore happens FIRST, in looper_audio_block()'s prologue,
 * and lands here. A bypass -- not another conditional downstream of the
 * expensive passes.
 *
 * ALL this path does: resolve the channel strip once per block, acquire
 * sectors from the SPSC mailbox, decode STSC frames, mix four stems, apply
 * the master-volume ramp, publish per-stem meter peaks, write stereo output,
 * and advance the stem playhead.
 */
/* noinline, noclone: the same reasoning already applied to xfer_do_commit()
 * and friends below. The runtime symbol-presence gate now requires this
 * function by exact name as its link-level evidence of the Stem Tape fast
 * path, and an anchored nm grep does not match a .constprop/.isra clone
 * suffix. This function also has exactly one call site, which is precisely
 * what made looper_audio_block() vanish from the ELF the moment it shrank --
 * so the attribute is what keeps that from silently happening here too. */
/* -O2, and it was missing for the same reason st_planar.c's was: the
 * attribute was applied to the INNER loop (stem_render_run, above) and to the
 * mixer, but not to the function that owns the block. This one runs the
 * residency acquire, the six run bounds, the seam arithmetic, the meter
 * publish and the loop-wrap backstop -- once or twice per 5.333 ms block, on
 * the deadline thread -- and was still being built for size. Pure computation;
 * -O2 can only change how fast it is, not what it produces, and the
 * full-playback gate's 0x2a737e00 hash is the proof. */
__attribute__((optimize("O2"), noinline, noclone))
static void stem_audio_block(int16_t *s, int32_t m0, int32_t md, int32_t mv)
{
	/* Audio-thread-EXCLUSIVE, and now genuinely private to this function:
	 * the ring slot the last successful acquire named, the "I have already
	 * told the mailbox I hold nothing" latch, and a shadow of
	 * underrun_count so the atomic mirror is only touched on a real
	 * change. */
	/* ONE SLOT PER STEM. Four rings, four mailboxes, four acquires -- and
	 * all four name the same span until reverse exists. */
	static uint8_t g_stem_active_slot_local[ST_PL_STEMS];
	static bool s_stem_released;
	static uint32_t s_stem_underrun_shadow;

	/* A WRAP armed against a window that has since been cancelled would
	 * seek into a loop that no longer exists. Drop it and let the gain ramp
	 * back; an ENTER or an EXIT stays valid because both name an absolute
	 * frame that is still correct. */
	if (s_stem_jump_pend == ST_SEAM_JUMP_WRAP &&
	    atomic_get(&g_stem_loop_active) == 0) {
		s_stem_jump_pend = 0u;
	}
	/* BEAT PULSE: largest absolute sample magnitude each stem produced in
	 * THIS block, in the stored domain (ST11_PCM_BIT_DEPTH bits signed).
	 * Block-local until the single publication at the end. */
	uint32_t stem_peak[ST11_STEM_COUNT] = { 0u, 0u, 0u, 0u };

_Static_assert(NTRK == ST11_STEM_COUNT, "trk[]/stem lane count must match 1:1");
st_stem_mix_channel_t stem_channels[ST11_STEM_COUNT];
st_stem_mix_prepared_t stem_prepared;

for (uint32_t s = 0; s < ST11_STEM_COUNT; s++) {
	stem_channels[s].gain_q8 = (int32_t)trk[s].vol_q8;
	stem_channels[s].mute = trk[s].muted != 0;
	stem_channels[s].solo = trk[s].solo != 0;
}
/* ONCE PER BLOCK, never per frame. Fader/mute/solo are
 * control-rate quantities that cannot change inside a block
 * (the control path runs at a lower priority than this thread),
 * so resolving them here collapses the whole channel-strip
 * decision -- the solo scan, the mute test, the gain ceiling --
 * into four integers the 48 kHz loop just multiplies by. That
 * work used to happen inside the mixer on every one of 48000
 * frames a second; on this device that is not spare capacity,
 * because the eMMC read path is CPU-bound and every cycle the
 * audio thread takes is a cycle of read throughput lost. See
 * st_stem_mix.h's own "GAIN CEILING" note for the measurement. */
st_stem_mix_prepare(stem_channels, &stem_prepared);

/* THE FX RACK, ONCE PER BLOCK. Tempo-derived quantities (the echo's 0.375-beat
 * length, the gate's 1/16 cycle) come from the SAME st_beat_timing the LEDs and
 * the loop already use, so nothing here is a second clock. The active mask is
 * the control overlay's momentary|latch, published by stem_ctl_apply(). */
st_fx_prepare(&g_stem_fx, g_stem_beat_timing.frames_per_beat,
	       g_stem_beat_timing.downbeat_frame, s_fx_active_mask);

	/* ==== STORED-SONG PLAYBACK, RENDERED IN RUNS ====
	 *
	 * This used to be a 256-iteration per-frame loop that,
	 * on EVERY frame, re-derived the needed sector (a
	 * division), re-tested residency, published the
	 * requested sector, and -- whenever the sector was
	 * missing -- polled the SPSC mailbox with a barriered
	 * atomic. None of that can change inside a run: a run
	 * never crosses a sector boundary, by construction.
	 * So it was ~48000 repetitions a second of work whose
	 * answer was already known.
	 *
	 * Worse, the starved case was self-reinforcing. With
	 * the stream short, the mailbox said "not ready" on
	 * most frames, and the thread answered by polling it
	 * 48000 times a second while emitting silence --
	 * burning exactly the CPU the streamer needed in order
	 * to make the sector ready. The read path is CPU-bound,
	 * so starvation was feeding itself.
	 *
	 * Now: decide once per run, render the run in a tight
	 * -O2 loop (stem_render_run()), and advance the stream
	 * by the whole run in one call. When the needed sector
	 * genuinely is not there, emit silence for the rest of
	 * the block and stop -- ONE mailbox poll per block, not
	 * one per frame, so a starving stream stops stealing the
	 * CPU that would end the starvation.
	 *
	 * Output is bit-identical to the per-frame form over a
	 * whole real song, host-tested (tests/test_stem_stream.c,
	 * run-form equivalence). */
	uint32_t f = 0;

	/*
	 * ---- THE TRANSPORT RATE FOR THIS BLOCK --------------------------
	 * One rate for the whole block and one for ALL FOUR STEMS, because
	 * there is one playhead: the stems are interleaved in the same
	 * sector and decoded from the same frame index, so they cannot
	 * drift apart by construction. Phase-locked is not a property that
	 * has to be maintained here; it is a property of there being a
	 * single transport.
	 *
	 * TWO THINGS MULTIPLY HERE, and they are different in kind.
	 *
	 *   THE PITCH is the requested rate: the rocker's semitone setting,
	 *   as a 2^(n/12) ratio from st_pitch.h -- the same equal-tempered
	 *   grid the Tape Looper's own semitone control uses. It is a tape
	 *   varispeed, so pitch and time move together; nothing here
	 *   time-stretches and nothing preserves pitch independently.
	 *
	 *   THE INERTIA is an envelope in 0..1 over that. A song pitched to
	 *   0.8x therefore spins up 0 -> 0.8x rather than 0 -> 1.0x, which
	 *   is what makes the two features compose instead of fight. This
	 *   multiply is the "one-line change" the previous revision of this
	 *   comment promised; it is now made.
	 */
	const uint32_t pitch_q16 =
		st_pitch_effective_q16(&s_stem_pitch, s_stem_slow_q16);
	const uint32_t rate_q16 = st_rs_rate_clamp(
		(uint32_t)(((uint64_t)pitch_q16 *
			     st_inertia_env_q16(&s_stem_inertia)) >> 16));
	/*
	 * ---- ONE RATE PER HEAD -------------------------------------------
	 *
	 * The render loop used to take a single rate for the whole block. It
	 * cannot any more: an isolated stem scratch moves ONE head while the
	 * other three keep running forward at the transport's rate, and that
	 * is not expressible as one number.
	 *
	 * Every entry is the transport rate here. Nothing yet writes a
	 * different one -- the gesture that will is step 3 -- so this commit
	 * changes the SHAPE of the quantity and not its value, and ordinary
	 * playback stays bit-identical (the full-playback gate's 0x2a737e00
	 * hash is the proof, not the intent).
	 *
	 * `rate_q16` itself stays, because the things that are genuinely
	 * song-level -- the seam's duck length, the loop's arming arithmetic --
	 * belong to the transport and not to any one head.
	 */
	uint32_t stem_rate_q16[ST_PL_STEMS];

	for (uint32_t rk = 0; rk < ST_PL_STEMS; rk++) {
		stem_rate_q16[rk] = rate_q16;
	}

	/*
	 * ---- THE SCRATCH, APPLIED --------------------------------------
	 *
	 * ONCE PER BLOCK, for the same reason the reverse request is consumed
	 * once per block: a head must not change speed or direction part-way
	 * through a block it is already being rendered for.
	 *
	 * The integrator lives here rather than on the control thread because
	 * the rate has to move on block boundaries -- that is where the render
	 * reads it -- and because this thread has the only exact clock.
	 */
	{
		static st_scratch_t s_scr;
		/*
		 * WHICH HEAD THE GESTURE OWNS, remembered across the coast:
		 * by the time the hand is off, the published target is already
		 * NONE, and the coast still has to be applied to the head that
		 * was being moved rather than to all of them.
		 */
		static uint8_t s_scr_owner = ST_SCR_T_NONE;
		/*
		 * THE DIRECTION THE HEAD WAS IN WHEN THE HAND ARRIVED.
		 *
		 * A stem may have been reverse-toggled before the gesture. The
		 * coast has to return it to THAT, not to forward: coasting to a
		 * positive rate would silently cancel the latch -- the player
		 * reversed a stem, scratched it, let go, and found it playing
		 * forward with nothing to explain it. Position is left where the
		 * scratch put it; direction goes back to the mode the player
		 * chose.
		 */
		static bool s_scr_was_rev;
		const atomic_val_t sv = atomic_get(&g_stem_scratch_req);
		const uint8_t tgt = ST_SCR_TGT(sv);
		const bool live = (tgt != ST_SCR_T_NONE);

		if (live && !s_scr.engaged) {
			/*
			 * THE GRAB. Start from the transport's CURRENT rate,
			 * not from a standstill -- a hand landing on a spinning
			 * record does not stop it dead, and starting at zero
			 * would be a click on every FUNCTION press.
			 *
			 * Direction is folded in here: a head already running
			 * backwards is grabbed at a NEGATIVE rate, so reversing
			 * a reversed stem starts from where it actually is
			 * rather than from its mirror image.
			 */
			const uint32_t k = (tgt == ST_SCR_T_MASTER) ? s_stem_transport : tgt;
			const int32_t signed_now =
				g_stem_stream[k].reverse ? -(int32_t)rate_q16
							  : (int32_t)rate_q16;

			s_scr_was_rev = g_stem_stream[k].reverse;
			st_scratch_begin(&s_scr, signed_now,
					  (tgt == ST_SCR_T_MASTER)
					   ? ST_SCRATCH_MAX_RATE_MASTER_Q16
					   : ST_SCRATCH_MAX_RATE_STEM_Q16);
		} else if (!live && s_scr.engaged) {
			(void)st_scratch_release(&s_scr);   /* begins the coast */
		}

		if (live) {
			st_scratch_set_drive(&s_scr, ST_SCR_DRIVE(sv));
			st_scratch_tick(&s_scr, ST_SCR_BLOCK_US);
		} else {
			/*
			 * COAST TO WHAT THE TRANSPORT WILL ACTUALLY RESUME AT,
			 * not to a hard-coded 1.0x. With the pitch rocker set
			 * the transport runs up to 1.19x, and a coast that
			 * finished at unity would drop the override and step
			 * the rate 0.19x in one block -- a 19% speed jump,
			 * audible as a pitch glitch, on every release while
			 * pitched. Signed, so a latched reverse survives.
			 */
			(void)st_scratch_coast(&s_scr, ST_SCR_BLOCK_US,
						s_scr_was_rev ? -(int32_t)rate_q16
							       : (int32_t)rate_q16);
		}

		if (live) {
			s_scr_owner = tgt;
		}

		if (s_scr.engaged || s_scr.coasting) {
			const int32_t sr = st_scratch_rate_q16(&s_scr);
			const bool want_rev = (sr < 0);
			const uint32_t mag = st_rs_rate_clamp(
				(uint32_t)(want_rev ? -sr : sr));
			uint32_t sk;

			/*
			 * SPLIT THE SIGNED RATE. The transport carries speed
			 * and direction separately -- magnitude in the rate
			 * array, sign in each head's own `reverse` -- so this
			 * is where one signed number becomes the two the engine
			 * already understands. Nothing downstream learns a new
			 * concept.
			 *
			 * st_stream_set_reverse() is the only way the sign is
			 * allowed to change: it moves no head and lifts only
			 * the terminal state the new direction frees, which is
			 * what keeps a direction change from being a seek.
			 */
			for (sk = 0; sk < ST_PL_STEMS; sk++) {
				/* MASTER moves all four; a stem gesture moves
				 * exactly its own and the other three carry on
				 * untouched, which is the whole point of it. */
				if (s_scr_owner != ST_SCR_T_MASTER && sk != s_scr_owner) {
					continue;
				}
				if (g_stem_stream[sk].reverse != want_rev) {
					st_stream_set_reverse(&g_stem_stream[sk], want_rev);
				}
				stem_rate_q16[sk] = mag;
			}
		} else {
			s_scr_owner = ST_SCR_T_NONE;
		}
	}

	/* The seam's duck length converted from output frames into the tape
	 * the playhead covers while it runs. See the arm clamp below. */
	const uint32_t seam_src = st_rs_src_for_out(ST_SEAM_FRAMES, rate_q16);

	/*
	 * ---- BACK ONTO THE 1:1 PATH WHEN THE RAMP ENDS ------------------
	 * A ramp almost never finishes on a whole frame, so the cursor is
	 * left holding a fraction. At exactly 1x that fraction is CONSTANT --
	 * the cursor advances one whole frame per output frame -- so nothing
	 * would ever clear it, and every steady-state frame for the rest of
	 * the session would be an interpolation between two samples instead
	 * of a sample. That is a real, permanent softening of the top end
	 * (linear interpolation at a fixed phase is a mild low-pass), paid
	 * forever for a ramp that ended half a second ago.
	 *
	 * So the cursor is snapped to the frame boundary the moment the reel
	 * reaches nominal speed. The cost is a position step of LESS THAN ONE
	 * SAMPLE, once per PLAY, at the top of a spin-up -- far below the
	 * ramp it is ending, and the price of every other frame in the
	 * session being read at 1:1 exactly as it was before inertia existed.
	 */
	/* BOTH must be at nominal, not just the reel. With the rocker pitched
	 * off unity the cursor legitimately carries a fraction on every block,
	 * and snapping it would be a position jump per block rather than the
	 * once-per-PLAY sub-sample step this is for. */
	if (st_inertia_at_unity(&s_stem_inertia) &&
	    st_pitch_is_unity(&s_stem_pitch) &&
	    s_stem_slow_q16 == ST_PITCH_ONE && !stem_rs_all_aligned()) {
		stem_rs_drop();
	}

	/* The envelope advances on the audio clock, once per block, by the
	 * frames this block will produce. Same clock as the playhead, so the
	 * ramp cannot drift against the audio it is bending. */
	st_inertia_advance(&s_stem_inertia, BLK_FRAMES);
	/* The slow toggle's glide, on the same clock and in the same place, so
	 * the two speed modifiers advance together and neither can lag the
	 * other by a block. */
	s_stem_slow_q16 = st_pitch_slow_glide(s_stem_slow_q16,
					       atomic_get(&g_stem_slow_req) != 0,
					       BLK_FRAMES, I2S_TRUE_HZ);

	/*
	 * ---- THE REVERSE TOGGLE, ONCE PER BLOCK ---------------------------
	 * Consumed here rather than inside the render loop because it is a
	 * transport-shaped decision, not a per-run one, and because doing it
	 * once means the four heads cannot change direction part-way through a
	 * block they are already being rendered for.
	 */
	{
		const atomic_val_t req = atomic_set(&g_stem_reverse_req, 0);

		if (req != 0) {
			const uint32_t k = (uint32_t)(req - 1);

			if (k < ST_PL_STEMS) {
				uint32_t j;
				const bool turning_on = !g_stem_stream[k].reverse;

				/* ONE TRACK AT A TIME, and the spec says what
				 * happens to the outgoing one: "track 2 resumes
				 * forward from wherever it is". Not from where
				 * it started, and not re-synced -- turning a
				 * head forward moves nothing, which is exactly
				 * what st_stream_set_reverse() guarantees. */
				for (j = 0; j < ST_PL_STEMS; j++) {
					const bool want = turning_on && (j == k);

					if (g_stem_stream[j].reverse == want) {
						continue;
					}
					st_stream_set_reverse(&g_stem_stream[j], want);
					/* THE CARRIED "FRAME BEHIND" IS ON THE
					 * WRONG SIDE NOW. Its position did not
					 * move, but the direction of travel did,
					 * so the frame behind the cursor is the
					 * one at the other neighbour. Dropping it
					 * makes the first blend after the turn
					 * start clean; it is not a position
					 * change, and only this head's state is
					 * touched -- the others did not turn. */
					s_rs_prev_valid[j]  = false;
					s_stem_rate_frac[j] = 0u;
				}

				/* THE SONG'S CLOCK MOVES TO A FORWARD HEAD.
				 * There is always one, because at most one track
				 * is reversed -- but "always" is the kind of
				 * claim that stops being true one refactor
				 * later, so the search falls back to leaving the
				 * transport where it is rather than pointing it
				 * at a head running backwards. */
				for (j = 0; j < ST_PL_STEMS; j++) {
					if (!g_stem_stream[j].reverse) {
						s_stem_transport = (uint8_t)j;
						break;
					}
				}
			}
		}
	}

	while (f < BLK_FRAMES) {
		/*
		 * THE TRANSPORT HEAD. Everything that belongs to the SONG rather
		 * than to a track -- the loop window, the seam duck, the beat
		 * phase, the published song frame -- is read from this one. It is
		 * always a head that is still going forward, so a reversed track
		 * cannot drag the song's clock backwards with it.
		 */
		st_stream_t *const tr = &g_stem_stream[s_stem_transport];
		/*
		 * PER-STEM, because a head can now be somewhere else.
		 *
		 * `needed[k]` is the group each head wants, `fis[k]` where it sits
		 * inside that group, `run_k[k]` how many source frames it may take
		 * before it leaves the group. `run` is the MINIMUM of those, which
		 * is the spec's own rule: "out_n is bounded by the minimum ... or
		 * the reversed stem starves while the other three run on."
		 */
		uint32_t needed[ST_PL_STEMS];
		uint32_t fis[ST_PL_STEMS];
		uint32_t run_k[ST_PL_STEMS];
		uint32_t pin_idx[ST_PL_STEMS];
		bool     from_pin[ST_PL_STEMS];
		bool     resident[ST_PL_STEMS];
		/* +1 forward, -1 backward -- and +1 for a head with no group,
		 * which reads zeros and so has no direction of its own. See
		 * stem_render_run()'s own note on why the caller decides. */
		int8_t   dirs[ST_PL_STEMS];
		uint32_t run, left_in_song, out_n;
		uint32_t sk;
		/* Per-stem source consumption, filled by stem_render_run(): the
		 * four streams are advanced by the four numbers the render loop
		 * actually produced rather than by one the caller re-derived. */
		uint32_t stem_used[ST_PL_STEMS];
		/* The loop window, sampled ONCE per iteration. `active` is read
		 * first and the bounds after it, which is the read side of the
		 * publication order described where these atomics are declared:
		 * an active loop is only ever observed with a complete window. */
		bool     lp_on  = atomic_get(&g_stem_loop_active) != 0;
		uint32_t lp_lo  = lp_on ? (uint32_t)atomic_get(&g_stem_loop_start_fr) : 0u;
		uint32_t lp_hi  = lp_on ? (uint32_t)atomic_get(&g_stem_loop_end_fr)   : 0u;
		if (lp_on && lp_hi <= lp_lo) {
			lp_on = false;   /* degenerate window: ignore it entirely */
		}

		/* ---- LOOP ENTRY: THE TRANSPORT IS NOT TOUCHED -------------
		 * Engaging the loop sets a boundary. It does not move the
		 * playhead, at all.
		 *
		 * This REPLACES an entry seek back to the captured frame, which
		 * is the defect the product ruling names directly. loop_start is
		 * captured at PLAY-DOWN, so by the time the hold threshold makes
		 * it a loop the playhead is a whole hold past it -- and seeking
		 * back replayed every sample in between. On a vocal reading
		 * "for me no", engaging during "me" produced "for me - me no":
		 * the syllable audibly restarted. The SP-1 does not do that.
		 * Loop state and transport position are separate concerns, and
		 * only a boundary CROSSING may move the playhead.
		 *
		 * So the request is consumed and nothing else happens. The
		 * window is already published (stem_ctl_apply set the bounds
		 * before `active`), playback continues sample-for-sample from
		 * wherever it is, and the first thing the player hears is
		 * simply the song carrying on. The wrap below does the rest. */
		(void)atomic_cas(&g_stem_loop_enter_req, 1, 0);

		/* ---- LOOP EXIT, taken before anything else in the block ----
		 * A one-shot request from the control thread. Consumed with an
		 * atomic clear so a single PLAY gesture causes exactly one seek
		 * no matter how many blocks pass before it is noticed.
		 *
		 * The seek lands on the CAPTURED frame, and st_stream_seek()
		 * invalidates residency so the very next statement re-acquires
		 * -- from the pin, which is exactly why the pin exists. No
		 * pause, no stop/start, no silent block: the remainder of THIS
		 * block is filled with forward playback from loop_end_frame.
		 *
		 * A release can arrive while a wrap's duck-in is still running.
		 * st_seam_begin() resumes from wherever the gain already is
		 * rather than snapping to unity, so the two transitions do not
		 * produce a step between them -- and because the jump waits for
		 * st_seam_jump_due() rather than counting frames, it still
		 * lands at zero gain. Counting was measured 3.4x WORSE than no
		 * ducker at all for exactly this case.
		 */
		if (atomic_cas(&g_stem_loop_exit_req, 1, 0)) {
			/* THE TRANSPORT IS NOT TOUCHED HERE EITHER. Releasing
			 * stops future wrapping and nothing more: the iteration
			 * already in flight plays on through the loop end and
			 * into the material that follows, which is how playback
			 * rejoins the song without a discontinuity.
			 *
			 * stem_ctl_apply() has already cleared g_stem_loop_active,
			 * so lp_on is false and the wrap arm below can no longer
			 * fire. That is the entire exit.
			 *
			 * THIS REVERSES A DOCUMENTED EARLIER DECISION, and the
			 * reversal is deliberate: st_loop.h's "EXIT POSITION"
			 * section resumes at loop_end so nothing already heard
			 * repeats. The product ruling is that a release must not
			 * move the playhead under any circumstances, which is
			 * incompatible with that, and it wins. The accepted
			 * consequence is that releasing part-way through an
			 * iteration lets the rest of the looped section play once
			 * more before the song moves on -- organically, with no
			 * seam, which is the point.
			 *
			 * A WRAP MAY ALREADY BE DUCKING. Its duck arms
			 * ST_SEAM_FRAMES before the boundary, so a release can
			 * land inside it. That wrap must not fire -- it would be
			 * a jump caused by releasing -- but the gain cannot snap
			 * back either. st_seam_cancel() reverses the ramp at the
			 * gain it has already reached. */
			if (s_stem_jump_pend == ST_SEAM_JUMP_WRAP) {
				s_stem_jump_pend = 0u;
				st_seam_cancel(&s_stem_seam);
			}
			atomic_inc(&g_stem_loop_exits);
		}

		/* ---- THE BOUNDARY THE PLAYHEAD IS ACTUALLY APPROACHING ----
		 * WITH NO ENTRY SEEK, THE PLAYHEAD CAN START OUTSIDE THE
		 * WINDOW, and that has to be handled or the loop silently never
		 * engages.
		 *
		 * loop_start is captured at PLAY-DOWN and the loop only becomes
		 * a loop once the hold threshold expires, so the playhead is
		 * always some hundreds of ms past loop_start by then -- and if
		 * the chosen division is SHORTER than that hold, it is already
		 * past loop_end too. A test of "song_frame < lp_hi" would then
		 * be false forever and nothing would ever wrap.
		 *
		 * The window is treated as PERIODIC from loop_start: the
		 * boundary being approached is the first lp_lo + n*len strictly
		 * above the playhead. That keeps the first lap on the musical
		 * grid -- it ends a whole number of loop lengths after the
		 * capture, never at an arbitrary offset -- and after the first
		 * wrap the playhead is at lp_lo, so every later lap is exactly
		 * one length. One division per block, not per frame. */
		uint32_t lp_end = lp_hi;

		if (lp_on) {
			const uint32_t len = lp_hi - lp_lo;
			const uint32_t pos = tr->song_frame;

			if (pos >= lp_hi) {
				lp_end = lp_lo + ((pos - lp_lo) / len + 1u) * len;
			}
		}

		/* ---- THE WRAP, ARMED BEFORE THE BOUNDARY ------------------
		 * The wrap is now the ONLY transition that moves the transport,
		 * and it is the one the contract explicitly allows: the
		 * playhead crossing loopEnd. Its duck starts ST_SEAM_FRAMES
		 * ahead of the boundary and the jump lands exactly ON it.
		 * Arming at the boundary instead leaves the gain near unity
		 * when the jump happens and removes almost none of the step --
		 * measured on the frozen fixture: 8753 armed at the boundary,
		 * 176 armed ahead of it. The run clamp further down guarantees
		 * the playhead lands on this frame rather than stepping over
		 * it. */
		if (lp_on && !s_stem_jump_pend &&
		    tr->song_frame >= lp_lo &&
		    tr->song_frame < lp_end &&
		    tr->song_frame + ST_SEAM_FRAMES >= lp_end) {
			s_stem_jump_to   = lp_lo;
			s_stem_jump_pend = ST_SEAM_JUMP_WRAP;
			s_stem_seam_lo   = lp_lo;
			s_stem_seam_hi   = lp_end;
			st_seam_begin_in(&s_stem_seam,
					  (uint16_t)(lp_end - tr->song_frame));
		}

		/* ---- THE JUMP, on the frame the gain actually reached zero -
		 * Never after a fixed count. This is the whole contract of
		 * st_seam.h and the one place it is honoured. WRAP is the only
		 * kind that reaches here: entry and release no longer move the
		 * transport at all. */
		if (s_stem_jump_pend && st_seam_jump_due(&s_stem_seam)) {
			/* The playhead is MOVED, not advanced: the frame
			 * behind the cursor is now a frame from somewhere
			 * else in the song, and blending across that join is
			 * the discontinuity the duck exists to prevent. */
			stem_rs_drop();
			/* EVERY HEAD THAT IS WHERE THE TRANSPORT IS. The duck
			 * was armed off the transport's approach to loop_end,
			 * and every head standing on that same frame is
			 * crossing the same boundary at the same instant, so
			 * they wrap together and the one duck covers all of
			 * them. A head that has DRIFTED is somewhere else and
			 * has not reached the boundary yet -- moving it here
			 * would be a jump it did not earn, and would re-sync
			 * exactly the divergence per-track reverse exists to
			 * create. It wraps on its own crossing, in the
			 * backstop below. */
			for (sk = 0; sk < ST_PL_STEMS; sk++) {
				if (g_stem_stream[sk].song_frame != tr->song_frame) {
					continue;
				}
				if (st_stream_seek(&g_stem_stream[sk], s_stem_jump_to) &&
				    sk == s_stem_transport) {
					atomic_inc(&g_stem_loop_wraps);
				}
			}
			s_stem_jump_pend = 0u;
		}

		for (sk = 0; sk < ST_PL_STEMS; sk++) {
			needed[sk] = st_stream_required_sector(&g_stem_stream[sk]);
			from_pin[sk] = false;
			pin_idx[sk] = 0u;
		}

		/* ---- RESIDENCY, PER STEM: the pin is consulted FIRST -------
		 * The pinned sectors are the loop's exit kit; right after a
		 * seek back to loop_start_frame they are the only place those
		 * bytes exist. Checking the pin before the ring also keeps the
		 * ring's own acquire from being spent on a sector the pin
		 * already holds.
		 *
		 * PER STEM, NOT ALL-FOUR-OR-NONE ACROSS STEMS. It stays
		 * all-or-none WITHIN a stem -- a group is either resident or it
		 * is not -- but the four heads may now want four different
		 * groups, and the old shared check would have silenced all four
		 * for whichever one was late. The spec names this directly: "a
		 * stem whose group is not resident is the one that underruns".
		 *
		 * While the heads are together (the ordinary case, and the only
		 * case before a track is reversed) all four ask for the same
		 * group and the four mailboxes fill in lockstep, so this is
		 * four hits or four misses exactly as it was. */
		{
			uint32_t acquired_slot;
			uint32_t got = 0u;

			for (sk = 0; sk < ST_PL_STEMS; sk++) {
				if (stem_loop_pin_lookup(needed[sk], &pin_idx[sk])) {
					from_pin[sk] = true;
					st_stream_sector_ready(&g_stem_stream[sk], needed[sk]);
				} else if (g_stem_stream[sk].ready_sector != needed[sk]) {
					/* Ask the mailbox (st_stem_bufmbox.h)
					 * whether the producer has published it
					 * since we last looked. Wait-free: one
					 * acquire load, and on success one
					 * release store. */
					if (st_stem_mbox_try_acquire(&g_stem_mbox[sk], needed[sk],
								      &acquired_slot)) {
						g_stem_active_slot_local[sk] = (uint8_t)acquired_slot;
						st_stream_sector_ready(&g_stem_stream[sk], needed[sk]);
					} else {
						/* WHICH STEM WAS NOT THERE. The
						 * global underrun counters say a
						 * span was not playable; only this
						 * says whose group was missing,
						 * which is the whole question when
						 * one stem drops out and the other
						 * three keep playing. */
						(void)atomic_add(&g_stem_miss[sk], 1);
					}
				}
				resident[sk] = (g_stem_stream[sk].ready_sector == needed[sk]);
				if (resident[sk]) {
					got++;
				}
			}
			if (got == ST_PL_STEMS) {
				s_stem_released = false;   /* holding buffers again */
			} else if (got == 0u && !s_stem_released) {
				/* Genuinely reading nothing -- not one stem of
				 * four, but none of them: say so, so the
				 * producer may fill every slot including the
				 * one the previously-held sector maps to. See
				 * st_stem_mbox_release()'s own doc comment for
				 * the loop-wrap stall this prevents. Latched,
				 * so it happens once per starvation episode.
				 *
				 * Releasing while ANY stem is still reading its
				 * group would tell the producer it may refill a
				 * buffer this thread is decoding out of, which
				 * is the one thing the release protocol exists
				 * to prevent. */
				for (sk = 0; sk < ST_PL_STEMS; sk++) {
					st_stem_mbox_release(&g_stem_mbox[sk]);
				}
				s_stem_released = true;
			}
		}

		/* THE PREFETCH LOOKAHEAD. Once the current sector is
		 * resident, ask for the one AFTER it, so the producer
		 * fills an idle slot during the ~7.08 ms this sector
		 * plays for -- that is what makes the ring actually
		 * read ahead. While the current sector is NOT resident
		 * (a real underrun, or the first tick after a seek or
		 * reload) keep asking for IT: fetching further ahead
		 * then would strand the consumer waiting on a sector
		 * nobody is fetching. */
		for (sk = 0; sk < ST_PL_STEMS; sk++) {
			uint32_t want = needed[sk];

			if (resident[sk]) {
				/* THE DIRECTION THE HEAD IS ACTUALLY GOING. A forward
				 * head wants the group after its own; a reversed head
				 * wants the one before it, and asking for the one after
				 * would prefetch tape it has already played while the
				 * group it is about to need went unfetched. This is the
				 * whole of what reverse costs the read path: one sign,
				 * on an address. */
				uint32_t ahead;

				if (g_stem_stream[sk].reverse) {
					/* A REVERSED HEAD PUBLISHES THE BASE OF ITS
					 * BATCH, NOT ITS NEXT GROUP.
					 *
					 * The producer's fill is always an ASCENDING
					 * run of ST_PL_REFILL_GROUPS starting at what
					 * the consumer requested -- st_planar.h says so
					 * directly: "a reversed stem covers groups
					 * [g-N+1, g] and is read as ONE ASCENDING
					 * block". Publishing needed-1 would make that
					 * run [needed-1, needed-1+R): one useful group
					 * and R-1 refetches of groups this head has
					 * already played, which is three times the read
					 * traffic for one reversed track.
					 *
					 * Publishing needed-R makes the SAME ascending
					 * read cover exactly the R groups this head is
					 * about to need, in the order storage wants
					 * them, at exactly the cost a forward head
					 * pays. Storage sees no difference between a
					 * forward stem and a reversed one; only the
					 * address moved.
					 *
					 * Before the start of the song there is nothing
					 * to fetch and nothing to wrap to either -- a
					 * reversed head clamps at frame 0 rather than
					 * wrapping (st_stem_stream.h, START_OF_SONG) --
					 * so the base is clamped rather than allowed to
					 * wrap around through zero. */
					ahead = (needed[sk] >= ST_PL_REFILL_GROUPS)
						? needed[sk] - ST_PL_REFILL_GROUPS
						: 0u;
				} else {
					ahead = needed[sk] + 1u;
					if (ahead >= ST_STEM_GEOM.sector_count) {
						/* End of song: wrap only if this song
						 * loops; otherwise there is nothing
						 * further to fetch and re-publishing
						 * `needed` is a no-op. */
						ahead = ST_STEM_GEOM.loop_enabled ? 0u : needed[sk];
					}
				}
				/* READ-AHEAD STAYS IN SONG ORDER, EVEN INSIDE A LOOP,
				 * and that is deliberate.
				 *
				 * The obvious-looking optimisation -- point the producer
				 * at the loop's start sector as the window end
				 * approaches, so the wrap arrives resident -- is WRONG
				 * on this ring, for the same reason st_stem_bufmbox.c's
				 * scan refuses to wrap at the song seam: sector s and
				 * sector s+SLOTS map to the SAME slot. Inside a loop
				 * both of those are live sectors, so prefetching the
				 * post-wrap region evicts the pre-wrap region the
				 * playhead has not reached yet.
				 *
				 * That is not hypothetical. With a 1/4-beat loop
				 * (sectors 3..20) an earlier version of this code did
				 * exactly that, and tests/test_loop_playback_gate.c
				 * caught the consequence directly: "needs sector 18,
				 * slot 6 holds sector 6" -- 18 % 12 == 6, the producer
				 * had filled the post-wrap sector into the slot the
				 * playhead was about to read. The loop starved and
				 * emitted silence.
				 *
				 * The wrap is covered by the PINNED sectors instead,
				 * exactly as the exit is; the pin lives outside the
				 * ring and so cannot collide with anything. */
				want = ahead;
			}
			st_stem_mbox_set_requested_sector(&g_stem_mbox[sk], want);
		}

		/*
		 * WHOSE UNDERRUN IS IT? Heads that share a position are
		 * logically ONE head and must starve together: before a track
		 * is reversed all four are co-located, and letting three
		 * advance while the fourth froze would desync them permanently
		 * for a jitter the ring is meant to absorb. A head that is
		 * somewhere ELSE is genuinely independent -- that is what
		 * reverse made it -- so it starves alone, goes silent, and the
		 * rest of the block plays.
		 *
		 * The transport is trivially co-located with itself, so a
		 * transport miss is always a whole-block underrun, exactly as
		 * it was.
		 */
		{
			bool block_underrun = false;

			for (sk = 0; sk < ST_PL_STEMS; sk++) {
				if (!resident[sk] &&
				    g_stem_stream[sk].song_frame == tr->song_frame) {
					block_underrun = true;
					break;
				}
			}
			if (block_underrun) {
			/* UNDERRUN: silence for the remainder of the
			 * block, then stop polling until the next block. */
			/*
			 * HOW MUCH SILENCE, not how many episodes.
			 *
			 * g_stem_underrun_count records the TRANSITION into
			 * underrun and nothing about its length, so one stalled
			 * frame and one stalled block both count as exactly 1.
			 * Read as a dropout count it is off by up to 256x, and
			 * it was: a run that sounded perfect reported 384
			 * "underruns" -- somewhere between 8 ms of inaudible
			 * silence across 20 seconds and 2 full seconds, with no
			 * way to tell which.
			 *
			 * Frames silenced is the quantity that decides whether
			 * anyone can hear it. Audio-thread-exclusive increment,
			 * atomic only so a diagnostic reader never tears it.
			 */
			(void)atomic_add(&g_stem_underrun_frames,
					  (atomic_val_t)(BLK_FRAMES - f));
			st_seam_advance(&s_stem_seam, BLK_FRAMES - f);
			for (; f < BLK_FRAMES; f++) {
				s[2 * f]     = 0;
				s[2 * f + 1] = 0;
			}
			/* Records the underrun EPISODE (once on the
			 * transition, not once per stuck frame) and leaves
			 * song_frame frozen -- the same accounting the
			 * per-frame form did. Every head: the ones that are
			 * resident are not advanced either, because the block
			 * they would have advanced through was not rendered. */
			for (sk = 0; sk < ST_PL_STEMS; sk++) {
				(void)st_stream_advance_frames(&g_stem_stream[sk], 1u);
			}
			break;
			}
		}

		/* THE RUN, NOW IN TWO DOMAINS.
		 *
		 * Until inertia there was only one: output frames and
		 * source frames were the same number, so a single set of
		 * clamps served both. A transport spinning up or down
		 * reads the tape more slowly than it fills the output,
		 * and the six bounds split accordingly:
		 *
		 *   SOURCE  sector, song, loop end, seam arm
		 *   OUTPUT  this output block, seam jump
		 *
		 * `run` is the SOURCE bound -- as many frames as lie
		 * simultaneously inside this sector, inside the song and
		 * inside the loop window. That is what stem_render_run()
		 * may decode out of `buf` and what
		 * st_stream_advance_frames() may be advanced by. The two
		 * OUTPUT bounds are applied below to `out_n`, once the
		 * rate has converted between the domains. At 1x that
		 * conversion is the identity and every clamp lands on
		 * exactly the value it did before. */
		/* ---- PER-HEAD SOURCE BOUNDS, then the MINIMUM --------------
		 * Each head has its own offset inside its own group and its own
		 * direction, so each has its own frames-remaining. The block can
		 * only render as many as the WORST-SUPPLIED head can feed --
		 * rendering more would run some head past the end of the group
		 * it holds, which is reading another sector's bytes as if they
		 * were this one's.
		 *
		 * A head that is not resident is SILENT this run (it decodes out
		 * of the all-zero group below) and does not bound anything: it
		 * is not reading real bytes, so there is nothing to run off the
		 * end of.
		 *
		 * FORWARD a head at offset fis may take (FRAMES_PER_SECTOR -
		 * fis) and lands on the next group's first frame; BACKWARD it
		 * may take (fis + 1) -- frames fis down to 0 -- and lands on the
		 * previous group's last one. Same rule, mirrored, and it is the
		 * same one st_stream_advance_frames() states as its own
		 * precondition. */
		/* No head's run can exceed the group it is reading, so that is
		 * the identity for a minimum -- not BLK_FRAMES, which would cap
		 * `run` below what a single pass can legitimately consume and
		 * split one iteration into several at pitched-up rates, on
		 * exactly the path that has the least CPU to spare. */
		{
		/* The transport's own offset inside its own group. It is always
		 * resident here -- a transport miss is a whole-block underrun,
		 * handled above -- and it is what a silent head borrows. */
		const uint32_t fis_tr = tr->song_frame -
					needed[s_stem_transport] * ST11_FRAMES_PER_SECTOR;

		run = ST11_FRAMES_PER_SECTOR;
		for (sk = 0; sk < ST_PL_STEMS; sk++) {
			uint32_t rk;
			const uint32_t pos_k = g_stem_stream[sk].song_frame;

			if (!resident[sk]) {
				/* SILENT THIS RUN. It reads the all-zero group,
				 * where every in-bounds index yields zero, so it
				 * is given the transport's own offset and a
				 * forward direction: that keeps every index in
				 * bounds without a second bounds rule, and keeps
				 * the four indices EQUAL, which is what lets the
				 * three heads that are reading real bytes stay
				 * on the fast path. It bounds nothing -- there
				 * are no real bytes to run off the end of. */
				fis[sk] = fis_tr;
				dirs[sk] = 1;
				/* Bounded so its own index cannot leave the
				 * group even in st_rs_out_frames()'s floored
				 * corner, and never tighter than the transport's
				 * own sector bound, so it constrains nothing. */
				run_k[sk] = ST11_FRAMES_PER_SECTOR - fis_tr;
				continue;
			}
			fis[sk] = pos_k - needed[sk] * ST11_FRAMES_PER_SECTOR;
			dirs[sk] = g_stem_stream[sk].reverse ? -1 : 1;
			if (g_stem_stream[sk].reverse) {
				rk = fis[sk] + 1u;
				/* The song's own front edge. A reversed head
				 * clamps at frame 0 and never wraps, so the run
				 * may reach it but not pass it. */
				if (rk > pos_k + 1u) {
					rk = pos_k + 1u;
				}
			} else {
				rk = ST11_FRAMES_PER_SECTOR - fis[sk];
				left_in_song = ST_STEM_GEOM.frames - pos_k;
				if (rk > left_in_song) {
					rk = left_in_song;
				}
			}
			/* ---- THE LOOP WINDOW, IN THIS HEAD'S OWN DIRECTION -
			 * The run must stop exactly ON the boundary this head
			 * is approaching, so the wrap lands on a frame boundary
			 * rather than somewhere inside a rendered run: the last
			 * frame rendered is the boundary's neighbour and the
			 * next one rendered is across it, so no frame is skipped
			 * and none is played twice.
			 *
			 * Forward that boundary is loop_end; backward it is
			 * loop_start, and the head wraps to loop_end and keeps
			 * going backward (the spec: "on reaching loop_start it
			 * wraps to loop_end"). */
			if (lp_on) {
				if (g_stem_stream[sk].reverse) {
					if (pos_k >= lp_lo && pos_k < lp_end &&
					    rk > pos_k - lp_lo + 1u) {
						rk = pos_k - lp_lo + 1u;
					}
				} else if (pos_k >= lp_lo && pos_k < lp_end &&
					   rk > lp_end - pos_k) {
					rk = lp_end - pos_k;
				}
			}
			run_k[sk] = rk;
			if (rk < run) {
				run = rk;
			}
		}
		}
		/* ---- A FIFTH BOUND, TRANSPORT-ONLY: land ON the arming frame
		 * The window clamps above are per-head; this one is not, and
		 * that is deliberate. The duck is a gain on the whole MIX, so
		 * there is one of it, and it belongs to the head the song's
		 * clock is made of. A drifted head's own wrap is covered by the
		 * backstop below. */
		if (lp_on && tr->song_frame >= lp_lo &&
		    tr->song_frame < lp_end) {
			/*
			 * The wrap's duck has to start exactly ST_SEAM_FRAMES
			 * before loop_end. A run is up to a whole block long,
			 * so without this clamp the playhead can step straight
			 * over that frame and arrive with the arm test never
			 * having been true at the right moment -- the duck
			 * would then be short, or the backstop would fire an
			 * un-ducked jump. Clamping here costs one extra loop
			 * iteration per lap and nothing else.
			 *
			 * ST_SEAM_FRAMES is a duck length in OUTPUT frames and
			 * this compares SOURCE positions, so the distance is
			 * converted at the current rate. Below 1x the playhead
			 * covers less tape during the duck, so the arm point
			 * sits nearer the end; leaving it unconverted would
			 * start the duck early, finish it early, and fire the
			 * wrap before the playhead ever reached loop_end. At 1x
			 * the conversion returns ST_SEAM_FRAMES unchanged. */
			if (!s_stem_jump_pend && !tr->reverse &&
			    tr->song_frame + seam_src < lp_end) {
				uint32_t to_arm = lp_end - seam_src -
						  tr->song_frame;

				if (run > to_arm) {
					run = to_arm;
				}
			}
		}
		if (run == 0u) {
			/* Defensive: cannot happen while the stream is
			 * PLAYING (song_frame < frames is the invariant
			 * st_stream_advance_frames() maintains), but a
			 * zero-length run would spin this loop forever, so
			 * never take that on trust in a real-time thread. */
			st_seam_advance(&s_stem_seam, BLK_FRAMES - f);
			for (; f < BLK_FRAMES; f++) {
				s[2 * f]     = 0;
				s[2 * f + 1] = 0;
			}
			break;
		}

		/* ---- SOURCE FRAMES -> OUTPUT FRAMES ----------------------
		 * The run is bounded in the source domain above. How many
		 * output slots it fills depends on the rate: at 1x it is the
		 * same number and everything below reduces to what shipped;
		 * during a ramp it is more, because the tape is passing more
		 * slowly than the output is being produced. */
		/* SIZED BY THE HEAD THAT RUNS OUT OF SOURCE FIRST.
		 *
		 * That used to be found by pairing `run` with the largest
		 * carried fraction, since a bigger fraction crosses a source
		 * frame sooner. With one rate for the whole block that was
		 * exactly right. It is not any more: rate now dominates the
		 * fraction, so a head at 4x exhausts the run in a quarter of
		 * the output frames a head at 1x does, whatever fraction each
		 * is carrying. Asking with the worst fraction and one rate
		 * would over-produce for the fastest head and read past the
		 * source it actually has. */
		{
			/*
			 * THE OUTPUT COUNT IS THE MINIMUM OVER THE HEADS, and
			 * with per-stem rates that is no longer the same as
			 * asking once with the worst fraction.
			 *
			 * `run` source frames buy a different number of output
			 * frames at each head's own rate: a stem at 4x exhausts
			 * the run in a quarter of the frames a stem at 1x does.
			 * Producing more than the slowest-supplied head can
			 * cover would read past the source it actually has --
			 * so every head is asked, and the smallest answer wins.
			 *
			 * At a shared rate every term is identical and this is
			 * exactly the single call it replaces, which is what
			 * keeps ordinary playback bit-identical.
			 */
			out_n = st_rs_out_frames(run, s_stem_rate_frac[0], stem_rate_q16[0]);
			for (sk = 1; sk < ST_PL_STEMS; sk++) {
				const uint32_t n_k = st_rs_out_frames(run, s_stem_rate_frac[sk],
								       stem_rate_q16[sk]);

				if (n_k < out_n) {
					out_n = n_k;
				}
			}
		}

		/* ---- A FIFTH BOUND, IN THE OUTPUT DOMAIN: this block ----- */
		if (out_n > BLK_FRAMES - f) {
			out_n = BLK_FRAMES - f;
		}
		/* ---- A SIXTH BOUND: land ON the frame the jump is due ----
		 * Same argument as the arm clamp, for the entry and the
		 * release: the seek must happen on the frame whose gain is
		 * zero, so the run may not carry the playhead past it.
		 * st_seam_frames_to_jump() is UINT16_MAX when nothing is
		 * pending, so ordinary playback is never shortened by this.
		 *
		 * This one is naturally an OUTPUT bound and always was: the
		 * seam ticks once per rendered frame, not once per frame of
		 * tape. It is applied here rather than to `run` for exactly
		 * that reason. */
		if (s_stem_jump_pend) {
			uint32_t to_jump = st_seam_frames_to_jump(&s_stem_seam);

			if (to_jump > 0u && out_n > to_jump) {
				out_n = to_jump;
			}
		}
		if (out_n == 0u) {
			/* Same defence as the zero-length source run above.
			 * st_rs_out_frames() never returns zero, so this can
			 * only come from a clamp, and a clamp to zero means
			 * the block is full -- but a real-time loop does not
			 * get to assume that. */
			st_seam_advance(&s_stem_seam, BLK_FRAMES - f);
			for (; f < BLK_FRAMES; f++) {
				s[2 * f]     = 0;
				s[2 * f + 1] = 0;
			}
			break;
		}

		/* RAM-ONLY: decodes out of whichever resident buffer
		 * g_stem_active_slot_local[] (audio-thread-exclusive)
		 * names -- this real-time thread never touches flash. */
		{
		const uint8_t *grp[ST_PL_STEMS];
		uint32_t gk;

		/* FOUR POINTERS, ONE PER STEM, each resolved from ITS OWN
		 * residency: the pin if that head's span is pinned, its own ring
		 * otherwise -- and the all-zero group if it has nothing, which
		 * is how one starved head plays silence while the other three
		 * play on WITHOUT a branch anywhere in the 48 kHz loop. Decoding
		 * zeros is silence; there is no special case to get wrong and no
		 * per-frame test to pay for. */
		for (gk = 0; gk < ST_PL_STEMS; gk++) {
			grp[gk] = !resident[gk]
				  ? g_stem_silent_group
				  : (from_pin[gk]
				     ? g_stem_loop_pin_bufs[gk][pin_idx[gk]]
				     : g_stem_group_bufs[gk][g_stem_active_slot_local[gk]]);
		}
		stem_render_run(grp, fis, &stem_prepared, m0, md, mv,
				 f, out_n, s, stem_peak, &s_stem_seam,
				 s_stem_transport,
				 stem_rate_q16, s_stem_rate_frac, run_k,
				 stem_used, g_stem_stream, dirs);
		f += out_n;
		/* THE PLAYHEAD ADVANCES BY WHAT WAS ACTUALLY READ, which is
		 * `out_n * rate` rounded down to whole frames -- the
		 * fractional remainder stays in s_stem_rate_frac and is
		 * carried into the next run, so nothing is lost or repeated
		 * across a run boundary. At 1x this is out_n exactly. */
		}
		/* EACH HEAD BY ITS OWN COUNT, in its own direction. A head
		 * that is not resident is advanced too, and freezes: its
		 * ready_sector still does not match what it needs, so
		 * st_stream_advance_frames() reports UNDERRUN, counts the
		 * episode once and leaves song_frame exactly where it is --
		 * which is the same accounting a whole-block underrun gets, and
		 * needs no separate case here. */
		for (sk = 0; sk < ST_PL_STEMS; sk++) {
			st_stream_tick_t tk =
				st_stream_advance_frames(&g_stem_stream[sk], stem_used[sk]);

			/* A REVERSED HEAD THAT REACHED THE FRONT OF THE SONG
			 * parks there, and its resampler's carried "frame
			 * behind" belongs to a position it is no longer moving
			 * away from. Dropped so that turning it forward again
			 * starts a clean blend rather than one across the park.
			 */
			if (tk == ST_STREAM_TICK_START_REACHED) {
				s_rs_prev_valid[sk]  = false;
				s_stem_rate_frac[sk] = 0u;
			}
		}

		/* ---- THE WRAP BACKSTOP ------------------------------------
		 * The ducked wrap above is the normal path, and after it the
		 * playhead reaches loop_end only at zero gain. Two cases still
		 * arrive here.
		 *
		 * A LENGTH CHANGE that moves loop_end behind the playhead
		 * between one iteration and the next: the duck cannot have been
		 * armed for a boundary that did not exist yet. Jumping
		 * un-ducked is a click -- but leaving the playhead outside the
		 * window would unroll the loop into the following material,
		 * which is worse, so the window wins.
		 *
		 * A RELEASE whose duck is still running when the playhead
		 * reaches loop_end. The release lands ON loop_end, so playing
		 * past it here and then jumping back would sound those same
		 * frames TWICE. Wrapping instead keeps the outgoing audio
		 * inside the loop it is leaving, and the step costs nothing
		 * because the gain is already most of the way down. A pending
		 * WRAP is the one kind NOT taken here: it is already on its way
		 * to loop_start, ducked, and a second jump would be the click
		 * the duck exists to remove.
		 *
		 * st_stream_seek() invalidates residency, so the next iteration
		 * re-acquires -- from the pin, which holds precisely the
		 * sectors at loop_start_frame. That is the second reason the
		 * pin exists: without it every wrap would race the streamer. */
		if (lp_on && lp_end > lp_lo) {
			for (sk = 0; sk < ST_PL_STEMS; sk++) {
				st_stream_t *const hd = &g_stem_stream[sk];
				bool wrapped = false;

				if (hd->reverse) {
					/* BACKWARD: the boundary is loop_START,
					 * and crossing it wraps to loop_END --
					 * the spec's own rule, and the mirror of
					 * the forward case rather than a second
					 * mechanism. A head that has retreated
					 * ONTO lp_lo has not crossed it yet; the
					 * run clamp above lets it reach lp_lo
					 * exactly, and the crossing is the step
					 * that would take it below. */
					if (hd->song_frame < lp_lo && lp_end > 0u) {
						wrapped = st_stream_seek(hd, lp_end - 1u);
					}
				} else if (sk == s_stem_transport &&
					   s_stem_jump_pend == ST_SEAM_JUMP_WRAP) {
					/* The transport's ducked wrap is already
					 * on its way; a second jump here would be
					 * the click the duck exists to remove. */
					continue;
				} else if (hd->song_frame >= lp_end) {
					wrapped = st_stream_seek(hd, lp_lo);
				}
				if (wrapped) {
					/* MOVED, not advanced: this head's
					 * "frame behind the cursor" is now a
					 * frame from somewhere else, and
					 * blending across that join is exactly
					 * the discontinuity a wrap must not
					 * cause. Only this head's carried state
					 * is dropped -- the other three did not
					 * move. */
					s_rs_prev_valid[sk]  = false;
					s_stem_rate_frac[sk] = 0u;
					if (sk == s_stem_transport) {
						atomic_inc(&g_stem_loop_wraps);
					}
				}
			}
		}

		/* Mirror the (audio-thread-exclusive) underrun episode
		 * counter into its atomic diagnostic twin, but only on
		 * the rare pass it actually changed -- atomic counters
		 * are for cross-thread OBSERVABILITY, not a hot-path
		 * cost. */
		/* THE WORST HEAD'S count, not stem 0's. This is a dropout
		 * diagnostic, and "some head starved" is the fact it is meant
		 * to report -- reading only one head would hide exactly the
		 * per-track starvation the per-stem residency above makes
		 * possible. g_stem_miss[] still says WHICH. */
		if (stem_underrun_worst() != s_stem_underrun_shadow) {
			s_stem_underrun_shadow = stem_underrun_worst();
			atomic_set(&g_stem_underrun_count, (atomic_val_t)s_stem_underrun_shadow);
		}
	}
	if (stem_underrun_worst() != s_stem_underrun_shadow) {
		s_stem_underrun_shadow = stem_underrun_worst();
		atomic_set(&g_stem_underrun_count, (atomic_val_t)s_stem_underrun_shadow);
	}

	/*
	 * PEAK HOLD, not last-block-wins. One atomic touch per stem per BLOCK
	 * (~5.3 ms), never per frame.
	 *
	 * The control thread services the LEDs roughly every 8-15 ms, so it
	 * reads this slower than the audio thread writes it. A plain store
	 * would mean the peaks of every block but the last one before each
	 * read are simply discarded -- and a drum transient that lands in a
	 * discarded block never reaches the light at all. Since the display is
	 * meant to show exactly those transients, the value published here is
	 * the LARGEST peak since the reader last took it, and the reader
	 * CLEARS it as it reads (atomic_set returns the old value, so that is
	 * one atomic exchange, not a read followed by a racy store).
	 *
	 * The merge below is a plain load/compare/store rather than a
	 * compare-and-swap loop, and that is deliberate. Only this thread ever
	 * raises the value and only the reader ever lowers it, so the single
	 * losable interleaving -- reader clears between this load and this
	 * store -- re-publishes a peak that was just reported. One LED frame
	 * holds a level for ~10 ms longer than it strictly should, which is
	 * invisible, and it costs a real-time thread nothing.
	 */
	for (uint32_t sp = 0; sp < ST11_STEM_COUNT; sp++) {
		if ((uint32_t)atomic_get(&g_stem_peak_pub[sp]) < stem_peak[sp]) {
			atomic_set(&g_stem_peak_pub[sp],
				    (atomic_val_t)stem_peak[sp]);
		}
	}
}
#endif /* SP1_XFER_ENABLE */

/* -O2 for the same reason as stem_audio_block() just above: this is its only
 * caller, it runs once per 5.333 ms block on the deadline thread, and it was
 * the last function on that path still built for size. */
__attribute__((optimize("O2")))
static void looper_audio_block(int16_t *s)
{
#if SP1_XFER_ENABLE
	/* ==================== STEM TAPE DISPATCH, FIRST =======================
	 * Before ANY inherited classic work. stem_audio_block()'s own comment
	 * has the full inventory of what that skips, and why it matters here. */
	if (atomic_get(&g_stem_reload_req)) {
		bool reload_ok = false;

		if (stem_streams_init(g_stem_reload_pending.song_start_block,
				       g_stem_reload_pending.song_block_count, g_stem_reload_pending.frames,
				       g_stem_reload_pending.sector_count, /*loop_enabled=*/true)) {
			/* Group 0 of all four stems was read AND validated by the
			 * streamer before it published the reload (see stem_song_
			 * post_commit_reload()), against the stem and span each
			 * group was asked for. The independent second check this
			 * site has always performed is now st_stream_init() above,
			 * which is the geometry half; the identity half a v1.1
			 * sector header used to provide is what the group header
			 * does better, and earlier. */
			{
				(void)st_beat_timing_init(&g_stem_beat_timing, g_stem_reload_pending.bpm_q8,
							   g_stem_reload_pending.downbeat_frame,
							   g_stem_reload_pending.sample_rate);
				{
					uint32_t mk;

					for (mk = 0; mk < ST_PL_STEMS; mk++) {
						st_stem_mbox_init(&g_stem_mbox[mk], 0u);
					}
				}
				reload_ok = true;
			}
		}
		if (!reload_ok) {
			(void)atomic_add(&g_stem_reload_fail_count, 1);
		}
		/* A seam armed against the song being replaced would seek into
		 * the new one. Nothing is sounding across a reload anyway, so
		 * the gain returns to unity with it. */
		s_stem_jump_pend = 0u;
		st_seam_reset(&s_stem_seam);
		/* A new song under the same playhead: the carried frame belongs
		 * to the song being replaced, and the reel starts from rest. */
		stem_rs_drop();
		st_inertia_reset(&s_stem_inertia);
		/* A NEW SONG STARTS AT ITS OWN PITCH. Carrying the last song's
		 * semitone offset across a reload would silently transpose
		 * something the player never adjusted. */
		st_pitch_reset(&s_stem_pitch);
		st_fnplay_reset(&s_stem_fnplay);
		atomic_set(&g_stem_slow_req, 0);
		s_stem_slow_q16 = ST_PITCH_ONE;
		atomic_set(&g_stem_song_selected, reload_ok ? 1 : 0);
		atomic_set(&g_stem_reload_req, 0);
	}

	/*
	 * ---- THE TRANSPORT HAS MASS -------------------------------------
	 * PLAY and STOP are requests to the reel, not to the output. The stem
	 * engine therefore keeps rendering for as long as the reel is TURNING,
	 * which on a stop is several hundred milliseconds after g_playing went
	 * false -- that spin-down is audible, pitched, and read from the tape
	 * like any other audio. Cutting the branch on g_playing alone, as this
	 * did before, is precisely what would turn a tape stop into a mute.
	 */
	if (g_playing) {
		st_inertia_play(&s_stem_inertia, I2S_TRUE_HZ);
	} else {
		st_inertia_stop(&s_stem_inertia, I2S_TRUE_HZ);
	}

	if (atomic_get(&g_stem_song_selected) != 0 &&
	    (g_playing || st_inertia_moving(&s_stem_inertia))) {
		int32_t m0, md, mv;

		/* Idempotent transport sync -- and it must stay called through
		 * the spin-down, or the stream would refuse to advance the
		 * playhead for the very frames the slowdown is made of. */
		stem_streams_play();
		/* CONSUME the transport/edit request flags rather than leaving
		 * them latched. Every handler for them below is classic-only and,
		 * on this firmware, a provable no-op: g_restart_req's body
		 * requires g_loop_active (never set -- no classic content),
		 * g_slot_switch_req reloads trk[] from g_meta.slot[].present[]
		 * (never nonzero -- the classic-source-absence gate proves it
		 * fail-closed), g_stop_req ends a take that cannot exist, and
		 * g_chop_req retunes the classic chop window. Clearing them is
		 * exactly what running those handlers would do, minus the dead
		 * work -- and NOT clearing them would latch a request forever,
		 * which is a real bug rather than a saved instruction. */
		g_stop_req = 0;
		g_restart_req = 0;
		g_chop_req = 0;
		g_slot_switch_req = 0;

		master_vol_ramp(&m0, &md, &mv);
		stem_audio_block(s, m0, md, mv);
		audio_block_epilogue();
		return;
	}
	/* No stem song playing: the classic engine below renders this block.
	 * Freeze the stem transport, and let the meters decay to dark rather
	 * than hold whatever the last playing block left behind. */
	stem_streams_stop();
	/* The reel is at rest, and it must be RECORDED as at rest. Reaching
	 * here with a spin-down still half-run -- which happens when the song
	 * is deselected mid-stop -- would leave the envelope frozen part-way,
	 * and the next PLAY would "catch" a reel that has not turned in
	 * minutes. The resampler's carried frame belongs to that stopped
	 * playhead too. */
	st_inertia_reset(&s_stem_inertia);
	stem_rs_drop();
	for (uint32_t sp = 0; sp < ST11_STEM_COUNT; sp++) {
		atomic_set(&g_stem_peak_pub[sp], 0);
	}
#endif
	if (g_xfer_mode) {
		/* ACKNOWLEDGE, then silence. Set here rather than anywhere
		 * earlier because this is the point past which this thread
		 * provably touches no stem buffer for the rest of the block. */
		atomic_set(&g_xfer_audio_quiesced, 1);
		memset(s, 0, BLK_BYTES);
		return;                                 /* USB transfer: silence out */
	}
	/* STEM TAPE: UAC2 removed -- there is no USB-sourced audio to prebuffer
	 * or drain here any more (see this file's own top-of-file comment).
	 * PASS A below now sources `live` as a constant silence in place of the
	 * old ring-drain. */

	/* FAILSAFE — exactly one recorder. Every block, find the single ARMED/REC
	 * track and make g_rec_track the one source of truth; if a second recorder
	 * somehow appeared, demote it back to play/empty. This guarantees recording
	 * can only ever touch one track at a time, no matter what races upstream. */
	{
		int only = -1;
		for (int i = 0; i < NTRK; i++) {
			uint8_t st = trk[i].state;
			if (st != TS_ARMED && st != TS_REC) continue;
			if (only < 0) only = i;
			else trk[i].state = (g_slot < NUM_SLOTS &&
					     g_meta.slot[g_slot].present[i]) ? TS_PLAY : TS_EMPTY;
		}
		g_rec_track = only;
	}

	/* PROVISIONAL AUTO-CONFIRM (engine-side, control-loop-independent): once
	 * a provisional take has captured ~150 ms of real material it is clearly
	 * not a transit graze (grazes abort within ~100 ms via the press-edge
	 * guard) — confirm it so the streamer starts flushing well inside the
	 * rec ring's ~341 ms horizon even if the control loop is frozen
	 * (FUNCTION page / USB transfer) before its own confirm could run.
	 * Without this a frozen control loop left a zombie RAM-only take that
	 * overflowed its ring and was discarded at the eventual stop tap. */
	/* DOUBLE-TAP DELETE: clear the track — abort any take it has in flight,
	 * drop it from the song, persist. If it was the song's only content, the
	 * song resets to empty (the next take sets a fresh loop length). Ring
	 * indices are NOT touched here: a mid-flush streamer pass may still be
	 * draining them (finite, writes land in the deleted track's own region);
	 * every take start re-zeroes them anyway. */
	for (int i = 0; i < NTRK; i++) {
		if (!g_del_req[i]) continue;
		g_del_req[i] = 0;
		if (g_rec_track == i) g_rec_track = -1;
		trk[i].state = TS_EMPTY;
		trk[i].rec_silence = 0; trk[i].rec_target = 0; trk[i].rec_count = 0; trk[i].muted = 0;
		trk[i].len_blocks = 0; trk[i].start_blk = 0; trk[i].content_blocks = 0;  /* drop all its segments */
		if (g_slot < NUM_SLOTS) {
			g_meta.slot[g_slot].present[i] = 0;
			g_meta.slot[g_slot].trk_len[i] = 0;
			g_meta.slot[g_slot].trk_start[i] = 0;
			g_meta.trk_content[g_slot][i] = 0;   /* keep the on-flash block self-consistent */
			g_meta.song_mode[g_slot] &= (uint8_t)~(uint8_t)(0x10u << i); /* M7-r4: unmute */
		}
		int any = 0;
		for (int k = 0; k < NTRK; k++)
			if (trk[k].state != TS_EMPTY ||
			    (g_slot < NUM_SLOTS && g_meta.slot[g_slot].present[k]))
				any = 1;
		if (!any) {
			g_loop_len = 0; g_loop_blocks = 0; g_loop_active = 0;
			if (g_slot < NUM_SLOTS) {
				g_meta.slot[g_slot].loop_len = 0;
				g_meta.song_mode[g_slot] = 0;   /* M7c: unstamp */
				g_meta.chop[g_slot][0] = 0;     /* M7a: unchop  */
				g_meta.chop[g_slot][1] = 0;
			}
			g_chop_div = 1; g_chop_off = 0;
			g_fixed_len = g_mode_pref;              /* rejoin global */
		}
		g_meta_save_req = 1;
	}

	/* HOLD-TO-RECORD. A track button held down records that track; releasing it
	 * stops. The FIRST take starts immediately and its hold duration sets the
	 * master length (snapped to whole bars on release); later tracks (overdubs)
	 * arm and begin on the next beat, in sync. Only one track records at a time. */

	/* RELEASE -> stop the current take */
	if (g_stop_req) {
		g_stop_req = 0;
		int i = g_rec_track;
		if (i >= 0 && i < NTRK) {
			if (trk[i].state == TS_ARMED) {
				/* cancelled before any sound — or while still PROVISIONAL
				 * (an empty-track instant arm whose press turned out to be
				 * a transit graze toward a higher button). A provisional
				 * take has only captured into RAM (PASS 1 skips its flush),
				 * so aborting leaves NO trace: no flash write, no junk
				 * take, no grid/BPM hijack on an empty song, and the
				 * transport state the arm forced is put back. */
				/* -> back to PLAY/EMPTY. */
				trk[i].state = (g_slot < NUM_SLOTS && g_meta.slot[g_slot].present[i])
					       ? TS_PLAY : TS_EMPTY;
				g_rec_track = -1;
				g_grid_punch_at = 0;   /* M8b: cancel the scheduled punch */
				if (g_loop_len == 0u) {
					/* A sole-track re-record ARM reset the song grid and the
					 * playhead. A cancel must UNDO that damage: restore the
					 * saved grid, and re-anchor every playing ring to the
					 * (reset) playhead — without this the track replays one
					 * stale ~341 ms ring chunk for as long as the song had
					 * been running (PASS 2 believes the ring is pinned full)
					 * and the NEXT take hijacks the song grid. */
					if (g_slot < NUM_SLOTS && g_meta.slot[g_slot].loop_len) {
						g_loop_len = g_meta.slot[g_slot].loop_len;
						g_loop_blocks = g_loop_len / SAMP_PER_BLK;
					}
					int anyp = 0;
					for (int k = 0; k < NTRK; k++)
						if (trk[k].state == TS_PLAY) {
							anyp = 1;
							trk[k].p_w = g_consume_pos;  /* starve -> clean refill */
						}
					if (!anyp) g_loop_active = 0;
				}
			} else if (trk[i].state == TS_REC) {
				/* FREE-LENGTH stop (every take): the loop is EXACTLY what was
				 * recorded — no quantization to the first track's grid, no
				 * silence padding while you hunt for the loop point. Rounded
				 * only to the 256-sample storage block (~±2.7 ms, inaudible)
				 * so the eMMC streaming stays block-aligned. The FIRST take
				 * of a song additionally defines the beat grid + BPM (LEDs,
				 * MIDI clock); later tracks free-run on their own cycles. */
				/* CONTENT length = the audio actually recorded, rounded UP to a
				 * whole block so nothing is lost. rec_target is set to CONTENT,
				 * not the (possibly longer) loop length, so the recorder pads
				 * only this final <1 block and finalises INSTANTLY on the tap. */
				/* R2 (perfect-loop): backdate the stop by the constant
				 * gesture latency so the end lands on the finger, not
				 * on the pipeline. */
				uint32_t rc = trk[i].rec_count;
				if (rc > STOP_COMP_SAMPLES + SAMP_PER_BLK)
					rc -= STOP_COMP_SAMPLES;
				uint32_t content = (rc + SAMP_PER_BLK - 1u)
						   / SAMP_PER_BLK;
				if (content < 1u) content = 1u;
				else if (content > MAX_LOOP_BLOCKS) content = MAX_LOOP_BLOCKS;
				/* M8b QUANTIZED STOP: a grid-punched take rounds to the
				 * NEAREST grid beat. The beat is block-rounded ONCE and
				 * shared by every grid take -> all lengths are multiples
				 * of the same base and stay locked to each other. */
				uint32_t glen = 0, gbb = 0;
				if (g_gridrec && g_gridrec_beat_samps) {
					gbb = (g_gridrec_beat_samps + SAMP_PER_BLK / 2u)
					      / SAMP_PER_BLK;
					if (gbb < 1u) gbb = 1u;
					uint32_t gm = (content + gbb / 2u) / gbb;
					if (gm < 1u) gm = 1u;
					uint32_t nearest = gm * gbb;
					if (g_loop_len == 0u) {
						/* M8b-r3 FIRST TAKE: TRIM-BACK policy — the
						 * stop is instant and a loop can never
						 * contain silence. Run on only in the
						 * "nailed it" window (last ~15% of a beat);
						 * otherwise snap DOWN to the last whole
						 * beat (overhang stays on flash, unplayed).
						 * Tap-tempo drift made the old run-to-the-
						 * line wait long enough to read as "still
						 * recording" (user report). */
						uint32_t fl = (content / gbb) * gbb;
						uint32_t win = (gbb * 4u) / 25u;
						if (content < gbb)
							glen = gbb;     /* degenerate: complete 1 beat */
						else if (nearest > fl &&
						         (nearest - content) <= win)
							glen = nearest; /* tiny run-on to the line */
						else
							glen = fl;      /* trim back — instant */
					} else {
						glen = nearest;         /* overdub: fixed-style */
					}
					if (glen > MAX_LOOP_BLOCKS) glen = 0;  /* fall back free */
				}
				if (g_loop_len == 0u) {
					uint32_t base = glen ? glen : content;
					g_loop_len = base * SAMP_PER_BLK;
					g_loop_blocks = base;
					if (glen && gbb) {
						/* the TAPPED grid defines the beat — exact
						 * stored-domain beat, not the estimator */
						g_beat_samples = g_gridrec_beat_samps;
						g_midi_div = g_gridrec_beat_samps / 24u;
					} else
					tempo_finish();         /* set the detected beat grid + BPM */
					if (g_slot < NUM_SLOTS) {
						g_meta.slot[g_slot].loop_len = g_loop_len;
						g_meta.song_mode[g_slot] = (uint8_t)
							((g_meta.song_mode[g_slot] & 0xF0u) |
							 (g_fixed_len ? 2u : 1u)); /* M7c stamp */
						g_meta_save_req = 1;
					}
				}
				uint32_t len = content;
				uint32_t tgt = content * SAMP_PER_BLK;   /* default: stop now */
				uint8_t  sil = 1;                        /* pad the final sub-block */
				if (trk[i].rec_target && !trk[i].rec_silence) {
					/* SECOND tap while a fixed-mode take is running on to
					 * the bar line (below): stop IMMEDIATELY — the loop
					 * keeps the already-snapped bar length; the unfilled
					 * remainder plays as silence (the old behavior, as an
					 * escape hatch). */
					len = trk[i].len_blocks;
					if (g_gridrec && g_gridrec_beat_samps) {
						/* M8b-r3: on a GRID take the escape trims to
						 * the last WHOLE beat instead of amputating
						 * mid-beat and leaving a silent tail. */
						uint32_t bb2 = (g_gridrec_beat_samps +
								SAMP_PER_BLK / 2u) / SAMP_PER_BLK;
						if (bb2 >= 1u) {
							uint32_t fl2 = ((trk[i].rec_count /
									 SAMP_PER_BLK) / bb2) * bb2;
							if (fl2 >= bb2) {
								len = fl2;
								content = fl2;
								tgt = fl2 * SAMP_PER_BLK;
							}
						}
					}
				} else if (glen) {
					/* M8b: grid take — same early/late machinery as
					 * fixed mode, with the tapped beat as the base:
					 * EARLY -> run on to the grid line capturing live;
					 * LATE -> snap back (overhang never plays). */
					len = glen;
					if (glen * SAMP_PER_BLK > trk[i].rec_count) {
						content = glen;
						tgt = glen * SAMP_PER_BLK;
						sil = 0;
					}
				} else if (g_fixed_len && g_loop_blocks) {
					/* FIXED mode: round to the NEAREST whole multiple of
					 * the base — ceil-only rounding gapped BOTH ways
					 * (community: stop a hair early and the tail padded
					 * with silence; a hair late and nearly a whole extra
					 * bar of silence was appended).
					 *  - stopped EARLY (before the nearest bar): the tap
					 *    SCHEDULES the stop — recording runs on to the bar
					 *    line capturing live audio, so the loop ends ON
					 *    the bar with real sound in it (the emit path
					 *    fades the final ~2.7 ms into the seam). The track
					 *    LED stays on until the bar; tap again to force an
					 *    immediate stop.
					 *  - stopped LATE (past the nearest bar): snap BACK to
					 *    it — the overhang stays on flash but is never
					 *    played (promotion fades the new seam). */
					uint32_t mult = (content + g_loop_blocks / 2u) / g_loop_blocks;
					if (mult < 1u) mult = 1u;
					uint32_t nlen = mult * g_loop_blocks;
					if (nlen <= MAX_LOOP_BLOCKS) {
						len = nlen;
						if (nlen * SAMP_PER_BLK > trk[i].rec_count) {
							/* EARLY: run on to the bar, capturing live */
							content = nlen;
							tgt = nlen * SAMP_PER_BLK;
							sil = 0;
						}
					}
					/* nlen over the region: len stays = content, stop now */
				}
				trk[i].content_blocks = content;     /* audio ends here */
				trk[i].len_blocks     = len;         /* loop length */
				trk[i].rec_target     = tgt;
				/* end the live phrase. When padding (immediate stops), the
				 * pad used to be hard zeros — a click baked into the seam;
				 * fade the first 128 pad samples (~2.7 ms) down instead. */
				trk[i].rec_silence = sil;
				g_gridrec = 0;
				if (sil) {
					trk[i].rec_fade = 128;
					/* the pad is only rec_target - rec_count samples
					 * (0..255); steepen the slope so the fade always
					 * COMPLETES inside it. */
					uint32_t pad = trk[i].rec_target - trk[i].rec_count;
					trk[i].rec_fstep = (pad && pad < 128u)
						? (uint8_t)((128u + pad - 1u) / pad) : 1u;
				}
			}
		}
	}

	/* PRESS -> start recording that track (if nothing else is recording) */
	for (int i = 0; i < NTRK; i++) {
		if (!g_arm_req[i]) continue;
		g_arm_req[i] = 0;
		if (!g_emmc_ready) continue;
		if (g_rec_track >= 0) continue;                       /* one at a time */
		/* ONE take in flight, device-wide: refuse while ANY track is armed,
		 * recording, or still flushing (TS_DONE). The rec ring is SHARED, so a
		 * second take during a drain would interleave into the same buffer; and
		 * two flushes would also exceed the eMMC write budget. The press becomes
		 * valid the moment the drain finishes (sub-second; LED solid meanwhile). */
		int busy = 0;
		for (int k = 0; k < NTRK; k++) {
			uint8_t st = trk[k].state;
			if (st == TS_REC || st == TS_ARMED || st == TS_DONE) busy = 1;
		}
		if (busy) continue;
		/* sole track in the song -> allow a fresh length (reset only the in-RAM
		 * master; the saved length is rewritten when this new take completes).
		 * ANY non-empty state on another track counts as "others" — including
		 * TS_DONE (a take still flushing to the card): resetting the length while
		 * another take is mid-flush would corrupt it. */
		int others = 0;
		for (int k = 0; k < NTRK; k++)
			if (k != i && (trk[k].state != TS_EMPTY ||
				       (g_slot < NUM_SLOTS && g_meta.slot[g_slot].present[k])))
				others = 1;
		if (!others) { g_loop_len = 0; g_loop_blocks = 0; g_loop_active = 0; }

		if (g_loop_len == 0u) {
			/* FIRST take: start the transport NOW so the recorder can watch the
			 * input, but DON'T capture yet — recording begins at the first sound
			 * (auto-start), at which point the playhead is reset so that sound is
			 * loop position 0. Snap the tape speed so no spin-up ramp is baked in. */
			g_cur_speed_q16 = g_play_speed_q16;   /* snap to the set tape speed */
			g_loop_active = 1; g_consume_pos = 0;
			g_pphase = 0; g_frames_since = 0; g_dec_acc = 0;
		}
		/* ARM (first take AND overdub): wait for the first sound, then the tick
		 * handler begins the capture so the loop starts exactly on the audio. */
		trk[i].r_w = 0; trk[i].r_r = 0; trk[i].flush_blk = 0;
		trk[i].flush_mod = MAX_LOOP_BLOCKS;
		trk[i].rec_count = 0; trk[i].rec_silence = 0; trk[i].rec_target = 0; trk[i].muted = 0;
		if (g_slot < NUM_SLOTS)   /* M7-r4: a fresh take is audible — unmute */
			g_meta.song_mode[g_slot] &= (uint8_t)~(uint8_t)(0x10u << i);
		trk[i].wait_peak = 0; trk[i].wait_ticks = 0;
		if (g_grid_active && g_grid_beat_frames) {
			if (g_loop_len == 0u) {
				/* M8b-r5: the FIRST take punches IMMEDIATELY. The
				 * punch re-anchors the downbeat anyway, so waiting
				 * for the tapped lattice bought nothing — the phase
				 * was discarded one line later (r2 contradiction;
				 * felt as "aggressive waiting", user report). Story:
				 * taps teach TEMPO, your first take places the
				 * downbeat, overdubs wait for your bar. Remaining
				 * first-take latency = the ~130 ms arm constant. */
				g_grid_punch_at = g_sample_clock ? g_sample_clock : 1u;
			} else {
				/* OVERDUBS: next BAR line — aligning to the
				 * existing material is the whole point. The armed
				 * LED fast-blinks the count-in. */
				uint64_t unit = (uint64_t)g_grid_beat_frames * 4u;
				uint64_t off = g_sample_clock - g_grid_anchor;
				g_grid_punch_at = g_grid_anchor +
					((off + unit - 1u) / unit) * unit;
			}
		} else {
			g_grid_punch_at = 0;
		}
		/* NOTE: len_blocks/start_blk are NOT reset here -- they are set when the
		 * first sound lands (TS_REC). Leaving them intact means a re-record that
		 * is cancelled (released before any sound) returns the track to PLAY with
		 * its ORIGINAL length, not a clobbered one. */
		trk[i].state = TS_ARMED;
		g_rec_track = i;
	}

	/* HOLD PLAY -> jump to the start of the song and play. Rewind the shared
	 * playhead to 0 and reset every track's read frontier so the streamer refills
	 * each loop from its first block; they all restart together, in sync. Ignored
	 * while recording (so a take isn't disrupted). */
	if (g_restart_req) {
		g_restart_req = 0;
		if (g_rec_track < 0 && g_loop_active) {
			g_consume_pos = 0; g_pphase = 0; g_frames_since = 0; g_dec_acc = 0; g_midi_cnt = 0;
			for (int i = 0; i < NTRK; i++) trk[i].p_w = 0;
			g_playing = 1;
			g_midi_start_pending = 1;
		}
	}

	/* CHOP CHANGE: drop the (old-window) read-ahead so the new window is
	 * audible within one refill round (~20-40 ms, boundary-faded by the
	 * starve machinery) instead of after ~341 ms of stale ring. */
	if (g_chop_req) {
		g_chop_req = 0;
		for (int i = 0; i < NTRK; i++)
			if (trk[i].state == TS_PLAY)
				trk[i].p_w = (g_consume_pos / SAMP_PER_BLK) * SAMP_PER_BLK;
	}

	/* SONG SWITCH: reload the tracks for the newly-selected slot. Tracks that the
	 * slot already has recorded -> PLAY (streamer refills from that slot's eMMC
	 * region from block 0); empty tracks -> ready to record. Restart the loop. */
	if (g_slot_switch_req) {
		g_slot_switch_req = 0;
		g_consume_pos = 0; g_pphase = 0; g_frames_since = 0; g_dec_acc = 0; g_midi_cnt = 0;
		g_rec_track = -1;
		/* this song's remembered loop length (0 = empty, ready for a fresh take) */
		g_loop_len    = (g_slot < NUM_SLOTS) ? g_meta.slot[g_slot].loop_len : 0;
		g_loop_blocks = g_loop_len / SAMP_PER_BLK;
		int any = 0;
		for (int i = 0; i < NTRK; i++) {
			uint8_t pres = (g_slot < NUM_SLOTS) ? g_meta.slot[g_slot].present[i] : 0;
			trk[i].state = pres ? TS_PLAY : TS_EMPTY;
			trk[i].p_w = 0;
			trk[i].rec_silence = 0; trk[i].rec_target = 0; trk[i].rec_count = 0;
			/* M7-r4: the song's saved mutes come back with it */
			trk[i].muted = (pres && g_slot < NUM_SLOTS &&
			                (g_meta.song_mode[g_slot] & (0x10u << i))) ? 1u : 0u;
			/* SEGMENT: restore this track's own length + phase anchor (older saves
			 * with trk_len==0 fall back to the base length = one segment). */
			if (pres && g_slot < NUM_SLOTS) {
				uint32_t L = g_meta.slot[g_slot].trk_len[i];
				trk[i].len_blocks = L ? L : g_loop_blocks;
				trk[i].start_blk  = g_meta.slot[g_slot].trk_start[i];
				trk[i].content_blocks = g_meta.trk_content[g_slot][i]; /* 0 = whole track */
			} else {
				trk[i].len_blocks = 0; trk[i].start_blk = 0;
				trk[i].content_blocks = 0;
			}
			if (pres) any = 1;
		}
		g_loop_active = any && (g_loop_len > 0);
	}

	/* ---- TAPE-EFFECT speed smoothing (once per block, like the SP-1) ----
	 * target = the rocker speed when playing, 0 when stopped. A one-pole filter
	 * glides the actual speed toward the target by 2% per block, giving the tape
	 * ramp on play/pause AND on tempo changes. Recording does NOT force 1.0x any
	 * more: capture ticks in the loop-sample domain at the current speed, so an
	 * overdub lands exactly as heard at ANY speed — and there's no pitch JUMP
	 * when record starts/stops. The Q16 step feeds the resampler below. */
	uint32_t target_q16 = g_playing ? g_play_speed_q16 : 0u;
	int32_t sd = (int32_t)target_q16 - (int32_t)g_cur_speed_q16;
	if (sd > -64 && sd < 64) g_cur_speed_q16 = target_q16;                 /* snap when ~there */
	else g_cur_speed_q16 = (uint32_t)((int32_t)g_cur_speed_q16 + sd / 50); /* one-pole ~2%/block */
	/* At exact unity, drop any fractional-phase residue left by the spin-up
	 * ramp (one-time <=1/2-sample jump, inaudible): otherwise frac stays
	 * nonzero forever and every playing track pays the interpolation
	 * multiply per frame despite running at 1.0x. */
	if (g_cur_speed_q16 == 65536u && g_playing && (g_pphase & 0xFFFFu))
		g_pphase &= ~0xFFFFu;
	uint32_t step = g_cur_speed_q16 / DECIM;                              /* Q16 per I2S frame */

	/* Snapshot per-track fader volume once per block. vol_q8 is volatile (reloaded
	 * every frame at -Os), but its only writer is the lower-priority controls path
	 * and the mixer (PREEMPT 0) outranks it, so it is constant
	 * across the 256 frames -- the snapshot is bit-identical and drops ~1024 reloads. */
	uint16_t vol_s[NTRK];
	for (int i = 0; i < NTRK; i++) vol_s[i] = trk[i].vol_q8;

	/* ==== PASS A: transport + record, stashing per-frame positions ====
	 * The old single loop interleaved all four tracks' mixing into every
	 * frame, paying loop + volatile-read overhead 4 x 48000 times a second
	 * even for empty tracks — measured with the kernel's thread stats at
	 * 31% CPU stopped / 40% playing, which starved the eMMC streamer below
	 * the refill rate it needed (the cut-outs while recording track 4).
	 * Restructured into per-block passes: A) advance transport + record
	 * exactly as before, stashing each frame's playhead position + phase;
	 * B) one tight accumulation loop per PLAYING track; C) master volume +
	 * limiter + stereo write-out. Arithmetic, ordering and per-frame starve
	 * semantics are unchanged — the loops are merely inverted so per-track
	 * invariants hoist out of the 48 kHz hot path. */
	static uint32_t posb[BLK_FRAMES];
	static uint16_t fracb[BLK_FRAMES];
	static int32_t  mix32[BLK_FRAMES];
	for (uint32_t f = 0; f < BLK_FRAMES; f++) {
		/* STEM TAPE: UAC2 removed -- `live` was the USB monitor sample;
		 * it is now always silence, in place of the real STIX-selected
		 * song stream a later Phase 2 commit wires in here. */
		int32_t live = 0;

		/* (the first take is started immediately by the press handler above, and
		 * overdubs are started on the next beat by the wrap logic below) */
		mix32[f] = live;                        /* live monitor under the mix */
		posb[f]  = g_consume_pos;
		fracb[f] = (uint16_t)(g_pphase & 0xFFFFu);

		/* advance the playback phase; each integer step is one loop-sample tick */
		g_dec_acc += live; g_frames_since++;
		if (g_loop_active) {
			g_pphase += step;
			while (g_pphase >= 65536u) {
				g_pphase -= 65536u;
				/* GATE 1: this used to also decimate the live input to the
				 * current tape rate into `lsamp` for recording capture.
				 * There is no recording in this firmware -- g_dec_acc/
				 * g_frames_since still accumulate above (mix32[f]=live,
				 * now always 0 -- see this loop's own comment) purely to
				 * stay bit-identical to the classic looper's tick-advance
				 * timing; they are reset here every tick same as before,
				 * just with no per-tick sample ever extracted from them. */
				g_dec_acc = 0; g_frames_since = 0;

				g_consume_pos++;
				/* MIDI 24-PPQN clock: a cheap per-sample COUNTER (the divisor
				 * g_midi_div is precomputed when the tempo is detected) -- no
				 * runtime divide here. The beat-phase display moved to once-per-
				 * block (after this loop); it only drives the LED + diag. */
				if (!g_grid_active && g_midi_div && ++g_midi_cnt >= g_midi_div) {
					g_midi_cnt = 0; g_midi_clk_produced++;
				}
			}
		}
	}

	/* ==== PASS B is REMOVED ====
	 *
	 * It accumulated each PLAYING classic track out of trk[].pring into
	 * mix32[]. Two independent facts make it unreachable in this firmware:
	 * no track can be in TS_PLAY (g_meta.slot[].present[] is never assigned
	 * a nonzero value anywhere in this file -- the classic-source-absence
	 * CI gate proves that fail-closed), and since the classic play-ring
	 * read-ahead (PASS 2 in streamer_thread) was deleted, NOTHING writes
	 * pring at all. It was a loop that could not run, reading a buffer that
	 * was never filled.
	 *
	 * Deleting it retires trk[].pring: 4 tracks x RING_SAMPLES(16384) x 2
	 * bytes = 131,072 bytes of RAM that no code path could ever read a
	 * meaningful sample out of. That is what pays for the stored-song
	 * ring's depth -- see ST_STEM_MBOX_SLOTS in st_stem_bufmbox.h.
	 *
	 * mix32[] therefore stays at whatever PASS A wrote (silence), which is
	 * exactly what it already held: this changes no audio, it removes work
	 * and memory that could not affect any. */


	/* ==== PASS C: master volume + soft limiter -> stereo out, OR the real
	 * stored-song stereo stem mix (STEM TAPE: architecture correction,
	 * Phase 2 slice 3 pre-work) ====
	 *
	 * ARCHITECTURE CORRECTION: a validated, playing Stem Tape song
	 * REPLACES the inherited classic mono-loop bus outright -- the two
	 * are never summed. Product rationale: Stem Tape never records or
	 * overdubs, so the classic per-track engine inherited byte-for-byte
	 * from the Tape Looper (PASS A/B above, mix32[]/trk[].pring) is not
	 * a second, independently-mixable audio source in this firmware --
	 * it is dead weight that must not leak into the real output even in
	 * a latent-bug scenario. (In this build present[] is in fact never
	 * assigned a nonzero value anywhere in this file -- see the
	 * classic-source-absence CI gate -- so trk[].state can never reach
	 * TS_PLAY and mix32[] is always silence today; the if/else below is
	 * the defense-in-depth architectural guarantee regardless, so a
	 * future change to that invariant could never reintroduce summed
	 * classic+stem audio by accident.) PASS A/B keep computing mix32[]
	 * unconditionally, unchanged -- the proven Tape-Looper-derived
	 * engine, its soft limiter, and this whole I2S output stage stay
	 * exactly as they were -- PASS C simply never reads mix32[] while a
	 * stem song is active. */
	{
		int32_t m0, md, mv;

		master_vol_ramp(&m0, &md, &mv);
		/* Classic mono bus + master volume + the proven soft_limit(),
		 * byte-for-byte as the Tape-Looper-derived engine always computed
		 * it. Reached only when no stem song is playing -- the stem path
		 * returned long before this point. */
		for (uint32_t f = 0; f < BLK_FRAMES; f++) {
			int32_t m = md ? (m0 + ((md * (int32_t)(f + 1)) >> 8)) : mv;
			int16_t classic = soft_limit((mix32[f] * m) >> 8);

			s[2 * f]     = classic;
			s[2 * f + 1] = classic;
		}
	}
	audio_block_epilogue();

	/* diag WATERMARKS (once per block): how close each ring got to its cliff
	 * this window — shows near-misses even when no starve/overrun fired. */
	{
		uint32_t _cp = g_consume_pos;
		for (int i = 0; i < NTRK; i++) {
			if (trk[i].state != TS_PLAY) continue;
			int32_t _av = (int32_t)(trk[i].p_w - _cp);
			if (_av < g_play_lowat) g_play_lowat = _av;
		}
		int _rt = g_rec_track;
		if (_rt >= 0 && trk[_rt].state == TS_REC) {
			uint32_t _fill = trk[_rt].r_w - trk[_rt].r_r;
			if (_fill > g_rec_hiwat) g_rec_hiwat = _fill;
		}
	}
}

/* eMMC busy-abort callback: polled ~1 kHz inside the driver's ABORTABLE R1b
 * waits (the idle cache flush), on the streamer thread. true = fire an HPI
 * and bail. Trips the moment a take is armed/recording/finalizing, shutdown
 * work is pending, or any playing ring has drained to half. */
static bool emmc_busy_abort_chk(void)
{
	if (g_stop_req || g_cache_flush_req)
		return true;
	for (int j = 0; j < NTRK; j++) {
		uint8_t sj = trk[j].state;
		if (sj == TS_ARMED || sj == TS_REC || sj == TS_DONE)
			return true;
		if (sj == TS_PLAY &&
		    (int32_t)(trk[j].p_w - g_consume_pos) <
		    (int32_t)(RING_SAMPLES / 2u))
			return true;
	}
	return false;
}

/* ========================================================================
 *  eMMC STREAMER  —  PREEMPT-5 (below audio), the ONLY eMMC user. Each loop:
 *  PASS 1 flushes the record ring to flash (writes-first), PASS 2 reads each
 *  play track ahead into its RAM ring. A balanced ADAPTIVE FLUSH yields the
 *  bus between the two passes — playback wins unless a play ring is about to
 *  underrun, recording wins at true rec-ring overflow. Also loads/saves the
 *  slot metadata (block 0) and runs the power-off cache flush.
 * ======================================================================== */
/* ---- background eMMC streamer (the ONLY eMMC user) -------------------------
 * Preemptible priority BELOW the cooperative audio thread, so the audio thread
 * can always preempt the bit-bang busy-waits and keep the I2S DMA fed. Per
 * PLAY track: read-ahead into the play ring. Per REC/DONE track: flush the rec
 * ring to the card; on DONE, finish the tail then switch the track to PLAY. */
static K_THREAD_STACK_DEFINE(streamer_stack, 4096);  /* 4096: the eMMC driver is -O2 here, so its read/send_command/crc chain inlines into a deeper frame on this thread */
static struct k_thread streamer_tcb;
static uint8_t g_streamer_started;   /* v1.2.3: streamer may start EARLY (standby) */
static void streamer_thread(void *a, void *b, void *c);
static void streamer_start(void)
{
	if (g_streamer_started) return;
	g_streamer_started = 1;
	k_thread_create(&streamer_tcb, streamer_stack, K_THREAD_STACK_SIZEOF(streamer_stack),
			streamer_thread, NULL, NULL, NULL,
			/* PREEMPT(1): the same priority as main(), NOT above it --
			 * see streamer_breathe() for the whole argument. Audio at
			 * PREEMPT(0) still preempts this instantly and anywhere;
			 * control work now waits for a yield point instead of
			 * landing in the middle of a sector read. Was (5), under
			 * main(1), which cost the streamer half its wall time. */
			K_PRIO_PREEMPT(ST_PRIO_STREAMER), 0, K_NO_WAIT);
}
static volatile uint8_t g_usb_up;    /* usb_audio_start() completed (gates xfer polling) */

#if SP1_XFER_ENABLE
/* ISR: drain the CDC RX FIFO into the ring buffer (host -> device bytes). */
static void cdc_rx_isr(const struct device *dev, void *u)
{
	ARG_UNUSED(u);
	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		uint8_t b[64];
		int n = uart_fifo_read(dev, b, sizeof b);
		if (n > 0) {
			/* If g_cdc_rx has no room, the excess is dropped here and
			 * counted in g_cdc_rx_dropped_bytes. That counter was
			 * added by Slice T0, which deliberately only made the loss
			 * VISIBLE without changing the ring size or drain rate --
			 * and it was right to flag "CDC receive loss or ring
			 * overflow" as a candidate cause of the physical failures,
			 * because that is exactly what it turned out to be: the
			 * then-1024-byte ring could not hold an 8210-byte bulk
			 * request and dropped most of every one. g_cdc_rx is now
			 * sized to hold one entire largest-possible request (see
			 * ST_CDC_RX_RING_BYTES's own comment), which is a hard
			 * bound on everything the host can have in flight at once,
			 * so this drop path should now be unreachable in normal
			 * operation. It is kept -- counted, never silent -- as a
			 * genuine last-resort detector: if it ever fires again it
			 * means a real invariant broke (a host sending more than
			 * one request without waiting for its response), and
			 * xfer_bulk_write_sector() reports it as its own distinct,
			 * accurate ERR_CDC_OVERFLOW rather than letting it
			 * masquerade as a mysterious receive timeout. */
			uint32_t put = ring_buf_put(&g_cdc_rx, b, (uint32_t)n);

			if (put < (uint32_t)n) {
				(void)atomic_add(&g_cdc_rx_dropped_bytes, (atomic_val_t)((uint32_t)n - put));
			}
		}
	}
}

/* Blocking byte send (matches how printk drives the console). */
static void cdc_tx(const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) uart_poll_out(cdc, p[i]);
}

/* feed_wdt() itself is defined much further down (near fnp_mode_toggle());
 * forward-declared here, matching this file's own established convention
 * for an early caller of a function defined later (see streamer_thread()'s
 * own single-line forward declaration above). Needed as of the bulk-upload
 * timeout fix below: cdc_rx()'s own wait loop must feed the watchdog
 * itself now that a caller may legitimately ask it to wait longer than the
 * WDT's 4000ms window (see wdt_install_timeout()'s own `.window.max =
 * 4000` -- every cdc_rx() call site in this file, until now, stayed at or
 * under that same ceiling specifically so the caller's own feed_wdt()
 * calls immediately before/after were enough; that stops being true the
 * moment any call site legitimately needs more than ~4 real seconds). */
static void feed_wdt(void);

/* Pull exactly n bytes from the RX ring, up to timeout_ms.
 *
 * Feeds the watchdog itself, once per poll iteration, whenever it is about
 * to sleep and try again -- REQUIRED (not merely a nicety) the moment
 * `timeout_ms` can legitimately exceed the WDT's own 4000ms window (see
 * ST_BULK_PAYLOAD_TIMEOUT_MS's own doc comment for why the bulk payload
 * receive is exactly such a case): without this, a genuinely slow but
 * still-succeeding transfer would hard-reset the whole SP-1 via the
 * watchdog partway through, which is a strictly worse failure than a
 * clean, caller-visible timeout response. Every existing call site in
 * this file passes timeout_ms <= 4000 and is therefore completely
 * unaffected in practice (feed_wdt() is cheap -- an NRF_WDT->RR[] register
 * write -- calling it once more per short wait changes nothing observable
 * there); this is additive safety margin for them, not a behavior change. */
static bool cdc_rx(uint8_t *p, uint32_t n, int timeout_ms)
{
	int64_t end = k_uptime_get() + timeout_ms;
	uint32_t got = 0;
	while (got < n) {
		got += ring_buf_get(&g_cdc_rx, p + got, n - got);
		if (got < n) {
			if (k_uptime_get() > end) return false;
			feed_wdt();
			k_msleep(1);
		}
	}
	return true;
}

/* A block command's sub-read stalled mid-stream, so the RX ring may hold a partial
 * payload that would misframe every later command. Drain it back to a clean command
 * boundary (consumer-side get, safe vs the producing ISR) and send the host an error
 * byte so it aborts that block; the host's next ping then resyncs cleanly. */
static void xfer_resync(uint8_t err_byte)
{
	uint8_t dump;
	while (ring_buf_get(&g_cdc_rx, &dump, 1) == 1) {
	}
	cdc_tx(&err_byte, 1);
}

/*
 * Re-reads both real v1.1 index blocks and (re)opens g_v11_session against
 * them fresh: REPLACE if the library already has a valid active record,
 * or INIT if it is genuinely blank/corrupt (the only two cases
 * st_ab_session_open_*() ever accept -- see st_ab_session.h). INIT's
 * `confirmed` is passed true unconditionally: the real wire contract has
 * no separate confirmation verb for v1.1 (docs section 7 performs
 * initialization via plain guarded writes, not a destructive-token
 * command like the old 'D'/'I'), so open_init()'s OWN requires_
 * initialization gate -- both index records already independently proven
 * blank/invalid -- is the entire, tightly-scoped safety condition here,
 * matching the contract exactly rather than inventing an extra step it
 * doesn't have. Leaves g_v11_session closed/unusable (every check_write()
 * call then returns NO_SESSION) if either index block can't even be read,
 * or if opening genuinely fails for a reason logged by the open result
 * (capacity, already-initialized) -- fails closed in every case.
 */
/* __attribute__((noinline, noclone)): same reasoning as xfer_do_commit()
 * elsewhere in this file -- a void(void) function with exactly one call
 * site (the 'Q' branch in xfer_service()) is an -Os inlining candidate
 * once nearby code shrinks; noinline stops that so this stays a real,
 * separately-named symbol the runtime symbol-presence gate can find, and
 * noclone is added defensively even though a void(void) call site gives
 * GCC's interprocedural constant propagation nothing to clone on. Trailing,
 * same-line attribute placement (not before the return type, not on its
 * own line) is required for parsers of this exact function-signature shape
 * elsewhere in this codebase -- see xfer_do_commit()'s own comment for the
 * full story; this function isn't parsed by that specific gate today, but
 * there's no reason to introduce a different, untested placement. */
static void xfer_v11_refresh_session(void)
	__attribute__((noinline, noclone));
static void xfer_v11_refresh_session(void)
{
	if (!g_v11_layout_ready) {
		memset(&g_v11_session, 0, sizeof(g_v11_session));
		st_bulk_seq_reset(&g_v11_bulk_seq, 0u, 0u);
		return;
	}

	uint8_t idx_a[ST11_PHYSICAL_BLOCK_BYTES];
	uint8_t idx_b[ST11_PHYSICAL_BLOCK_BYTES];

	if (!emmc_read_blocks(g_v11_layout.index_a_start, idx_a, 1) ||
	    !emmc_read_blocks(g_v11_layout.index_b_start, idx_b, 1)) {
		memset(&g_v11_session, 0, sizeof(g_v11_session));
		st_bulk_seq_reset(&g_v11_bulk_seq, 0u, 0u);
		return;
	}

	st_ab_open_result_t r = st_ab_session_open_replace(&g_v11_session, idx_a, idx_b, &g_v11_layout,
							     g_v11_layout.song_a_blocks);

	if (r == ST_AB_OPEN_ERR_NOT_INITIALIZED) {
		(void)st_ab_session_open_init(&g_v11_session, idx_a, idx_b, &g_v11_layout, true);
	}

	/* Bulk upload (Slice C2): reset the sequence tracker to match the
	 * session that just (re)opened. Only a REPLACE session ever has a
	 * real inactive SONG region to upload into (docs section 7: an INIT
	 * session's only writable blocks are the two index records, no song
	 * region) -- region_cap 0 for any other case makes every real 'U'
	 * request fail closed (OUT_OF_BOUNDS/DEST_MISMATCH) rather than
	 * silently accept a bulk write this session was never meant to
	 * allow. region_cap reuses the SAME g_v11_session.needed_song_blocks
	 * st_ab_session_check_write() itself enforces per block (see that
	 * function's own is_frozen_song branch) -- not a second, independently
	 * derived capacity number that could drift from the real gate. */
	if (g_v11_session.open && g_v11_session.kind == ST_AB_SESSION_REPLACE) {
		uint32_t region_start = (g_v11_session.inactive_song_slot == ST11_SLOT_A)
						 ? g_v11_session.layout.song_a_start
						 : g_v11_session.layout.song_b_start;

		st_bulk_seq_reset(&g_v11_bulk_seq, region_start, g_v11_session.needed_song_blocks);
	} else {
		st_bulk_seq_reset(&g_v11_bulk_seq, 0u, 0u);
	}
}

/*
 * Builds and sends the real 'Q' -> STCP capability reply (docs section 2),
 * immediately followed by the bulk-upload capability extension (docs/
 * stem-tape-bulk-upload-v1.md) -- ONE continuous transmission, both parts
 * of the same 'Q' response. Re-reads both index blocks and runs the SAME
 * selector st_ab_session itself uses (st_stix_read_library()) so the
 * advisory active-slot/generation fields reflect the current on-disk
 * truth -- advisory only (docs section 4: "the device's advisory
 * activeIndexSlot is never trusted"), the companion always re-derives its
 * own answer from the raw index bytes it reads independently via 'R'.
 * Sends nothing at all (neither part) if g_v11_layout_ready is false
 * (docs section 2: "Silence = stock firmware = read-only") or either
 * index block can't be read -- fails closed exactly like every other
 * v1.1 path here. The original 100-byte STCP reply is byte-for-byte
 * UNCHANGED by the extension below -- st11_stcp_build() itself is never
 * modified, so the frozen fixture equality test in test_stem_v11.c is
 * unaffected.
 */
/* __attribute__((noinline, noclone)): same reasoning as
 * xfer_v11_refresh_session() immediately above. */
static void xfer_v11_send_caps(void)
	__attribute__((noinline, noclone));
static void xfer_v11_send_caps(void)
{
	if (!g_v11_layout_ready) {
		return;
	}

	uint8_t idx_a[ST11_PHYSICAL_BLOCK_BYTES];
	uint8_t idx_b[ST11_PHYSICAL_BLOCK_BYTES];

	if (!emmc_read_blocks(g_v11_layout.index_a_start, idx_a, 1) ||
	    !emmc_read_blocks(g_v11_layout.index_b_start, idx_b, 1)) {
		return;
	}

	st_stix_library_state_t lib;

	st_stix_read_library(idx_a, idx_b, g_v11_layout.song_a_start, g_v11_layout.song_a_blocks,
			      g_v11_layout.song_b_start, g_v11_layout.song_b_blocks, &lib);

	uint8_t reply[4 + ST11_CAPS_BYTES];

	st11_stcp_build(&g_v11_layout, g_v11_device_blocks_total, lib.active_index_slot, lib.active_song_slot,
			lib.generation, reply);
	cdc_tx(reply, sizeof reply);

	uint8_t bulk_caps[ST_BULK_CAPS_BYTES];

	st_bulk_build_caps(bulk_caps);
	cdc_tx(bulk_caps, sizeof bulk_caps);
}

/* Read-adapter for st_ab_session_verify_song_before_commit()'s injected I/O
 * (st11_block_read_fn) -- a thin wrapper over the real emmc_read_blocks(),
 * matching st_transfer.h's own injected-function convention the retired
 * v1.0 code used identically (xfer_sector_read()). Its address is taken
 * (passed as a function pointer below), never called directly, so it is
 * not an -Os single-call-site inlining candidate the way
 * xfer_v11_refresh_session()/xfer_v11_send_caps() are -- xfer_sector_read()
 * never needed a noinline attribute either, for the same reason. */
static int xfer_v11_block_read(uint32_t block, uint8_t out[ST11_PHYSICAL_BLOCK_BYTES], void *ctx)
{
	ARG_UNUSED(ctx);
	/* Feeds the watchdog on EVERY block. This callback's only caller is
	 * st_ab_session_verify_song_before_commit(), whose loop re-reads the
	 * entire song region -- for a real 248.5 MiB song, 509,024 calls that
	 * run for minutes without ever returning to any other code. Nothing in
	 * that pure library loop feeds the WDT (it has no business knowing
	 * about one), and the WDT window is 4000ms, so before this the device
	 * hard-reset a few seconds into every real commit: the host saw its
	 * acknowledgement never arrive and reported a timeout, when in fact the
	 * SP-1 had reset out from under it. feed_wdt() is a single NRF_WDT->RR[]
	 * register write, so doing it per block costs nothing measurable next
	 * to the eMMC read it accompanies. */
	feed_wdt();
	return emmc_read_blocks(block, out, 1) ? 0 : -1;
}

/*
 * THE only function in this firmware image that can ever call
 * emmc_write_blocks() for the v1.1 A/B storage engine. Every block this
 * accepts has already been proven safe by st_ab_session_check_write()
 * against g_v11_session's CURRENT frozen destination pair (see
 * st_ab_session.h's own doc: active song/index rejected outright, anything
 * outside the frozen pair rejected, a commit record validated field-by-
 * field and generation-checked before being allowed to land) -- bounded by
 * this function's OWN body, independent of caller, the same discipline the
 * retired v1.0 adapters (xfer_staging_write() et al.) established. See the
 * STRICT persistence safety gate's own ALLOWED_WRITE_FUNCS entry for this
 * function, which checks that exact property by source inspection.
 *
 * Before the ONE magic-committing write of a REPLACE session is allowed
 * through, this firmware verifies the actual song bytes already on the
 * frozen region against the candidate record's own declared checksums --
 * real injected I/O via xfer_v11_block_read() above -- rather than ever
 * trusting the companion's own claim that its upload succeeded (the user's
 * explicit requirement: "do not rely solely on the companion behaving
 * correctly"). A candidate that fails this real verification is never
 * marked verified, so st_ab_session_check_write() itself refuses the
 * commit -- this function does not duplicate that refusal, only runs the
 * verify attempt once, before the decision that matters.
 */
static int xfer_v11_write(uint32_t block, const uint8_t data[ST11_PHYSICAL_BLOCK_BYTES])
	__attribute__((noinline, noclone));
static int xfer_v11_write(uint32_t block, const uint8_t data[ST11_PHYSICAL_BLOCK_BYTES])
{
	if (!g_v11_layout_ready) {
		return -1;
	}

	st11_region_id_t region = st11_region_of_block(&g_v11_layout, block);
	bool is_frozen_index = (g_v11_session.inactive_index_slot == ST11_SLOT_A && region == ST11_REGION_INDEX_A) ||
				(g_v11_session.inactive_index_slot == ST11_SLOT_B && region == ST11_REGION_INDEX_B);

	if (g_v11_session.open && !g_v11_session.closed && g_v11_session.kind == ST_AB_SESSION_REPLACE &&
	    !g_v11_session.song_verified && is_frozen_index) {
		uint32_t magic = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
				  ((uint32_t)data[3] << 24);

		if (magic == ST11_INDEX_MAGIC) {
			st_stix_record_t candidate;

			st_stix_deserialize(data, &candidate);
			/* Fast path FIRST: if every sector of this exact record
			 * was already read back off the media and folded into the
			 * session's running checksums during a bulk upload, the
			 * verification is already complete and costs nothing here.
			 * Only when that is unavailable -- a classic 'W' upload,
			 * which never accumulates, or an accumulation invalidated
			 * by a gap -- does the full re-read run. That fallback
			 * re-reads the WHOLE song inside this one wire command:
			 * for a real 248.5 MiB song that is over half a million
			 * block reads taking minutes, which is why
			 * xfer_v11_block_read() must feed the watchdog (it now
			 * does) and why the host may still time out waiting for
			 * this acknowledgement. Both paths derive the checksums
			 * from bytes genuinely read back off the media; neither
			 * ever trusts what the companion claimed it sent. */
			if (st_ab_session_verify_accumulated(&g_v11_session, &candidate) ||
			    st_ab_session_verify_song_before_commit(&g_v11_session, &candidate, xfer_v11_block_read,
								     NULL, xfer_scratch())) {
				st_ab_session_mark_song_verified(&g_v11_session);
			}
		}
	}

	/* Slice C3: whether THIS write, if accepted, IS the sole magic-
	 * committing write of the open session (REPLACE or INIT alike --
	 * docs 12.7: "the magic ... is written LAST and is the sole commit
	 * point" applies to both). Recomputed independently of the
	 * verify-before-commit block above (which only ever runs before
	 * song_verified is set) so a magic write that lands on a LATER call,
	 * after verification already happened earlier, is still recognized. */
	bool is_magic_commit_write = false;

	if (is_frozen_index) {
		uint32_t magic = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
				  ((uint32_t)data[3] << 24);

		is_magic_commit_write = (magic == ST11_INDEX_MAGIC);
	}

	if (st_ab_session_check_write(&g_v11_session, block, data) != ST_AB_WRITE_OK) {
		return -1;
	}
	if (!emmc_write_blocks(block, data, 1)) {
		return -1;
	}
	if (is_magic_commit_write) {
		/* A real new generation was just durably WRITTEN (not yet
		 * flushed) -- st_ab_session_check_write() itself already
		 * closed the session (single-use latch) exactly when it
		 * accepted this specific write, so reaching here means the
		 * commit genuinely landed, never merely attempted. Consumed
		 * by the NEXT real 'F' (durability barrier) -- docs step 18,
		 * "Flush", is always the immediately-following operation
		 * after step 17's magic write -- which performs the actual
		 * post-commit reload; see xfer_service()'s own 'F' handler
		 * and stem_song_post_commit_reload()'s own doc comment. */
		g_v11_commit_pending = true;
	}
	return 0;
}

/*
 * Slice C3 -- post-commit runtime reload, streamer-thread half. Called
 * from the real 'F' handler (xfer_service()) the instant a durability
 * flush succeeds AFTER a magic-committing write genuinely landed
 * (g_v11_commit_pending, set by xfer_v11_write() above). Re-reads both
 * index blocks fresh and re-runs the SAME real selector
 * (st_stix_read_library()) every other v1.1 path already uses -- never
 * assumes the just-committed record is the one now active; docs section 5
 * steps 19-21 require literally re-reading and re-selecting, not merely
 * trusting the write that was just performed.
 *
 * If the newly selected record names a song, reads its real sector 0 into
 * group 0 of every stem (this thread's own job -- the only thread that ever
 * touches flash) and validates it through a LOCAL, throwaway st_stream_t
 * instance -- never g_stem_stream itself, which stays audio-thread-
 * exclusive at every moment (see that struct's own doc comment) -- before
 * publishing the reload request. This is exactly the same "prove it's
 * real before ever selecting it" discipline boot itself uses in
 * streamer_thread's own boot block, just performed against a scratch
 * instance instead of the shared one, since only audio_thread may ever
 * construct the real g_stem_stream (see g_stem_reload_req's own doc
 * comment for the full handoff and why it is safe: g_playing is already 0
 * for the whole duration of any transfer session, so audio_thread is
 * provably not touching g_stem_group_bufs[]/the mailboxes anywhere in the window
 * this function can ever run).
 *
 * Only on successful validation does this fill g_stem_reload_pending and
 * publish g_stem_reload_req -- ANY failure (layout not ready, index
 * unreadable, no valid record, no song present, geometry/sector-0
 * validation failure) leaves the CURRENTLY selected song, if any,
 * completely undisturbed: this function never clears g_stem_song_selected
 * on failure, and audio_thread's own reload consumption performs an
 * independent second validation pass before it ever would either (see
 * looper_audio_block()'s own comment at its check site) -- an interrupted
 * or corrupt post-commit reload can degrade to "keep playing the OLD
 * song" but never to a torn/partial new one.
 */
/*
 * FORWARD DECLARATIONS -- the planar read helpers are defined down with the
 * streamer, where the rest of the flash path lives, but the post-commit
 * reload below runs on that same thread and needs group 0 of every stem
 * before it may publish a song. Declaring rather than moving keeps the read
 * path in one place.
 */
static bool stem_read_groups(uint32_t song_start_block, uint32_t groups,
			      uint32_t stem, uint32_t first_group,
			      uint32_t slot, uint32_t n);
static bool stem_prime_group0(uint32_t song_start_block, uint32_t groups);

static void stem_song_post_commit_reload(void)
{
	if (!g_v11_layout_ready) {
		return;
	}

	uint8_t idx_a[ST11_PHYSICAL_BLOCK_BYTES];
	uint8_t idx_b[ST11_PHYSICAL_BLOCK_BYTES];

	if (!emmc_read_blocks(g_v11_layout.index_a_start, idx_a, 1) ||
	    !emmc_read_blocks(g_v11_layout.index_b_start, idx_b, 1)) {
		return;
	}

	st_stix_library_state_t lib;

	st_stix_read_library(idx_a, idx_b, g_v11_layout.song_a_start, g_v11_layout.song_a_blocks,
			      g_v11_layout.song_b_start, g_v11_layout.song_b_blocks, &lib);

	if (lib.status != ST_STIX_LIB_OK || !(lib.active.flags & ST11_IX_FLAG_SONG_PRESENT)) {
		/* Nothing to reload for stem playback -- either genuinely no
		 * valid record yet (should never happen immediately after a
		 * real, just-verified commit, but this function fails closed
		 * rather than assuming) or a genuine index-only commit (INIT)
		 * with no song at all, which correctly has nothing to select. */
		return;
	}

	st_stream_t local_check;

	if (!st_stream_init(&local_check, lib.active.song_start_block, lib.active.song_block_count,
			     lib.active.frames, lib.active.sector_count, /*loop_enabled=*/true)) {
		return;
	}
	/* GROUP 0 OF ALL FOUR STEMS, and that IS the validation now.
	 *
	 * v1.1 read sector 0 and checked its 32-byte STSC header against the
	 * stream geometry. A v1.2 group header is eight bytes -- magic, the
	 * stem it claims to be, the span it covers -- and carries no timing at
	 * all, because timing was never sector data in the first place: the
	 * STIX record has always been the authoritative source and always won
	 * a disagreement. What the group header does carry is IDENTITY, which
	 * the sector header could not: stem_prime_group0() validates each of
	 * the four groups against the stem and span it was ASKED for, so a
	 * region miscomputed from the STIX geometry fails here rather than
	 * playing as another stem's audio. */
	if (!stem_prime_group0(lib.active.song_start_block, lib.active.sector_count)) {
		return;
	}
	(void)local_check;

	/* All real validation passed -- hand off to audio_thread. Plain-field
	 * write BEFORE the atomic release fences, matching g_stem_beat_
	 * timing's own already-proven idiom (see g_stem_reload_req's own doc
	 * comment). Clearing g_stem_song_selected here (not left to audio_
	 * thread) means no audio block in between can observe a stale
	 * "selected" flag alongside an already-in-flight reload -- the two
	 * atomic_set() calls are two separate operations, but stem_active
	 * (looper_audio_block()'s own gate) is already false throughout via
	 * g_playing==0, so no block ever computes stem_active=true from the
	 * old selection during this brief window regardless. */
	g_stem_reload_pending = lib.active;
	atomic_set(&g_stem_song_selected, 0);
	atomic_set(&g_stem_reload_req, 1);
}

/* STEM TAPE Slice T0's throughput benchmark ('Y' command,
 * xfer_bench_run()/bench_inactive_song_region()/bench_fill_pattern()/
 * bench_send_result()/bench_send_error(), plus the BENCH_MODE_ and
 * BENCH_MAX_ constants and s_bench_last_base/s_bench_last_count) is
 * REMOVED as of this change -- product decision (Slice C4): its whole
 * job was measuring real physical CDC/eMMC throughput numbers to design
 * the real bulk-upload path against, and that path now exists and is
 * wired in below (xfer_bulk_write_sector(), command 'U', docs/stem-tape-
 * bulk-upload-v1.md) -- the production firmware has no further use for
 * an unbounded, arbitrary-pattern eMMC writer once the real verified
 * path supersedes it. Not merely disabled: no definition is left to
 * link (see the strict persistence safety gate's ALLOWED_WRITE_FUNCS,
 * which now proves xfer_bench_run has ZERO call sites, the same
 * standard already applied to every other retired write path here).
 * docs/stem-tape-benchmark-t0.md is kept, with a status note, for the
 * measured throughput numbers it recorded -- the numbers that shaped
 * the real bulk-upload contract's own design. */

/*
 * Bulk verified-sector upload (docs/stem-tape-bulk-upload-v1.md), Slice C2
 * -- the REAL production replacement for the old per-512-byte-block song-
 * data write path. Command 'U': receives one complete 8192-byte Stem Tape
 * v1.1 sector, validates it, writes all 16 physical blocks in ONE real
 * multi-block eMMC burst (emmc_write_blocks(..., 16) -> CMD25, not sixteen
 * independent CMD24s), reads the SAME 16 blocks back in one burst, and
 * only acknowledges success once the read-back bytes' own CRC-32 matches
 * what was sent -- replacing the separate 512-byte-at-a-time read-back
 * pass entirely (see st_bulk_xfer.h's own doc for the full wire contract).
 *
 * SAFETY: identical boundary to xfer_v11_write()'s own -- every block is
 * proven safe by st_ab_session_check_write() against g_v11_session's
 * CURRENT frozen destination pair before a single byte reaches eMMC (loop
 * below, one call per physical block, exactly like xfer_v11_write() itself
 * does for every 'W'). st_bulk_xfer.h's own sequence/bounds check
 * (g_v11_bulk_seq) is an earlier, cheap, redundant-but-harmless fast-
 * rejection -- never the authoritative gate. This command is NEVER used
 * for index blocks: 'W' remains the sole path for the STIX v2 index
 * region and xfer_v11_write()'s own magic-commit detection, both
 * completely untouched by this function.
 *
 * IDEMPOTENCY: the sequence tracker only ever advances after a REAL
 * write+read-back+CRC round trip fully succeeds for a genuinely NEW
 * sector (st_bulk_seq_check() == ST_BULK_SEQ_NEW) -- never on the
 * strength of an acknowledgement alone. A lost-ACK retry
 * (ST_BULK_SEQ_RETRY) reprocesses the exact same write+verify pipeline in
 * full and does not advance the tracker again -- writing the same bytes
 * to the same block twice is safe by construction (eMMC program is not
 * order-sensitive across identical data).
 *
 * BACKPRESSURE / FRAMING: the payload always follows the request header
 * in ONE continuous host transmission (matching the classic 'W' verb's
 * own single-shot address+data convention), so this function ALWAYS
 * drains the wire-fixed ST_BULK_PAYLOAD_BYTES (8192) after a successfully
 * received header, regardless of what the header's own (untrusted) declared
 * payload_len says -- a malformed header can therefore never leave unread
 * payload bytes behind to misframe the next command. Reuses the SAME
 * cdc_rx() backpressure-correct receive loop every other verb uses (see
 * that function's own doc); the CDC ring-overflow counter added in the T0
 * slice (g_cdc_rx_dropped_bytes) is checked across the exact span of this
 * receive, so an overflow during THIS payload is reported precisely
 * (ERR_CDC_OVERFLOW) rather than silently risking a corrupted accept.
 *
 * RAM: reuses xfer_scratch() (the SAME ST11_SECTOR_BYTES buffer
 * xfer_v11_write()'s own verify-before-commit step uses) for both the
 * received payload AND the read-back bytes -- zero new large static
 * allocation. Safe: this function and xfer_v11_write() are never both
 * mid-flight at once (xfer_service() services exactly one command at a
 * time), and by the time this function's own read-back overwrites the
 * buffer, the received payload's CRC has already been captured into a
 * plain local (hdr.payload_crc32), so nothing of the payload is still
 * needed from the buffer itself.
 */
/* feed_wdt() is already forward-declared above (see cdc_rx()'s own doc
 * comment) -- no second declaration needed here.
 *
 * Returns -1 on every rejected/failed path (never reaching the real eMMC
 * write or a magic-committing anything past it), 0 on the one accepted
 * path -- the SAME `return -1;` early-guard idiom xfer_v11_write() itself
 * uses, deliberately, so this function fits the strict persistence safety
 * gate's own existing, proven verification pattern (see .github/scripts/
 * stemtape_player_safety_gate.py's ALLOWED_WRITE_FUNCS entry for
 * "xfer_bulk_write_sector") rather than asking that gate to special-case a
 * different shape. The caller (xfer_service()) does not use the return
 * value -- every path already sends its own response over CDC (or, for
 * the one un-parseable-header case, a raw resync byte) before returning.
 *
 * __attribute__((noinline, noclone)): confirmed necessary by a real CI
 * run, not anticipated -- this function has exactly one, always-taken
 * call site (the 'U' branch in xfer_service()'s dispatcher), the same
 * -Os single-call-site inlining shape xfer_v11_refresh_session()/
 * xfer_v11_send_caps() already carry this exact attribute for (see
 * either one's own comment for the full reasoning); without it, -Os
 * folded this function's body entirely into its caller, leaving no
 * standalone symbol for the runtime symbol-presence gate to find AND --
 * more importantly -- no standalone symbol for the strict persistence
 * safety gate's disassembly-based ALLOWED_WRITE_FUNCS attribution to
 * enclose this function's own emmc_write_blocks() call site in. Keeping
 * this specific function un-inlined is not merely a test-passing
 * convenience: it is what keeps this safety-boundary function's own
 * machine code independently auditable in the final binary at all.
 * Trailing, same-line attribute placement matches this file's own
 * established convention for this exact function-signature shape (see
 * xfer_v11_write()'s own identical placement, which the strict
 * persistence safety gate's parser is proven against). */
/* The classic/v1.1 'W' handler already trusts EXACTLY 4000ms as the real,
 * proven-in-production device-side receive window for one EMMC_BLOCK_SIZE
 * (512-byte) block (see its own cdc_rx() call site above) -- physically
 * measured against this same real hardware and USB CDC link over years of
 * classic Tape Looper use, not a number this project invented. A bulk
 * sector's payload is exactly ST_BULK_PAYLOAD_BYTES / EMMC_BLOCK_SIZE (16)
 * times as much data to receive in one call as that already-proven
 * precedent covers, so this scales the SAME trusted per-block number by
 * that exact, structural ratio -- not a new, independently-chosen value.
 * This is a CEILING for the rare slow case, not the expected duration:
 * cdc_rx() returns the moment all bytes have actually arrived, so a
 * healthy transfer completes in whatever real time it takes and never
 * waits anywhere near this bound. Safe to use here specifically because
 * cdc_rx() itself now feeds the watchdog on every poll iteration (see its
 * own doc comment) -- before that fix, anything above the WDT's own
 * 4000ms window risked a hard reset partway through a legitimately slow
 * (but otherwise succeeding) transfer, which is a strictly worse outcome
 * than a clean, retryable ERR_TIMEOUT_PAYLOAD response. */
#define ST_BULK_PAYLOAD_TIMEOUT_MS (4000 * (ST_BULK_PAYLOAD_BYTES / EMMC_BLOCK_SIZE))

static int xfer_bulk_write_sector(void)
	__attribute__((noinline, noclone));
static int xfer_bulk_write_sector(void)
{
	uint8_t hdr_bytes[ST_BULK_REQ_HEADER_BYTES];

	if (!cdc_rx(hdr_bytes, sizeof hdr_bytes, 2000)) {
		/* The request header itself never fully arrived -- there is no
		 * real seq/dest_block to echo yet, so this is NOT the structured
		 * 14-byte response case; matches 'R'/'W's own established
		 * xfer_resync() idiom for "not enough is known yet to answer". */
		xfer_resync('E');
		return -1;
	}

	st_bulk_req_header_t hdr;

	st_bulk_parse_header(hdr_bytes, &hdr);
	feed_wdt();

	/* From here on seq/dest_block are known -- EVERY remaining path sends
	 * the real, structured response with them correctly echoed (see this
	 * function's own doc comment on why the payload is always drained
	 * next, regardless of what is wrong with the header). */
	uint32_t dropped_before = (uint32_t)atomic_get(&g_cdc_rx_dropped_bytes);
	bool payload_ok = cdc_rx(xfer_scratch(), ST_BULK_PAYLOAD_BYTES, ST_BULK_PAYLOAD_TIMEOUT_MS);
	uint32_t dropped_after = (uint32_t)atomic_get(&g_cdc_rx_dropped_bytes);

	feed_wdt();

	/* payload_ok false means cdc_rx() gave up before ST_BULK_PAYLOAD_BYTES
	 * ever fully arrived -- the RX ring may still hold a partial fragment
	 * of that payload, and (if the host was still genuinely mid-write when
	 * we gave up) more of it can keep landing for a little while after this
	 * point too. Left alone, either would misframe the very next header
	 * this device tries to parse -- on the host's retry, its fresh 17-byte
	 * request header would be read starting from wherever this abandoned
	 * payload's leftovers happen to end, not from a real header boundary,
	 * corrupting hdr.seq/hdr.dest_block/hdr.payload_len with garbage and
	 * cascading into a nonsense response. This is exactly the class of
	 * problem xfer_resync() exists for elsewhere in this file (see its own
	 * doc comment); reuse its same drain-to-clean-boundary step here rather
	 * than inventing a different one, even though this path -- unlike
	 * xfer_resync()'s callers -- still owes the host the real, structured
	 * 14-byte response below (with seq/dest_block correctly echoed) instead
	 * of a raw resync error byte, since the header already parsed fine.
	 * Applied unconditionally on !payload_ok, before the version/length
	 * checks below get a chance to return first: those checks read hdr
	 * fields that were parsed before this drain and are unaffected by it,
	 * so whichever specific error status ends up being reported, the ring
	 * is left clean for it. One immediate pass only, matching xfer_resync()
	 * exactly -- it does not wait for more bytes to arrive, since this
	 * function has already waited up to ST_BULK_PAYLOAD_TIMEOUT_MS and must
	 * return promptly now; a host still transmitting long after that point
	 * is a transport-level problem no single bounded drain here can fully
	 * cover. */
	if (!payload_ok || dropped_after != dropped_before) {
		uint8_t dump;

		while (ring_buf_get(&g_cdc_rx, &dump, 1) == 1) {
		}
	}

	if (hdr.version != ST_BULK_PROTO_VERSION) {
		uint8_t resp[ST_BULK_RESP_BYTES];

		st_bulk_build_response(ST_BULK_ERR_UNSUPPORTED_VERSION, hdr.seq, hdr.dest_block, 0u, resp);
		cdc_tx(resp, sizeof resp);
		return -1;
	}
	if (hdr.payload_len != ST_BULK_PAYLOAD_BYTES) {
		uint8_t resp[ST_BULK_RESP_BYTES];

		st_bulk_build_response(ST_BULK_ERR_BAD_LENGTH, hdr.seq, hdr.dest_block, 0u, resp);
		cdc_tx(resp, sizeof resp);
		return -1;
	}
	/* CDC_OVERFLOW is deliberately checked BEFORE TIMEOUT_PAYLOAD, and the
	 * order matters for real-world diagnosis, not just tidiness: when the
	 * ISR has dropped bytes, cdc_rx() is left waiting for data that no
	 * longer exists and ALWAYS eventually reports a timeout too. With the
	 * timeout tested first, a genuine overflow could only ever surface as
	 * ERR_TIMEOUT_PAYLOAD -- which is exactly what happened across three
	 * real physical upload attempts, each one pointing the investigation at
	 * "the transfer is too slow" when the truth was "the buffer was far too
	 * small and most of the sector was thrown away". Overflow is the more
	 * specific and strictly more informative diagnosis, so it wins; a
	 * timeout is only reported when no bytes were dropped at all, which now
	 * genuinely does mean the data never arrived. */
	if (dropped_after != dropped_before) {
		uint8_t resp[ST_BULK_RESP_BYTES];

		st_bulk_build_response(ST_BULK_ERR_CDC_OVERFLOW, hdr.seq, hdr.dest_block, 0u, resp);
		cdc_tx(resp, sizeof resp);
		return -1;
	}
	if (!payload_ok) {
		uint8_t resp[ST_BULK_RESP_BYTES];

		st_bulk_build_response(ST_BULK_ERR_TIMEOUT_PAYLOAD, hdr.seq, hdr.dest_block, 0u, resp);
		cdc_tx(resp, sizeof resp);
		return -1;
	}

	/* Validate the incoming CRC before touching eMMC at all. */
	uint32_t recv_crc = st_crc32_compute(xfer_scratch(), ST_BULK_PAYLOAD_BYTES);

	if (recv_crc != hdr.payload_crc32) {
		uint8_t resp[ST_BULK_RESP_BYTES];

		st_bulk_build_response(ST_BULK_ERR_CRC_MISMATCH, hdr.seq, hdr.dest_block, 0u, resp);
		cdc_tx(resp, sizeof resp);
		return -1;
	}

	if (!g_v11_layout_ready) {
		uint8_t resp[ST_BULK_RESP_BYTES];

		st_bulk_build_response(ST_BULK_ERR_LAYOUT_NOT_READY, hdr.seq, hdr.dest_block, 0u, resp);
		cdc_tx(resp, sizeof resp);
		return -1;
	}
	if (!g_v11_session.open || g_v11_session.closed) {
		uint8_t resp[ST_BULK_RESP_BYTES];

		st_bulk_build_response(g_v11_session.closed ? ST_BULK_ERR_SESSION_CLOSED : ST_BULK_ERR_NO_SESSION,
					hdr.seq, hdr.dest_block, 0u, resp);
		cdc_tx(resp, sizeof resp);
		return -1;
	}

	st_bulk_seq_check_t seqchk = st_bulk_seq_check(&g_v11_bulk_seq, hdr.seq, hdr.dest_block);

	if (seqchk != ST_BULK_SEQ_NEW && seqchk != ST_BULK_SEQ_RETRY) {
		st_bulk_status_t status = (seqchk == ST_BULK_SEQ_DEST_MISMATCH)   ? ST_BULK_ERR_DEST_MISMATCH
					   : (seqchk == ST_BULK_SEQ_OUT_OF_BOUNDS) ? ST_BULK_ERR_OUT_OF_BOUNDS
										    : ST_BULK_ERR_OUT_OF_SEQUENCE;
		uint8_t resp[ST_BULK_RESP_BYTES];

		st_bulk_build_response(status, hdr.seq, hdr.dest_block, 0u, resp);
		cdc_tx(resp, sizeof resp);
		return -1;
	}

	/* THE authoritative bounds/active-region gate: one call per physical
	 * block, exactly like xfer_v11_write() itself uses for every 'W' --
	 * never write a single byte to eMMC before EVERY block in this
	 * sector has passed. A song-region block is never magic-interpreted
	 * (st_ab_session_check_write()'s own doc: `data` is only examined for
	 * the frozen INDEX destination), and this pure per-block check has no
	 * side effects on non-index blocks (verified against st_ab_session.c's
	 * own is_frozen_song branch), so calling it 16 times per sector --
	 * including on every retry -- is always safe. */
	for (uint32_t k = 0; k < ST_BULK_BLOCKS_PER_SECTOR; k++) {
		st_ab_write_check_t chk = st_ab_session_check_write(
			&g_v11_session, hdr.dest_block + k, xfer_scratch() + k * ST11_PHYSICAL_BLOCK_BYTES);

		if (chk != ST_AB_WRITE_OK) {
			st_bulk_status_t status =
				(chk == ST_AB_WRITE_ERR_ACTIVE_REGION) ? ST_BULK_ERR_ACTIVE_REGION
									: ST_BULK_ERR_OUTSIDE_FROZEN_PAIR;
			uint8_t resp[ST_BULK_RESP_BYTES];

			st_bulk_build_response(status, hdr.seq, hdr.dest_block, 0u, resp);
			cdc_tx(resp, sizeof resp);
			return -1;
		}
	}

	/* The real multi-block eMMC program -- ONE burst (CMD25), not sixteen
	 * independent single-block CMD24s. */
	if (!emmc_write_blocks(hdr.dest_block, xfer_scratch(), ST_BULK_BLOCKS_PER_SECTOR)) {
		uint8_t resp[ST_BULK_RESP_BYTES];

		st_bulk_build_response(ST_BULK_ERR_EMMC_WRITE_FAIL, hdr.seq, hdr.dest_block, 0u, resp);
		cdc_tx(resp, sizeof resp);
		return -1;
	}

	feed_wdt();

	/* Read the SAME 16 blocks back -- one real multi-block burst, reusing
	 * the SAME scratch buffer (the sent payload's own CRC is already
	 * captured in hdr.payload_crc32/recv_crc; nothing more is needed from
	 * those bytes). This IS the read-back verification the wire contract
	 * replaces the old separate 512-byte read-back pass with. */
	if (!emmc_read_blocks(hdr.dest_block, xfer_scratch(), ST_BULK_BLOCKS_PER_SECTOR)) {
		uint8_t resp[ST_BULK_RESP_BYTES];

		st_bulk_build_response(ST_BULK_ERR_EMMC_READBACK_FAIL, hdr.seq, hdr.dest_block, 0u, resp);
		cdc_tx(resp, sizeof resp);
		return -1;
	}

	feed_wdt();

	uint32_t verified_crc = st_crc32_compute(xfer_scratch(), ST_BULK_PAYLOAD_BYTES);

	if (verified_crc != hdr.payload_crc32) {
		uint8_t resp[ST_BULK_RESP_BYTES];

		st_bulk_build_response(ST_BULK_ERR_READBACK_CRC_MISMATCH, hdr.seq, hdr.dest_block, verified_crc, resp);
		cdc_tx(resp, sizeof resp);
		return -1;
	}

	/* Only now, after a FULLY verified round trip, advance the sequence
	 * tracker -- and only for a genuinely NEW sector; a retry must never
	 * advance it again (see st_bulk_seq_advance()'s own doc). */
	if (seqchk == ST_BULK_SEQ_NEW) {
		st_bulk_seq_advance(&g_v11_bulk_seq, hdr.seq);
		/* Fold THIS sector's read-back bytes into the session's running
		 * commit-verification checksums while they are already in RAM and
		 * already proven (above) to be exactly what storage returned. This
		 * is what lets the eventual magic write commit instantly instead of
		 * re-reading the entire song a second time inside that one command
		 * -- for a real 248.5 MiB song that second pass is over half a
		 * million block reads, which overran both the host's acknowledgement
		 * timeout and the hardware watchdog. Same safety claim, same bytes,
		 * same derivation; only the timing differs. See
		 * st_ab_session_accumulate_sector()'s own doc comment. */
		st_ab_session_accumulate_sector(&g_v11_session, hdr.seq, xfer_scratch());
	}

	uint8_t resp[ST_BULK_RESP_BYTES];

	st_bulk_build_response(ST_BULK_OK, hdr.seq, hdr.dest_block, verified_crc, resp);
	cdc_tx(resp, sizeof resp);
	return 0;
}

/* STEM TAPE PHASE 1: g_xfer_dirty (which track regions the host wrote this
 * transfer session, used by the classic committer to merge host writes into
 * the durable index) and xfer_commit() (the classic looper's write-cache-to-
 * durable-index committer) is removed for this phase -- there is no write
 * path left that could ever need committing (see the read-only 'W'/'F'/'X'
 * handlers above and the removed 'Z' verb). Phase 2 reintroduces a real,
 * validated commit. */

/* The block-transfer protocol, serviced from the streamer (the only eMMC user).
 * OUT of transfer mode: scan the RX stream for the 8-byte enter-magic.
 * IN transfer mode: run ONE command per call ('P'ing/'R'ead/'W'rite/'F'lush/e'X'it),
 * auto-committing + auto-exiting after 15 s with no command so a dropped page can't
 * wedge it or strand an upload in volatile cache. */
static void xfer_service(void)
{
	static const uint8_t MAGIC[8] = { 'S','P','1','X','F','E','R','!' };
	static uint8_t  m;
	static int64_t  last;

	if (!g_xfer_mode) {
		uint8_t b;
		while (ring_buf_get(&g_cdc_rx, &b, 1) == 1) {
			m = (b == MAGIC[m]) ? (uint8_t)(m + 1) : (b == MAGIC[0] ? 1u : 0u);
			if (m == 8u) {
				m = 0;
				/* Don't freeze the streamer mid-take: if a recording is still
				 * being captured or flushed, finalize it first (the audio thread
				 * promotes it + the streamer drains the ring and persists the
				 * index). Enter on a later magic -- the host's handshake retries,
				 * and a take finalizes in well under that window. */
				bool busy = (g_rec_track >= 0) || g_meta_save_req;
				for (int t = 0; t < NTRK; t++)
					if (trk[t].state == TS_REC || trk[t].state == TS_DONE) busy = 1;
				if (busy) {
					g_stop_req = 1;
					break;
				}
				/* Cleared BEFORE the flag is raised: an
				 * acknowledgement left over from a previous
				 * transfer must never stand in for this one. */
				atomic_set(&g_xfer_audio_quiesced, 0);
				atomic_set(&g_xfer_stream_quiesced, 0);
				/* AND THE PINS GO, BEFORE ANY BYTE IS WRITTEN.
				 * The upload verify scratch IS the last pin
				 * buffer (see xfer_scratch()), so a pin left
				 * claiming residency across a transfer would be
				 * claiming bytes the upload is about to
				 * overwrite. Dropped here, at the same point
				 * the acknowledgements are cleared, so the two
				 * halves of the same invariant cannot drift
				 * apart. Refilled by the streamer's own next
				 * pass after the transfer, from flash. */
				stem_loop_pins_drop();
				g_xfer_mode = 1;
				g_playing = 0;           /* pause the transport during transfer */
				last = k_uptime_get();
				break;
			}
		}
		return;
	}

	/*
	 * NOTHING DISPATCHES UNTIL BOTH THREADS HAVE ACKNOWLEDGED.
	 *
	 * Transfer mode has just been entered; the audio thread and the streamer
	 * each set their bit at the point they provably stop touching stem
	 * buffers. Until both have, a command that writes shared storage could
	 * still land under a thread mid-block. The command byte is left in the
	 * ring and read on a later pass -- the host's own handshake retries
	 * anyway, and both acknowledgements arrive within one audio block
	 * (5.3 ms) and one streamer pass (1 ms) of the flag being raised.
	 *
	 * Deliberately gating the WHOLE dispatch rather than only the commands
	 * that touch the shared buffer: a future command would otherwise have to
	 * remember to opt in, and forgetting is silent.
	 */
	if (!xfer_quiesced()) {
		/*
		 * AND IT MUST NOT BE ABLE TO HANG HERE. While gated no command
		 * is consumed, so the idle timeout below is unreachable -- a
		 * thread that never acknowledged would strand the device in a
		 * transfer mode that does nothing at all. A second is orders of
		 * magnitude longer than the 5.3 ms audio block and 1 ms streamer
		 * pass both acknowledgements come from, so reaching this means
		 * something is genuinely wrong, and returning to ordinary
		 * playback is the better failure.
		 */
		if (k_uptime_get() - last > 1000) {
			g_slot_switch_req = 1;
			g_xfer_mode = 0;
		}
		return;
	}

	uint8_t cmd;
	if (ring_buf_get(&g_cdc_rx, &cmd, 1) != 1) {            /* idle: exit on timeout */
		if (k_uptime_get() - last > 15000) {
			/* STEM TAPE PHASE 1: no xfer_commit() here -- there is nothing
			 * that could be "stranded in cache" (the 'W' verb never writes,
			 * see above), so there is nothing to persist on timeout either. */
			g_slot_switch_req = 1;                 /* reload tracks for the active song (read-only) */
			g_xfer_mode = 0;
		}
		return;
	}
	last = k_uptime_get();

	if (cmd == 'P') {                                      /* ping -> magic + layout */
		uint8_t r[4 + 6 * 4];
		memcpy(r, "SP1!", 4);
		uint32_t info[6] = { EMMC_BLOCK_SIZE, NUM_SLOTS, NTRK,
				     SLOT0_BLOCK, TRACK_BLOCKS, META_MAGIC };
		memcpy(r + 4, info, sizeof info);
		cdc_tx(r, sizeof r);
	} else if (cmd == 'R' || cmd == 'W') {                 /* read one block / write one block (v1.1-region only) */
		uint8_t a[4];
		if (!cdc_rx(a, 4, 1000)) { xfer_resync(cmd == 'R' ? 'e' : 'E'); return; }
		uint32_t blk = (uint32_t)a[0] | ((uint32_t)a[1] << 8) |
			       ((uint32_t)a[2] << 16) | ((uint32_t)a[3] << 24);
		uint32_t total = SLOT0_BLOCK + (uint32_t)NUM_SLOTS * NTRK * TRACK_BLOCKS;
		static uint8_t sec[EMMC_BLOCK_SIZE];
		if (cmd == 'R') {
			/* Reads are always safe: the classic looper's own block
			 * address space (unchanged, read-only-safe) UNION the v1.1
			 * region (docs section 5 steps 9-10/15-16 explicitly read
			 * back what was just written, to verify it byte-for-byte
			 * before ever trusting it -- 'R' has to reach those blocks
			 * for that to be possible at all). */
			bool in_v11_region =
				g_v11_layout_ready && st11_region_of_block(&g_v11_layout, blk) != ST11_REGION_NONE;
			bool ok = (blk < total || in_v11_region) && emmc_read_blocks(blk, sec, 1);
			uint8_t h = ok ? 'r' : 'e';
			cdc_tx(&h, 1);
			if (ok) cdc_tx(sec, EMMC_BLOCK_SIZE);
		} else {
			/* Stem Tape v1.1: the classic looper's own block address
			 * space stays exactly as read-only-safe as Phase 1 left it
			 * -- xfer_v11_write() is bounded by its own body to the
			 * v1.1 region, and only accepts a block there when
			 * st_ab_session_check_write() proves it safe against the
			 * CURRENT session's frozen destination pair. */
			if (!cdc_rx(sec, EMMC_BLOCK_SIZE, 4000)) {
				xfer_resync('E');
				return;
			}
			uint8_t h = (xfer_v11_write(blk, sec) == 0) ? (uint8_t)ST11_WRITE_ACK : (uint8_t)'E';
			cdc_tx(&h, 1);
		}
	} else if (cmd == 'M') {                               /* READ-SIZE SWEEP -- read-only measurement */
		/*
		 * WHAT THIS ANSWERS, and why it is worth a command.
		 *
		 * Per-track reverse playback needs a reversed stem to read from
		 * its own position. In the v1.1 layout all four stems are
		 * interleaved in one frame, so that costs a whole extra SECTOR
		 * stream -- 143% of a read engine that only has 100%. Storing
		 * each stem in its own contiguous plane would let a reversed
		 * stem fetch only its own quarter, but ONLY IF a quarter-size
		 * read actually costs about a quarter.
		 *
		 * That hinges entirely on the 1763 us start-bit hunt: per BLOCK
		 * and the feature is affordable (78% duty), per READ and it is
		 * not (153%). sp1_emmc.c's loop says per block. This measures
		 * it, because the change it would justify re-encodes every
		 * stored song and cannot be walked back cheaply.
		 *
		 * READ-ONLY. Nothing here writes, erases or flushes; it reads
		 * blocks 'R' is already allowed to read. It is also only
		 * reachable from transfer mode, where playback is stopped, so
		 * it cannot steal bandwidth from a live stream and skew its own
		 * measurement.
		 */
		/* 1..16 blocks: a full sector, a stem plane (4), and the sizes
		 * either side of it, so the fit has spread on both sides of the
		 * quarter-size read the whole question is about. */
		static const uint32_t k_sizes[] = { 1u, 2u, 4u, 8u, 16u };
		const uint32_t n_sizes = 5u;
		const uint32_t total_blocks =
			SLOT0_BLOCK + (uint32_t)NUM_SLOTS * NTRK * TRACK_BLOCKS;
		uint8_t ack = 'm';
		uint32_t si;

		cdc_tx(&ack, 1);
		printk("STEMRC sweep begin (read-only, %u reps per size)\n",
		       (unsigned)ST_RC_SWEEP_REPS);

		for (si = 0; si < n_sizes; si++) {
			const uint32_t nb = k_sizes[si];
			uint64_t sum_cyc = 0;
			uint32_t worst_cyc = 0, ok = 0, rep;
			uint32_t hunt = 0, dma = 0, crc = 0;

			for (rep = 0; rep < ST_RC_SWEEP_REPS; rep++) {
				/* Step the address every rep so the card's own
				 * read cache cannot serve a repeat and report a
				 * cost the streamer will never see. Wrapped
				 * inside the region 'R' already permits. */
				const uint32_t span = total_blocks - SLOT0_BLOCK;
				const uint32_t blk = SLOT0_BLOCK +
					((rep * ST_RC_SECTOR_BLOCKS) %
					 (span - ST_RC_SECTOR_BLOCKS));
				uint32_t c0, c1;

				feed_wdt();
				c0 = DWT->CYCCNT;
				if (!emmc_read_blocks(blk, xfer_scratch(),
						       nb)) {
					continue;
				}
				c1 = DWT->CYCCNT;
				{
					const uint32_t d = c1 - c0;

					sum_cyc += d;
					if (d > worst_cyc) {
						worst_cyc = d;
					}
				}
				ok++;
				/* The driver republishes these on every call;
				 * the last one is representative. */
				hunt = emmc_dbg_rd_hunt_us;
				dma  = emmc_dbg_rd_dma_us;
				crc  = emmc_dbg_rd_crc_us;
			}

			if (ok == 0u) {
				printk("STEMRC blocks=%u FAILED (no successful reads)\n",
				       nb);
				continue;
			}
			/* 64 MHz core clock -> microseconds. */
			printk("STEMRC blocks=%u n=%u avg_us=%u worst_us=%u "
			       "hunt_us=%u dma_us=%u crc_us=%u\n",
			       nb, ok,
			       (uint32_t)((sum_cyc / ok) / 64u),
			       worst_cyc / 64u, hunt, dma, crc);
			feed_wdt();
		}
		printk("STEMRC sweep end -- fit these with st_readcost_fit(); "
		       "hunt_us scaling with blocks= is the whole question\n");
	} else if (cmd == 'F') {                               /* real durability barrier (docs section 1, 10 item 6) */
		/* emmc_cache_flush() blocks until the card's internal volatile
		 * write cache actually programs to NAND (EXT_CSD FLUSH_CACHE) --
		 * the SAME real hardware primitive stop_and_flush() already uses
		 * at power-off, and the classic looper's own xfer_do_commit()
		 * used identically before this migration. Only acks success
		 * (ST11_FLUSH_ACK, 0x66); a failed flush must never claim
		 * durability it didn't actually achieve. Safe to block the bus
		 * here: a transfer is audio-paused throughout (see
		 * xfer_service()'s own comment) -- exactly the "SAFE points, not
		 * mid-record" rule emmc_cache_flush()'s own doc requires.
		 * Unconditional (not gated on g_v11_layout_ready or which region
		 * was last written): 'F' flushes the WHOLE card's write cache,
		 * not a specific region, matching the unchanged base transport's
		 * own scope (docs section 1: 'F' takes no payload). */
		uint8_t h = emmc_cache_flush() ? (uint8_t)ST11_FLUSH_ACK : (uint8_t)'E';
		cdc_tx(&h, 1);

		/* Slice C3: this flush ack is sent first, unconditionally,
		 * exactly as before -- the reload below never delays or
		 * changes it. docs section 5 step 18 ("Flush") always
		 * immediately follows step 17's magic write, so a real 'F'
		 * that lands right after a genuine commit (g_v11_commit_
		 * pending, set by xfer_v11_write()) is the natural, wire-
		 * contract-compatible trigger point for the post-commit
		 * reload -- no new command verb needed. One-shot: cleared
		 * here regardless of the reload's own outcome, so a LATER,
		 * unrelated 'F' (e.g. one more flush during ordinary song-
		 * region writes) never re-triggers it. */
		if (h == (uint8_t)ST11_FLUSH_ACK && g_v11_commit_pending) {
			g_v11_commit_pending = false;
			stem_song_post_commit_reload();
		}
	} else if (cmd == 'X') {                               /* Phase 1: exit transfer mode -- never commits */
		g_slot_switch_req = 1;                         /* reload tracks for the active song (read-only) */
		g_xfer_mode = 0;
		uint8_t h = 'x';
		cdc_tx(&h, 1);

	/* ---- Stem Tape v1.1 (docs/stem-tape-transfer-v1.1.md section 1-2):
	 * 'Q' is the ONLY new wire command this contract adds -- v1.1 reuses
	 * 'P'/'R'/'W'/'F'/'X' above verbatim, unlike the old, now-deleted
	 * Gate 2 contract's own extra verbs (V/B/S/K/C/A/D/I). Answering 'Q'
	 * also (re)opens g_v11_session, since a real companion always
	 * re-queries Q immediately before writing (docs section 5 step 1) --
	 * see xfer_v11_send_caps()/xfer_v11_refresh_session() above for the
	 * fail-closed details. Silence (no reply at all) when
	 * g_v11_layout_ready is false is the documented "unsupported
	 * firmware, stay read-only" signal (docs section 2) -- not a bug. */
	} else if (cmd == 'Q') {                               /* v1.1 capability query -> STCP */
		xfer_v11_refresh_session();
		xfer_v11_send_caps();

	/* Bulk verified-sector upload (docs/stem-tape-bulk-upload-v1.md),
	 * Slice C2: 'U' is a NEW verb, additive only -- P/Q/R/W/F/X above are
	 * byte-for-byte unchanged, and 'W' remains the sole path for STIX
	 * index records. See xfer_bulk_write_sector()'s own doc comment
	 * (immediately above xfer_service()) for the full wire protocol,
	 * safety argument, and idempotency rules. 'Y' (the Slice T0 throughput
	 * benchmark) is RETIRED as of Slice C4 -- see this file's own removal
	 * note where xfer_bench_run() used to be defined, immediately above
	 * this function's own doc comment -- so 'U' is the last real verb in
	 * this dispatcher; an unrecognized command byte falls through with no
	 * branch taken, the same fail-silent behavior this dispatcher has
	 * always had for any command it doesn't know. */
	} else if (cmd == ST_BULK_CMD) {
		(void)xfer_bulk_write_sector();
	}
}
#endif /* SP1_XFER_ENABLE */

/* =====================================================================
 * STORAGE CODEC pack/unpack
 * Place this entire block in main.c just BEFORE streamer_thread()
 * (above `static void streamer_thread(void *a,...)` at main.c:1523).
 * SP1_CODEC, SAMP_PER_BLK, EMMC_BLOCK_SIZE are all in scope there.
 *
 *  codec_pack  : int16 ring -> packed flash bytes (ENCODE), nblk*512 bytes out
 *  codec_unpack: packed flash bytes -> int16 ring (DECODE), nblk*512 bytes in
 *
 * Args:
 *   ring       : the int16 ring base (g_rring for write, trk[].pring for read)
 *   ring_mask  : RRING_MASK (write) or RING_MASK (read) — power of two, sample-domain
 *   start      : ring sample offset of the FIRST sample (already & ring_mask'd by caller)
 *   flash      : the linear 512*nblk-byte batch buffer (batchbuf)
 *   nblk       : number of 512-byte flash blocks
 * Each block holds exactly SAMP_PER_BLK int16 samples. The caller guarantees
 * `start` is block-aligned in the ring, so each block's run wraps the ring at
 * most once (same invariant the original memcpy pairs used).
 * ===================================================================== */

#if SP1_CODEC == SP1_CODEC_PCM
/* ---- PCM: memcpy-equivalent (handles the single ring wrap) ---------------- */
static void codec_pack(const int16_t *ring, uint32_t ring_mask, uint32_t start,
                       uint8_t *flash, uint32_t nblk)
{
	int16_t *out = (int16_t *)flash;
	uint32_t ntot = nblk * SAMP_PER_BLK;
	uint32_t ring_samps = ring_mask + 1u;
	uint32_t run1 = ring_samps - start;
	if (run1 > ntot) run1 = ntot;
	memcpy(out, &ring[start], run1 * 2u);
	if (ntot > run1)
		memcpy(out + run1, &ring[0], (ntot - run1) * 2u);
}
static void codec_unpack(int16_t *ring, uint32_t ring_mask, uint32_t start,
                         const uint8_t *flash, uint32_t nblk)
{
	const int16_t *in = (const int16_t *)flash;
	uint32_t ntot = nblk * SAMP_PER_BLK;
	uint32_t ring_samps = ring_mask + 1u;
	uint32_t run1 = ring_samps - start;
	if (run1 > ntot) run1 = ntot;
	memcpy(&ring[start], in, run1 * 2u);
	if (ntot > run1)
		memcpy(&ring[0], in + run1, (ntot - run1) * 2u);
}

#elif SP1_CODEC == SP1_CODEC_ULAW
/* ---- G.711 u-law, 8-bit, 2:1 --------------------------------------------- */
#define ULAW_BIAS 0x84
#define ULAW_CLIP 32635
static inline uint8_t ulaw_encode(int16_t pcm)
{
	int sign = (pcm >> 8) & 0x80;
	int s = pcm;
	if (sign) s = -s;
	if (s > ULAW_CLIP) s = ULAW_CLIP;
	s += ULAW_BIAS;
	int exp = 7;
	for (int em = 0x4000; (s & em) == 0 && exp > 0; exp--, em >>= 1) { }
	int mant = (s >> (exp + 3)) & 0x0F;
	return (uint8_t)(~(sign | (exp << 4) | mant));
}
static inline int16_t ulaw_decode(uint8_t u)
{
	u = ~u;
	int sign = u & 0x80;
	int exp  = (u >> 4) & 0x07;
	int mant = u & 0x0F;
	int s = ((mant << 3) + ULAW_BIAS) << exp;
	s -= ULAW_BIAS;
	return (int16_t)(sign ? -s : s);
}
/* one u-law byte per sample; SAMP_PER_BLK == 512 == EMMC_BLOCK_SIZE */
static void codec_pack(const int16_t *ring, uint32_t ring_mask, uint32_t start,
                       uint8_t *flash, uint32_t nblk)
{
	uint32_t ntot = nblk * SAMP_PER_BLK;
	uint32_t pos = start;
	for (uint32_t i = 0; i < ntot; i++) {
		flash[i] = ulaw_encode(ring[pos]);
		pos = (pos + 1u) & ring_mask;
	}
}
static void codec_unpack(int16_t *ring, uint32_t ring_mask, uint32_t start,
                         const uint8_t *flash, uint32_t nblk)
{
	uint32_t ntot = nblk * SAMP_PER_BLK;
	uint32_t pos = start;
	for (uint32_t i = 0; i < ntot; i++) {
		ring[pos] = ulaw_decode(flash[i]);
		pos = (pos + 1u) & ring_mask;
	}
}

#else /* SP1_CODEC == SP1_CODEC_ADPCM */
/* ---- IMA ADPCM, 4-bit, ~4:1, SELF-CONTAINED 512-byte blocks --------------
 * Each 512-byte flash block decodes STANDALONE (predictor + step index RESET at
 * the block start) so random-access loop seeks land on any block.
 *   byte 0..1 : int16 predictor seed (little-endian) = block's first sample
 *   byte 2    : uint8 step index seed (0..88)
 *   byte 3    : pad (0)
 *   byte 4..511 : 508 data bytes * 2 nibbles = 1016 samples (SAMP_PER_BLK).
 * Within a block, sample[k] is encoded as nibble[k] against the running
 * predictor seeded from the header (so nibble[0] re-encodes sample[0] against
 * predictor==sample[0]; round-trips to ~sample[0]). 508 bytes = exactly 1016
 * nibbles = SAMP_PER_BLK. Low nibble of each byte first, then high nibble. */
static const int8_t  ima_index_tab[16] = {
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8
};
static const int16_t ima_step_tab[89] = {
	    7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
	   19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
	   50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
	  130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
	  337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
	  876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
	 2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
	 5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
#define ADPCM_HDR_BYTES   4u
#define ADPCM_DATA_BYTES  (EMMC_BLOCK_SIZE - ADPCM_HDR_BYTES)   /* 508 -> 1016 samples */

static inline uint8_t ima_enc_step(int16_t sample, int32_t *pred, int *idx)
{
	int step = ima_step_tab[*idx];
	int diff = sample - *pred;
	int code = 0;
	if (diff < 0) { code = 8; diff = -diff; }
	if (diff >= step)        { code |= 4; diff -= step; }
	if (diff >= (step >> 1)) { code |= 2; diff -= step >> 1; }
	if (diff >= (step >> 2)) { code |= 1; }
	/* reconstruct EXACTLY as the decoder will, to keep predictor in lockstep */
	int diffq = step >> 3;
	if (code & 4) diffq += step;
	if (code & 2) diffq += step >> 1;
	if (code & 1) diffq += step >> 2;
	if (code & 8) *pred -= diffq; else *pred += diffq;
	if (*pred >  32767) *pred =  32767;
	if (*pred < -32768) *pred = -32768;
	*idx += ima_index_tab[code & 7];
	if (*idx < 0)  *idx = 0;
	if (*idx > 88) *idx = 88;
	return (uint8_t)(code & 0x0F);
}
static inline int16_t ima_dec_step(uint8_t code, int32_t *pred, int *idx)
{
	int step = ima_step_tab[*idx];
	int diffq = step >> 3;
	if (code & 4) diffq += step;
	if (code & 2) diffq += step >> 1;
	if (code & 1) diffq += step >> 2;
	if (code & 8) *pred -= diffq; else *pred += diffq;
	if (*pred >  32767) *pred =  32767;
	if (*pred < -32768) *pred = -32768;
	*idx += ima_index_tab[code & 7];
	if (*idx < 0)  *idx = 0;
	if (*idx > 88) *idx = 88;
	return (int16_t)*pred;
}

/* Encode exactly ONE block (SAMP_PER_BLK==1016 samples) into one 512-byte block,
 * predictor + step index RESET at block start -> block is standalone. */
static void adpcm_pack_block(const int16_t *ring, uint32_t ring_mask,
                             uint32_t start, uint8_t *blk)
{
	uint32_t pos = start;
	int32_t pred = ring[pos];          /* seed predictor = first sample */
	int idx = 0;                        /* fixed reset step index */
	blk[0] = (uint8_t)(pred & 0xFF);
	blk[1] = (uint8_t)((pred >> 8) & 0xFF);
	blk[2] = (uint8_t)idx;
	blk[3] = 0;
	uint8_t *data = blk + ADPCM_HDR_BYTES;
	for (uint32_t i = 0; i < ADPCM_DATA_BYTES; i++) {
		int16_t s0 = ring[pos];  pos = (pos + 1u) & ring_mask;
		uint8_t n0 = ima_enc_step(s0, &pred, &idx);
		int16_t s1 = ring[pos];  pos = (pos + 1u) & ring_mask;
		uint8_t n1 = ima_enc_step(s1, &pred, &idx);
		data[i] = (uint8_t)(n0 | (n1 << 4));
	}
}
/* Decode exactly ONE block back into SAMP_PER_BLK==1016 ring samples. */
static void adpcm_unpack_block(int16_t *ring, uint32_t ring_mask,
                               uint32_t start, const uint8_t *blk)
{
	uint32_t pos = start;
	int32_t pred = (int16_t)((uint16_t)blk[0] | ((uint16_t)blk[1] << 8));
	int idx = blk[2];
	if (idx > 88) idx = 88;
	const uint8_t *data = blk + ADPCM_HDR_BYTES;
	for (uint32_t i = 0; i < ADPCM_DATA_BYTES; i++) {
		uint8_t b = data[i];
		ring[pos] = ima_dec_step(b & 0x0F, &pred, &idx);
		pos = (pos + 1u) & ring_mask;
		ring[pos] = ima_dec_step((b >> 4) & 0x0F, &pred, &idx);
		pos = (pos + 1u) & ring_mask;
	}
}
/* nblk blocks, each independent (fresh predictor) — REQUIRED for random-access
 * loop seeks: a play read can start at ANY block, so every block must decode
 * without history from the previous one. */
static void codec_pack(const int16_t *ring, uint32_t ring_mask, uint32_t start,
                       uint8_t *flash, uint32_t nblk)
{
	uint32_t pos = start;
	for (uint32_t b = 0; b < nblk; b++) {
		adpcm_pack_block(ring, ring_mask, pos, flash + b * EMMC_BLOCK_SIZE);
		pos = (pos + SAMP_PER_BLK) & ring_mask;
	}
}
static void codec_unpack(int16_t *ring, uint32_t ring_mask, uint32_t start,
                         const uint8_t *flash, uint32_t nblk)
{
	uint32_t pos = start;
	for (uint32_t b = 0; b < nblk; b++) {
		adpcm_unpack_block(ring, ring_mask, pos, flash + b * EMMC_BLOCK_SIZE);
		pos = (pos + SAMP_PER_BLK) & ring_mask;
	}
}
#endif /* SP1_CODEC */

#if SP1_XFER_ENABLE
/*
 * PRIME THE WHOLE READ-AHEAD WINDOW before playback is allowed to start.
 *
 * Boot used to read, validate and publish sector 0 and then immediately set
 * g_stem_song_selected -- so the very first sector boundary the playhead
 * crossed was, by construction, an underrun: nothing else was resident yet.
 * The stream then spent the start of every song climbing out of a hole
 * instead of playing from a full ring.
 *
 * This fills every remaining slot (sectors 1..SLOTS-1) through exactly the
 * same path the running prefetch uses -- real physical read, real STSC
 * header parse, real geometry validation against the song's own record,
 * publication through the same atomic mailbox -- so a sector that would be
 * rejected during playback is rejected here too, before a note is heard.
 *
 * Runs on streamer_thread, the only thread that ever touches flash, and only
 * at boot-time selection, before g_stem_song_selected is published. Stops at
 * the first failure and returns how many sectors are genuinely resident, so
 * the caller reports the real depth rather than the intended one; a song
 * shorter than the ring simply primes fewer.
 */
/*
 * ONE STEM, `n` CONSECUTIVE GROUPS, ONE READ.
 *
 * This is the whole economy of song-planar in one function. A stem's timeline
 * is contiguous on storage, so n groups of stem k are 4n consecutive blocks;
 * and the caller has already established via st_stem_mbox_producer_next_run()
 * that the n destination slots are consecutive with no wrap. So a batch is a
 * single emmc_read_blocks() -- which is what makes four per-stem rings cost
 * about what one shared sector ring cost, instead of four times as much.
 *
 * EVERY GROUP IS VALIDATED, not just the first. A group carries its own
 * header naming the stem and the span it holds, and under v1.2 every read is
 * a group read, so a misaddressed batch has to be caught here rather than
 * heard later. Validation is against the stem and group the caller ASKED for,
 * so a read that silently landed in another stem's region fails.
 */
static bool stem_read_groups(uint32_t song_start_block, uint32_t groups,
			      uint32_t stem, uint32_t first_group,
			      uint32_t slot, uint32_t n)
{
	const uint32_t blk = st_pl_group_block(song_start_block, groups,
						stem, first_group);
	uint32_t i;

	if (!emmc_read_blocks(blk, &g_stem_group_bufs[stem][slot][0],
			       n * ST_PL_GROUP_BLOCKS)) {
		return false;
	}
	for (i = 0; i < n; i++) {
		if (!st_pl_validate(g_stem_group_bufs[stem][slot + i],
				     stem, first_group + i)) {
			return false;
		}
	}
	return true;
}

/*
 * GROUP 0 OF ALL FOUR STEMS, synchronously, into slot 0 of each ring.
 *
 * The v1.1 equivalent was a single sector-0 read, and it served the same two
 * callers: boot selection and the post-commit reload. Both need the first span
 * resident and adopted before either thread's steady-state loop starts, which
 * is exactly what st_stem_mbox_init() asserts about the sector it is given.
 */
static bool stem_prime_group0(uint32_t song_start_block, uint32_t groups)
{
	uint32_t k;

	for (k = 0; k < ST_PL_STEMS; k++) {
		if (!stem_read_groups(song_start_block, groups, k, 0u, 0u, 1u)) {
			return false;
		}
	}
	return true;
}

/*
 * Fill the rest of every stem's ring before playback starts, so the first
 * sector boundary is not also the first underrun. Group 0 is already resident
 * and adopted in all four (stem_prime_group0 + st_stem_mbox_init).
 *
 * Returns the number of spans resident, which is the smallest across the four
 * stems -- a span is only playable when ALL FOUR of its groups are there.
 */
static uint32_t stem_prime_read_ahead(void)
{
	uint32_t worst = ST_STEM_MBOX_SLOTS;
	uint32_t k;

	for (k = 0; k < ST_PL_STEMS; k++) {
		uint32_t primed = 1u;   /* group 0: read, validated, published */
		uint32_t g;

		for (g = 1u; g < ST_STEM_MBOX_SLOTS; g++) {
			const uint32_t slot = st_stem_mbox_slot_of(g);

			if (g >= ST_STEM_GEOM.sector_count) {
				break;          /* song shorter than the ring */
			}
			if (!stem_read_groups(ST_STEM_GEOM.song_start_block,
					       ST_STEM_GEOM.sector_count,
					       k, g, slot, 1u)) {
				break;
			}
			st_stem_mbox_publish_ready(&g_stem_mbox[k], g, slot);
			primed++;
		}
		if (primed < worst) {
			worst = primed;
		}
	}
	return worst;
}
#endif /* SP1_XFER_ENABLE */

/* ROUNDS PER PASS. A round that finds nothing to do ends the loop, so this is
 * a livelock guard rather than a tuning knob: refilling one stem's ring from
 * completely empty takes SLOTS/R batches, and past that a round provably finds
 * every slot resident and returns nothing. */
#define ST_STEM_REFILL_ROUNDS ((ST_STEM_MBOX_SLOTS / ST_PL_REFILL_GROUPS) + 1u)

/* The rotating sweep indexes stems with a mask, which is only the same thing as
 * a modulo while the count is a power of two. */
_Static_assert((ST_PL_STEMS & (ST_PL_STEMS - 1u)) == 0u,
		"the refill rotation masks the stem index; ST_PL_STEMS must be a power of two");

/*
 * HANDING THE CPU BACK, IN TWO FORMS, FOR TWO DIFFERENT REASONS.
 *
 * ---- WHY THE PRIORITIES CHANGED (st44) ----------------------------------
 *
 * main() ran at PREEMPT(1) and the streamer at PREEMPT(5), so every ladder
 * read, LED pass and diagnostic print interrupted a sector read wherever it
 * happened to land. Hardware showed the cost: a read that takes 1829 us
 * unpreempted (p50) averaged 3750 us, and the streamer -- inside a read 98.6%
 * of the wall clock -- held the CPU for only 49% of it.
 *
 * The comment that justified PREEMPT(1) said "preempting the streamer is
 * harmless NOW: the rings ride 341 ms". That is the TAPE LOOPER's ring.
 * Stem Tape's is G-R = 4 groups = 28.3 ms, twelve times shallower, and the
 * justification did not survive the format change with it.
 *
 * EQUAL PRIORITY, NOT INVERSION. Both now run at PREEMPT(1) and the streamer
 * yields once per read. That gives the property asked for -- control work
 * cannot interrupt a sector read midway -- without the waste a demotion would
 * cost: to let a strictly LOWER-priority main run, the streamer would have to
 * k_sleep() for a fixed span whether main needed it or not, and main's own
 * 0.64 ms of work per 8 ms pass would have to be paid for in dead time on top
 * of itself. k_yield() costs nothing when nobody is waiting.
 *
 * Audio stays at PREEMPT(0) and still preempts both, instantly, anywhere.
 * MIDI stays at PREEMPT(6), below all audio-critical work.
 *
 * ---- THE TWO CALLS ------------------------------------------------------
 *
 * yield  once per read. Bounds how long main() can wait to run at one read
 *        (p50 1.8 ms, worst 25 ms) -- far inside its own 8 ms k_msleep()
 *        cadence, and four orders of magnitude inside the 4 s watchdog.
 *        Reschedules only to READY threads at PREEMPT(1) or above, so it is
 *        free when main() is in its sleep, which is 92% of the time.
 *
 * sleep  a 0.5 ms breather per 64 reads. k_yield() cannot reach anything
 *        BELOW PREEMPT(1) -- MIDI at (6) and the idle thread -- so a real
 *        sleep still has to happen periodically. Counted per read rather
 *        than per pass because a pass now issues up to ROUNDS x STEMS reads:
 *        a per-pass counter would stretch the worst gap by that factor, and
 *        at 3750 us a read that is one sleep per ~3.8 s against a 4 s
 *        watchdog. Well under 1%.
 */
static void streamer_breathe(void)
{
	static uint32_t units;

	/* Free when main() is sleeping; a scheduling point when it is not. */
	k_yield();
	if ((++units & 0x3Fu) == 0u) {
		k_usleep(500);
	}
}

static void streamer_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	static uint8_t blk[EMMC_BLOCK_SIZE];
	static uint8_t metabuf[META_BLOCKS * EMMC_BLOCK_SIZE];  /* 2-block song index */
	/* batchbuf (16 KB) is GONE along with PASS 2, its only user -- see the
	 * note where PASS 2 used to be, below. It staged classic play-ring
	 * refills for tracks that this firmware can never put in TS_PLAY, and
	 * its 16 KB is now the stored-song ring's third and fourth sector
	 * buffers instead. */

	(void)emmc_init();
	/* AFTER init: emmc_init() resets the clock to the slow safe value — the
	 * old code zeroed it BEFORE init, so every bit-bang phase (start-bit
	 * hunts, CRC tokens, busy polls) has been running ~4x slower than
	 * intended this whole time. Zero it here so it actually sticks. */
	g_emmc_clk_half_us = 0u;
	/* Announce the build BEFORE anything else this thread prints, right
	 * after the Zephyr banner, so the very first lines of any capture
	 * identify the firmware. See ST_BUILD_TAG. */
	printk("STEMTAPE BUILD %s\n", ST_BUILD_TAG);
	g_emmc_ready = emmc_is_ready() ? 1 : 0;

	/* Enable the card's internal write cache if it has one. Read EXT_CSD (CMD8) to
	 * check CACHE_SIZE and the spec revision; if present, turn the cache on. It
	 * absorbs the record write-bursts so an overdub acks from the card's SRAM
	 * instead of stalling the bus -- without it the 4th simultaneous track
	 * overflows the rec ring. There is deliberately NO flush during play (that
	 * freezes the bus and starves playback); the card flushes in the background,
	 * and we force a single flush at power-off (see stop_and_flush) so loops are
	 * durable. eMMC is streamer-only, so this boot-time read is safe here. */
	/* The write cache absorbs each record burst so the write returns immediately
	 * instead of programming NAND on the bus and starving the playing tracks
	 * (which is what crackles). Both builds use it; the 24 kHz build pairs it with
	 * the in-spec 16 MHz bus (the overclock, not the cache, was its white-noise). */
	if (g_emmc_ready && emmc_read_ext_csd(blk)) {
		uint32_t cache_kb = (uint32_t)blk[249] | ((uint32_t)blk[250] << 8) |
				    ((uint32_t)blk[251] << 16) | ((uint32_t)blk[252] << 24);
		g_cache_kb = cache_kb;
		/* diag snapshot: WR_REL_SET, WR_REL_PARAM, SEC_FEATURE_SUPPORT,
		 * BKOPS_SUPPORT, HPI_FEATURES, OUT_OF_INTERRUPT_TIME, BKOPS_STATUS,
		 * EXT_CSD_REV, ERASE_GROUP_DEF — confirms on the REAL unit which
		 * FTL-management features (TRIM/BKOPS/HPI) the card supports. */
		g_extcsd_dump[0] = blk[167]; g_extcsd_dump[1] = blk[166];
		g_extcsd_dump[2] = blk[231]; g_extcsd_dump[3] = blk[502];
		g_extcsd_dump[4] = blk[503]; g_extcsd_dump[5] = blk[198];
		g_extcsd_dump[6] = blk[246]; g_extcsd_dump[7] = blk[192];
		g_extcsd_dump[8] = blk[175];
		if (cache_kb > 0u && blk[192] >= 6u)   /* CACHE_SIZE>0, EXT_CSD_REV>=6 (v4.5+) */
			g_cache_on = emmc_cache_enable() ? 1u : 0u;
		if (blk[503] & 0x01) {                 /* HPI: abort lever for the idle flush */
			g_hpi_on = emmc_hpi_enable() ? 1u : 0u;
			if (g_hpi_on)
				emmc_set_abort_cb(emmc_busy_abort_chk);
		}
#if SP1_XFER_ENABLE
		/* Stem Tape v1.1: EXT_CSD SEC_COUNT (offset 212, 4 bytes LE) --
		 * the real, standard eMMC "device size in 512-byte blocks" field
		 * -- is the real, capacity-detected device size the v1.1 region
		 * layout is computed against (docs section 10 item 2: "aligned,
		 * non-overlapping and inside deviceBlocks"), FROZEN here for the
		 * life of the image (the wiring directive's own requirement: "do
		 * not relocate it later after real songs may exist"). A zero/
		 * implausible reading, or a device too small to hold two equal
		 * song regions plus two index regions, leaves g_v11_layout_ready
		 * at its safe default (0, fail closed) -- 'Q' then stays silent
		 * (docs section 2) rather than advertising a layout the device
		 * cannot actually honor. */
		{
			uint32_t sec_count = (uint32_t)blk[212] | ((uint32_t)blk[213] << 8) |
					      ((uint32_t)blk[214] << 16) | ((uint32_t)blk[215] << 24);

			/* st11_storage_layout_compute()'s `device_blocks_total` is
			 * blocks AVAILABLE STARTING AT base_block, not the raw
			 * device size from block 0 -- so the reserved base has to
			 * be subtracted before calling it, even though the STCP
			 * reply's own "total device blocks" field (below) is the
			 * raw, ABSOLUTE size every region start is compared
			 * against, and stays sec_count unchanged. */
			if (sec_count > ST11_STORAGE_BASE_BLOCK &&
			    st11_storage_layout_compute(ST11_STORAGE_BASE_BLOCK,
							 sec_count - ST11_STORAGE_BASE_BLOCK, &g_v11_layout)) {
				g_v11_device_blocks_total = sec_count;
				g_v11_layout_ready = 1u;
			}
		}
#endif
	}

	/* STEM TAPE PHASE 1: storage fails closed. Load the slot metadata (block
	 * 0) if present AND recognized; otherwise -- unknown, stock/factory,
	 * classic Tape Looper, corrupt, or absent -- NEVER format-fresh, NEVER
	 * write. Stay on the safe, empty, in-RAM-only default and report
	 * g_storage_unrecognized so the read-only state is visible (see
	 * controls_diag()) instead of silently pretending an empty song exists. */
	memset(&g_meta, 0, sizeof(g_meta));
	g_meta.magic = META_MAGIC;
	for (uint32_t s = 0; s < NUM_SLOTS; s++) g_meta.slot[s].speed_q16 = 65536u;
	if (g_emmc_ready && emmc_read_blocks(META_BLOCK, metabuf, META_BLOCKS)) {
		struct meta_blk *m = (struct meta_blk *)metabuf;
		if (m->magic == META_MAGIC && m->cur_slot < NUM_SLOTS) {
			memcpy(&g_meta, m, sizeof(g_meta));     /* resume saved songs (read-only) */
		} else {
			g_storage_unrecognized = 1;             /* read-only: no format-fresh write */
		}
	} else {
		g_storage_unrecognized = 1;                     /* absent/unreadable: read-only */
	}
	/* M8a: grid extension (block 2). Bad tag/sum -> all zeros = no grids. */
	if (g_emmc_ready && emmc_read_blocks(GRID_EXT_BLOCK, metabuf, 1)) {
		struct grid_ext *ge = (struct grid_ext *)metabuf;
		uint16_t gsum = 0;
		for (uint32_t gi = 0; gi < NUM_SLOTS; gi++)
			gsum = (uint16_t)(gsum + ge->bpm_q8[gi]);
		if (ge->magic == GRID_EXT_MAGIC && gsum == ge->sum)
			for (uint32_t gi = 0; gi < NUM_SLOTS; gi++)
				g_grid_bpm_q8[gi] = ge->bpm_q8[gi];
	}
	g_slot = g_meta.cur_slot;
	g_mode_pref = g_meta.fixed_len ? 1u : 0u;   /* M7c: global mode preference */
	g_fixed_len = g_mode_pref;                  /* effective refined when the
	                                             * current song loads (main) */
	g_meta_loaded = 1;

#if SP1_XFER_ENABLE
	/* Stem Tape v1.1 boot-time A/B selection (docs section 10 item 4:
	 * "Boot from the greater valid generation, falling back to the other
	 * record"). Runs the SAME selector 'Q' and every session-open use
	 * (st_stix_read_library(), never the device's own advisory hint) once
	 * at cold boot and logs the result. As of Phase 2 slice 3B, this IS
	 * consumed downstream: if a song is present, its geometry seeds the
	 * pure streaming state machine and its own sector 0 is read here
	 * (see the block below) into g_stem_group_bufs[..][0], which looper_audio_
	 * block()'s own PASS C plays back; streamer_thread's own per-pass
	 * prefetch step (below the main while(1) loop's top) keeps the
	 * OTHER buffer filled ahead of the playhead for the rest of the
	 * song. Never writes; a blank/corrupt library logs as such and is
	 * left exactly as read -- 'Q' and the next real upload's session-
	 * open are what actually act on it. */
	if (g_v11_layout_ready && g_emmc_ready) {
		uint8_t idx_a[ST11_PHYSICAL_BLOCK_BYTES];
		uint8_t idx_b[ST11_PHYSICAL_BLOCK_BYTES];

		if (emmc_read_blocks(g_v11_layout.index_a_start, idx_a, 1) &&
		    emmc_read_blocks(g_v11_layout.index_b_start, idx_b, 1)) {
			st_stix_library_state_t lib;

			st_stix_read_library(idx_a, idx_b, g_v11_layout.song_a_start, g_v11_layout.song_a_blocks,
					      g_v11_layout.song_b_start, g_v11_layout.song_b_blocks, &lib);
			if (lib.status == ST_STIX_LIB_OK) {
				/* Split the 64-bit generation into two 32-bit halves for
				 * printk -- Zephyr's default minimal printk build does
				 * not reliably support %llu, and this is a boot-time
				 * diagnostic only (see this block's own comment), not a
				 * value anything downstream parses. */
				printk("V11 lib: gen_hi=%u gen_lo=%u active_index=%u active_song=%u\n",
				       (unsigned)(uint32_t)(lib.generation >> 32),
				       (unsigned)(uint32_t)lib.generation, (unsigned)lib.active_index_slot,
				       (unsigned)lib.active_song_slot);

				/* STEM TAPE Phase 2 slice 3B/3B.1: continuous
				 * streaming. Reuses THIS SAME real st_stix_read_
				 * library() result -- never a second parser -- to
				 * seed the pure state machine with the song's own
				 * real geometry, then read its first STSC sector:
				 * one bounded, one-time flash read, here in
				 * streamer_thread (the only thread that ever touches
				 * flash; audio_thread only ever reads the resulting
				 * g_stem_group_bufs[] from RAM, see looper_audio_block()'s
				 * own PASS C). Real geometry + header validation
				 * (st_stream_init()/st_stream_validate_sector()) --
				 * both read-only-geometry-safe to call here, see
				 * st_stream_t's own doc comment -- before publishing
				 * g_stem_song_selected: invalid geometry or a
				 * corrupt/garbage sector 0 is left unselected (silent
				 * stem playback) rather than trusted. This is the
				 * ONLY place g_stem_stream's construction happens;
				 * from here on it belongs to audio_thread alone.
				 * st_stem_mbox_init() publishes sector 0 as already
				 * resident in buffer 0 -- audio_thread's own first
				 * real tick then acquires it through the SAME mailbox
				 * path every later sector uses, no special-casing
				 * needed. The REST of the song is streamed in by
				 * this same thread's own per-pass prefetch step,
				 * below the main while(1) loop's top -- see its own
				 * comment. */
				if (lib.active.flags & ST11_IX_FLAG_SONG_PRESENT) {
					if (stem_streams_init(lib.active.song_start_block,
							       lib.active.song_block_count, lib.active.frames,
							       lib.active.sector_count, /*loop_enabled=*/true)) {
						if (stem_prime_group0(lib.active.song_start_block,
								       lib.active.sector_count)) {
							{
								/* STEM_PRIME_GROUP0() IS THE VALIDATION.
								 * v1.1 checked sector 0's 32-byte STSC
								 * header against the stream geometry;
								 * a v1.2 group header is eight bytes and
								 * carries identity instead -- magic, the
								 * stem it claims to be, the span it
								 * covers. stem_read_groups() checks each
								 * of the four against what was ASKED
								 * for, so a region miscomputed from the
								 * STIX geometry fails above rather than
								 * playing as another stem's audio. The
								 * geometry itself was already checked by
								 * st_stream_init().
								 *
								 * NO SECTOR-0 TIMING CROSS-CHECK ANY
								 * MORE, and nothing is lost with it.
								 * v1.1's STSC header repeated bpm_q8
								 * and downbeat_frame, and this block
								 * compared them to the STIX record
								 * purely to log a disagreement -- STIX
								 * always won and the sector copy was
								 * never acted on. A v1.2 group header
								 * is eight bytes of IDENTITY (magic,
								 * stem, span) and carries no timing to
								 * disagree with, so the one authority
								 * is now also the only one. See
								 * st_beat_phase.h for why this is the
								 * sole place tempo is ever read, and
								 * g_stem_beat_timing's own comment for
								 * why writing it here, strictly before
								 * the release fence below, needs no
								 * separate synchronization. */
								/* TEMPO REPORT, always -- success as well as
								 * failure. The global loop quantises every
								 * window to this beat and REFUSES to start
								 * without it, so "no loop happened" and "no
								 * tempo" are the same fact and must not be
								 * silent. Reporting only the failure left
								 * the working case unprovable, which is
								 * what made st15's dead loop hard to
								 * diagnose from the device. */
								if (!st_beat_timing_init(&g_stem_beat_timing, lib.active.bpm_q8,
											  lib.active.downbeat_frame,
											  lib.active.sample_rate)) {
									printk("V11 lib: TEMPO INVALID "
									       "(bpm_q8=%u sample_rate=%u "
									       "downbeat=%u) -- frames_per_beat=0. "
									       "LED beat pulse disabled AND the "
									       "global loop will refuse to start. "
									       "Fix the STIX record's timing.\n",
									       (unsigned)lib.active.bpm_q8,
									       (unsigned)lib.active.sample_rate,
									       (unsigned)lib.active.downbeat_frame);
								} else {
									printk("V11 lib: TEMPO OK bpm_q8=%u "
									       "(%u.%02u BPM) sample_rate=%u "
									       "downbeat=%u frames_per_beat=%u\n",
									       (unsigned)lib.active.bpm_q8,
									       (unsigned)(lib.active.bpm_q8 >> 8),
									       (unsigned)(((lib.active.bpm_q8 & 0xFFu) * 100u) >> 8),
									       (unsigned)lib.active.sample_rate,
									       (unsigned)lib.active.downbeat_frame,
									       (unsigned)g_stem_beat_timing.frames_per_beat);
								}
								{
									uint32_t mk;

									for (mk = 0; mk < ST_PL_STEMS; mk++) {
										st_stem_mbox_init(&g_stem_mbox[mk], 0u);
									}
								}
								{
									/* Fill the REST of the ring before
									 * publishing the song, so playback
									 * starts from a full read-ahead
									 * window rather than underrunning on
									 * its first sector boundary. */
									uint32_t primed = stem_prime_read_ahead();

									printk("V11 lib: read-ahead primed %u/%u sectors "
									       "(%u ms) before enabling playback\n",
									       (unsigned)primed,
									       (unsigned)ST_STEM_MBOX_SLOTS,
									       (unsigned)((primed * ST11_FRAMES_PER_SECTOR * 1000u)
											  / ST11_SAMPLE_RATE_HZ));
								}
								/* STEM TAPE has no persistent mute: a Track
								 * press is momentary solo only. Clear any
								 * trk[].muted the classic reload path
								 * restored from g_meta.song_mode[] --
								 * inherited state from firmware that DID
								 * latch mutes, which would otherwise leave
								 * a stem silently muted with no gesture
								 * left in this firmware to un-mute it. */
								for (int mi = 0; mi < NTRK; mi++) {
									trk[mi].muted = 0u;
									trk[mi].solo = 0u;
								}
								atomic_set(&g_stem_song_selected, 1); /* release fence */
							}
						}
					} else {
						printk("V11 lib: song geometry failed streaming validation, "
						       "stem playback disabled\n");
					}
				}
			} else {
				printk("V11 lib: %s, requires initialization\n",
				       lib.status == ST_STIX_LIB_BLANK ? "blank" : "corrupt");
			}
		} else {
			printk("V11 lib: index read failed at boot\n");
		}
	}
#endif

	while (1) {
#if SP1_XFER_ENABLE
		/* Website loop transfer: scan for the connect-magic / run one command
		 * per pass. While a transfer is active the transport is paused and
		 * the streamer serves ONLY the transfer (audio is silent anyway).
		 * v1.2.3: gated on USB being up — the streamer can now run during
		 * charge-standby, before usb_audio_start(). */
		if (g_usb_up)
			xfer_service();
#endif
		if (g_xfer_mode) {
			/* Same acknowledgement, from the producer side: past this
			 * point the streamer fills no buffer this pass. */
			atomic_set(&g_xfer_stream_quiesced, 1);
			k_msleep(1);
			continue;
		}

		/* Power-off cache flush: program the volatile write cache to NAND so the
		 * last take + slot index survive a power cut. Requested by stop_and_flush
		 * AFTER recording is finalized + while shutting down, so this bus-blocking
		 * flush has nothing live to starve. Done here because the streamer is the
		 * only eMMC user. */
		if (g_emmc_quiesce) {                   /* shutting down: bus parked */
			k_msleep(10);
			continue;
		}
		if (g_cache_flush_req) {
			(void)emmc_cache_flush();
			g_emmc_quiesce = 1;   /* no further eMMC traffic after the final flush */
			g_cache_flush_req = 0;
			continue;
		}

		bool work = false;
		/* `cpos` (the classic playhead snapshot) and `slot` (the classic
		 * song slot this sweep was serving) both went with PASS 2 -- they
		 * existed only to decide which classic loop blocks to refill. */

		/* STEM TAPE PHASE 1: g_meta_save_req / g_grid_save_req are still SET
		 * by various control-loop actions (mute/speed/grid changes) exactly
		 * as in the classic looper -- that request bookkeeping is shared,
		 * harmless state, left untouched. What's removed is the WRITE this
		 * phase must never perform: both requests are simply cleared here,
		 * in RAM only, with no meta_write_blocks()/emmc_write_blocks() call.
		 * No mute/speed/grid/settings persistence in this phase. */
		if (g_meta_save_req) {
			g_meta_save_req = 0;
		}
		if (g_grid_save_req) {
			g_grid_save_req = 0;
		}

#if SP1_XFER_ENABLE
		/* PREFETCH. st_stem_mbox_producer_next_fill() decides what to
		 * fetch: it scans forward from the sector the consumer says it
		 * needs, over the whole read-ahead window, and names the
		 * nearest sector whose slot does not already hold it -- so a
		 * fill is always spent on the gap the consumer will reach
		 * soonest, and the slot the consumer is currently reading from
		 * is structurally excluded. There is no producer-side memory of
		 * "what I last published" any more: the ring's own slot
		 * contents ARE that record, and reading them back is what makes
		 * a seek or a loop wrap self-correcting without anything having
		 * to detect it.
		 *
		 * One bounded read per pass, at most, so this never dominates a
		 * single streamer_thread iteration.
		 *
		 * A read that fails outright (short/failed physical read) simply
		 * does not publish -- so the slot stays stale and the very next
		 * pass picks the same sector again, matching the classic
		 * engine's own g_p2rfail "read failed: retry in a few ms"
		 * convention below, since a single transient bus hiccup is not
		 * proof the sector's DATA is bad. A read that succeeds but
		 * fails validation (corrupt header, wrong sector_index/
		 * first_frame/frame_count, or out-of-range) is counted on the
		 * corrupt diagnostic counter and likewise does not publish.
		 * Neither failure ever reaches the mailbox, so the consumer
		 * stays in UNDERRUN (silence) until a read genuinely succeeds
		 * and validates. */
		/* ---- THE LOOP EXIT KIT ------------------------------------
		 * Fill both pinned regions -- the window's start (the entry seek
		 * and every wrap) and its end (every exit) -- once per gesture,
		 * BEFORE the ordinary prefetch below: the ring can recover from
		 * a late fill, an entry or an exit cannot.
		 *
		 * The control thread publishes the wanted base sectors in
		 * g_stem_loop_pin_want[] the instant the gesture ARMS -- on the
		 * PLAY-DOWN edge, a full ST_LOOP_HOLD_MS before the loop can
		 * start. Eight sector reads take ~41 ms against a 450 ms hold,
		 * so BOTH regions are resident before the entry seek needs the
		 * first and long before any exit can need the second.
		 *
		 * Publication order is bytes-then-count-then-base, and the
		 * consumer reads base-then-count, so a partially filled pin can
		 * never be observed as valid. Nothing rewrites a pin while it
		 * is valid, so there is no refill-while-reading hazard here at
		 * all -- see the pin's own declaration comment. */
		if (atomic_get(&g_stem_song_selected)) {
			uint32_t region;

			for (region = 0u; region < ST_LOOP_PIN_REGIONS; region++) {
			int32_t want_base =
				(int32_t)atomic_get(&g_stem_loop_pin_want[region]);

			if (want_base >= 0 &&
			    (int32_t)atomic_get(&g_stem_loop_pin_base[region]) != want_base) {
				/*
				 * ONE READ PER STEM, NOT ONE PER SPAN -- which is
				 * why the pin pool is stem-major.
				 *
				 * A region is `depth` consecutive spans, so for a
				 * single stem those are `depth` consecutive groups
				 * on storage AND `depth` consecutive buffers in
				 * RAM. The whole region for one stem is therefore
				 * one emmc_read_blocks() of depth*4 blocks, and a
				 * loop arms in 8 reads where v1.1 took 10. Laying
				 * the pool out span-major would have made it 4
				 * reads per span, 40 in all.
				 */
				uint32_t filled = st_loop_pin_depth[region];
				uint32_t k;

				atomic_set(&g_stem_loop_pin_count[region], 0);   /* invalidate first */
				atomic_set(&g_stem_loop_pin_base[region], -1);

				/* Clamp to the song before reading, not during:
				 * a short region is fine, a read past the song is
				 * not. */
				if ((uint32_t)want_base >= ST_STEM_GEOM.sector_count) {
					filled = 0u;
				} else if (filled > ST_STEM_GEOM.sector_count - (uint32_t)want_base) {
					filled = ST_STEM_GEOM.sector_count - (uint32_t)want_base;
				}

				for (k = 0; k < ST_PL_STEMS && filled > 0u; k++) {
					const uint32_t blk = st_pl_group_block(
						ST_STEM_GEOM.song_start_block,
						ST_STEM_GEOM.sector_count,
						k, (uint32_t)want_base);
					uint32_t i;

					if (!emmc_read_blocks(blk,
							       &g_stem_loop_pin_bufs[k][st_loop_pin_off[region]][0],
							       filled * ST_PL_GROUP_BLOCKS)) {
						filled = 0u;
						break;
					}
					for (i = 0; i < filled; i++) {
						if (!st_pl_validate(
							    g_stem_loop_pin_bufs[k][st_loop_pin_off[region] + i],
							    k, (uint32_t)want_base + i)) {
							filled = 0u;   /* never pin unvalidated bytes */
							break;
						}
					}
				}
				if (filled > 0u) {
					atomic_set(&g_stem_loop_pin_count[region],
						   (atomic_val_t)filled);
					atomic_set(&g_stem_loop_pin_base[region],
						   (atomic_val_t)want_base);
				}
			} else if (want_base < 0 &&
				   (int32_t)atomic_get(&g_stem_loop_pin_base[region]) >= 0) {
				/* Loop over: drop the pin so the ring alone feeds
				 * playback again and no stale sector can ever be
				 * preferred over a fresh one. */
				atomic_set(&g_stem_loop_pin_count[region], 0);
				atomic_set(&g_stem_loop_pin_base[region], -1);
			}
			}
		}

		if (atomic_get(&g_stem_song_selected)) {
			/*
			 * THE SUSTAINED PATH -- one batched read per stem per
			 * pass, and the thing every sizing decision was about.
			 *
			 * v1.1 fetched one 16-block sector holding all four
			 * stems. v1.2 fetches ST_PL_REFILL_GROUPS consecutive
			 * groups of ONE stem, four times. That is more reads per
			 * pass and FEWER reads per span: a batch of R groups
			 * carries R spans of that stem, so the four stems
			 * between them cost 4/R reads per span instead of 1 --
			 * 2 reads at R=2, moving 8 blocks each.
			 *
			 * THAT COST ESTIMATE WAS WRONG, and hardware said so.
			 * It predicted 3834 us per read and "92% busy against
			 * v1.1's 83%, an operating point this device has already
			 * run with zero silence frames". The measured average is
			 * 3790 us per read -- close -- but the arithmetic built
			 * on it was not: 4096 bytes per read against the
			 * 1,152,000 B/s four stems demand needs an average under
			 * 3556 us, so R=2 is 6.6% SHORT with the streamer already
			 * at 100% duty, not 92% busy with margin. The run that
			 * proved it returned rate=1080852Bps, sil=344507 frames
			 * across und=3762 episodes.
			 *
			 * The deficit is not in this scheduling. 68% of every
			 * read is the bit-banged start-bit hunt, issuing about 47
			 * clock pulses per block for ~288 us -- roughly 6 us per
			 * GPIO toggle, which no register write plus register read
			 * can cost. st42 splits that window (STEMRD spin= against
			 * clk=) to say whether it is the loop or this thread not
			 * being scheduled. Until that lands, no depth, layout or
			 * batch size should be chosen from the numbers above.
			 *
			 * What it buys is that a stem's address is now
			 * independent of the others', which is the whole
			 * precondition for reversing one of them.
			 *
			 * ---- TWO FIXES LIFTED FROM THE TAPE LOOPER'S OWN
			 * ---- STREAMER, WHICH HIT THIS EXACTLY (src/main.c)
			 *
			 * That firmware serves four per-track rings from the same
			 * card and its comments record both pathologies by name,
			 * with the measurements that found them:
			 *
			 *   "the track that sorted LAST got locked out entirely
			 *    whenever the round kept terminating early, and one
			 *    track would sit at ZERO delivered blocks for whole
			 *    takes while its siblings stayed fat"   (main.c:2949)
			 *
			 *   "one-chunk-per-pass measured out at only ~18 passes/s,
			 *    pinning refill throughput to exactly consumption with
			 *    zero surplus to rebuild margins"       (main.c:2963)
			 *
			 * Both were reintroduced here. A fixed `for (stem = 0..3)`
			 * refills stem 3 a whole read AFTER stem 0 on every pass --
			 * at the measured 3790 us average read that is 11.4 ms of
			 * standing phase lag against a G-R = 4 group (28.3 ms)
			 * runway, and the hardware shows it: STEMPS came back
			 * miss=31,73,285,2077, monotone in the stem index. A shared
			 * stall cannot produce that shape, because the consumer's
			 * acquire loop BREAKS at the first missing stem and would
			 * therefore pile every count on stem 0.
			 *
			 * So, exactly as the Tape Looper does it:
			 *
			 *   ROTATION -- the sweep starts at a different stem each
			 *   round, so no stem is permanently last in line and
			 *   recovery after a stall is not serialised in the same
			 *   order every time.
			 *
			 *   ROUNDS -- the sweep repeats while it is still finding
			 *   work, instead of falling through to the rest of the
			 *   pass after one batch per stem. This is what lets a pass
			 *   rebuild surplus after a stall rather than delivering
			 *   exactly one batch and starting over.
			 *
			 * NEITHER ADDS THROUGHPUT, and neither is the fix for the
			 * measured 6.6% read-rate deficit -- that is the start-bit
			 * hunt, which st42 instruments. These remove a structural
			 * unfairness that would otherwise become severe the moment
			 * one stem's head genuinely diverges under reverse, and
			 * they cost one uint32_t.
			 */
			uint32_t round;
			bool round_work;
			/* Rotating first-served stem. The ONLY state either fix
			 * adds. */
			static uint32_t s_refill_rr;

			round = 0u;
			do {
			uint32_t stem_k;

			round_work = false;
			s_refill_rr = (s_refill_rr + 1u) & (ST_PL_STEMS - 1u);

			for (stem_k = 0; stem_k < ST_PL_STEMS; stem_k++) {
				const uint32_t stem =
					(s_refill_rr + stem_k) & (ST_PL_STEMS - 1u);
				uint32_t first, target_slot, n, i;
				uint32_t read_t0, read_us;
				bool read_ok;

				if (!st_stem_mbox_producer_next_run(
					    &g_stem_mbox[stem],
					    ST_STEM_GEOM.sector_count,
					    ST_PL_REFILL_GROUPS,
					    &first, &target_slot, &n)) {
					continue;
				}

				read_t0 = DWT->CYCCNT;
				read_ok = stem_read_groups(ST_STEM_GEOM.song_start_block,
							    ST_STEM_GEOM.sector_count,
							    stem, first, target_slot, n);
				read_us = (DWT->CYCCNT - read_t0) / 64u; /* 64 MHz -> us, same convention
									  * as g_audio_us_max below */

				/* atomic_add()/atomic_set(): single-writer counters
				 * (this thread is the only one that ever writes any
				 * of these), so plain read-modify-write would
				 * already be race-free for the WRITE side alone --
				 * these are atomic so any OTHER thread's concurrent
				 * READ (e.g. a future diagnostic print from main())
				 * never observes a torn 32-bit value either. */
				(void)atomic_add(&g_stem_diag_read_calls, 1);
				atomic_set(&g_stem_diag_read_us_last, (atomic_val_t)read_us);
				if (read_us > (uint32_t)atomic_get(&g_stem_diag_read_us_max)) {
					atomic_set(&g_stem_diag_read_us_max, (atomic_val_t)read_us);
				}
				if (read_us > (uint32_t)atomic_get(&g_stem_diag_read_us_win)) {
					atomic_set(&g_stem_diag_read_us_win, (atomic_val_t)read_us);
				}
				/* Fold this read's phase breakdown into the window
				 * sums BEFORE anything else can issue a read and
				 * overwrite sp1_emmc.c's per-call globals. Every
				 * outcome counts, failures included: a read that
				 * timed out spent its time somewhere too, and
				 * excluding it would flatter the average. */
				(void)atomic_add(&g_stem_diag_ph_reads, 1);
				(void)atomic_add(&g_stem_diag_ph_us, (atomic_val_t)read_us);
				(void)atomic_add(&g_stem_diag_ph_hunt_us,
						  (atomic_val_t)emmc_dbg_rd_hunt_us);
				(void)atomic_add(&g_stem_diag_ph_spin_us,
						  (atomic_val_t)emmc_dbg_rd_hunt_spin_us);
				(void)atomic_add(&g_stem_diag_ph_dma_us,
						  (atomic_val_t)emmc_dbg_rd_dma_us);
				(void)atomic_add(&g_stem_diag_ph_crc_us,
						  (atomic_val_t)emmc_dbg_rd_crc_us);
				(void)atomic_add(&g_stem_diag_ph_clks,
						  (atomic_val_t)emmc_dbg_rd_hunt_clks);
				/* A MAX, not a sum: one preempted burst anywhere
				 * in the window is the whole finding, and summing
				 * would bury it under hundreds of healthy reads. */
				if ((uint32_t)emmc_dbg_rd_pulse_max_ns >
				    (uint32_t)atomic_get(&g_stem_diag_ph_pulse_ns)) {
					atomic_set(&g_stem_diag_ph_pulse_ns,
						   (atomic_val_t)emmc_dbg_rd_pulse_max_ns);
				}

				if (read_ok) {
					/* PUBLISHED ONE GROUP AT A TIME, after
					 * every byte of the batch is written and
					 * validated -- the release store is
					 * per-slot, so a consumer observing group
					 * g has observed g's bytes and nothing is
					 * claimed on behalf of a group whose read
					 * failed. */
					(void)atomic_add(&g_stem_diag_bytes_total,
							  (atomic_val_t)(n * ST_PL_GROUP_BYTES));
					(void)atomic_add(&g_stem_diag_read_us_total, (atomic_val_t)read_us);
					for (i = 0; i < n; i++) {
						st_stem_mbox_publish_ready(
							&g_stem_mbox[stem], first + i,
							st_stem_mbox_slot_of(first + i));
					}
					work = true;
					round_work = true;
				} else {
					/* A failed or misaddressed batch publishes
					 * NOTHING, so the slots stay stale and
					 * next_run() names this same group again
					 * next pass. Counted as corrupt because
					 * stem_read_groups() validates every group
					 * header it read, so "the read returned
					 * bytes that are not this stem's group g"
					 * lands here rather than being played. */
					(void)atomic_add(&g_stem_corrupt_count, 1);
					(void)atomic_add(&g_stem_badhdr[stem], 1);
					/* A failed read means the card is busy or
					 * the batch was misaddressed. Neither is
					 * improved by hammering it again inside
					 * this pass: end the round and let the
					 * outer loop come back in a few ms, which
					 * is what the pre-rounds code did by
					 * construction. */
					round_work = false;
					break;
				}
				/* The watchdog guarantee, per READ rather than per
				 * pass -- see streamer_breathe(). */
				streamer_breathe();
			}
			/* Stop the moment a whole round finds nothing to do (the
			 * steady-state case, and the common one). The cap is a
			 * livelock guard only: refilling one stem's ring from
			 * completely empty takes SLOTS/R batches, so beyond that
			 * there is provably nothing left for a round to find. */
			} while (round_work && ++round < ST_STEM_REFILL_ROUNDS);
		}
#endif

		/* STEM TAPE PHASE 1: the classic looper's PASS 1 (rec-ring-to-flash
		 * write burst, take finalization/promotion, recording-driven
		 * g_meta.slot[].present[]/g_meta_save_req writes, and loop-seam
		 * de-click flash writes) is removed for this phase -- it is entirely
		 * gated on trk[].state == TS_REC/TS_DONE, states that are now
		 * unreachable (hold-to-record arming is removed, see main()'s control
		 * loop). Loading an EXISTING, already-persisted song for playback is a
		 * completely separate, read-only path (the g_slot_switch_req handler,
		 * which sets trk[i].state = TS_PLAY straight from g_meta.slot[].
		 * present[] on song load) and is untouched. Phase 2 reintroduces
		 * recording behind a real, validated write path. */

		/* PASS 2 (the classic looper's play-ring read-ahead) is REMOVED.
		 *
		 * It refilled trk[].pring from the classic per-track flash regions
		 * for every track in TS_PLAY, staging each chunk through a 16 KB
		 * batchbuf. In this firmware no track can ever be in TS_PLAY:
		 * g_meta.slot[].present[] is never assigned a nonzero value anywhere
		 * in this file, which the classic-source-absence CI gate proves
		 * fail-closed. So the whole pass was a loop whose body could not
		 * execute, holding 16 KB of RAM to stage reads it would never make.
		 *
		 * That 16 KB is exactly one more stored-song sector buffer, and it
		 * is spent on one: ST_STEM_MBOX_SLOTS goes from 2 to 4, taking the
		 * stream's read-ahead from a single sector to three. Buffering is
		 * what turns a card that is on average fast enough into a card that
		 * is fast enough EVERY time -- an eMMC read can stall on internal
		 * housekeeping (this driver's own start-bit hunt allows up to 80 ms
		 * for it), and with one sector of slack any stall longer than
		 * 7.08 ms is an audible hole no matter how quick the average read is.
		 *
		 * Stored-song prefetch (above) is the only read-ahead this firmware
		 * has, and it is unaffected -- it never used batchbuf or pring, and
		 * reads straight into the ring's sector buffers. */
		if (!work) {
			/* IDLE WINDOW: drain the card's write cache in the background.
			 * emmc_cache_flush_try() was built for exactly this (abortable:
			 * the busy-abort hook fires an HPI the moment a take arms or a
			 * play ring drains toward half) but was NEVER WIRED IN — the
			 * cache only flushed at power-off, so it silently filled across
			 * a session and later takes paid internal-eviction busy on
			 * every write burst. That is the "gets worse and worse",
			 * worst-on-the-4th-track cut-out: the first takes write into
			 * an empty cache, the last ones fight the card's housekeeping
			 * for the bus. Keeping the cache drained between takes gives
			 * every take a fresh, absorbent cache. */
			bool quiet = (g_rec_track < 0) && !g_xfer_mode &&
				     g_hpi_on && g_emmc_ready && !g_emmc_quiesce &&
				     !g_meta_save_req && !g_cache_flush_req;
#if SP1_XFER_ENABLE
			/* NEVER while a stored song is streaming. A cache flush
			 * programs NAND and freezes the bus for as long as it
			 * takes -- this file's own eMMC-cache comment already
			 * states the rule ("There is deliberately NO flush
			 * during play (that freezes the bus and starves
			 * playback)"), but the condition it was written for only
			 * covered the classic looper's recording states, which
			 * predate stem playback entirely. A stem song streams
			 * continuously with under one sector of slack, so a
			 * mid-song flush is a guaranteed audible stall. */
			if (atomic_get(&g_stem_song_selected)) {
				quiet = false;
			}
#endif
			if (quiet)
				for (int j = 0; j < NTRK; j++) {
					uint8_t sj = trk[j].state;
					if (sj == TS_ARMED || sj == TS_REC || sj == TS_DONE)
						quiet = false;
				}
			static int64_t flush_last;
			int64_t nowms = k_uptime_get();
			if (quiet && nowms - flush_last >= 50) {
				flush_last = nowms;
				(void)emmc_cache_flush_try();
			}
			k_msleep(2);
		} else {
			/* ANTI-STARVATION: the streamer at PREEMPT(5) outranks main(8),
			 * the WDT feeder. A long stretch of back-to-back work (or any
			 * future livelock in this loop) must NEVER be able to hold main
			 * off the CPU for the 4 s watchdog window. Same guarantee as
			 * before, now counted per unit of work rather than per pass --
			 * see streamer_breathe(), which the batched refill rounds also
			 * call, so the two share one budget. */
			streamer_breathe();
		}
	}
}

/* ========================================================================
 *  MIDI  —  timer-driven 24-PPQN clock + Start/Stop out over the SYNC jack.
 *  A free hardware timer clocks the UART bits one per ISR with interrupts
 *  left ON, so it never masks the eMMC/I2S ISRs (the fix for the >3-track
 *  crackle the old irq-locked bit-bang caused).
 * ======================================================================== */
/* ---- MIDI clock + Pocket-Operator sync out over the SYNC jack --------------
 * Pins from TimK's sync-jack schematic:
 *   MIDI  : BC807_BASE = P0.23 -> a PNP transistor that drives SYNC_RING. The
 *           PNP INVERTS: P0.23 LOW -> ring HIGH (MIDI idle/mark), P0.23 HIGH ->
 *           ring LOW (start bit/space). So we bit-bang the MIDI waveform, and
 *           midi_line() flips it for the transistor (set MIDI_INVERT 0 to undo
 *           if a receiver sees it inverted).
 *   PO sync: PO_A = P0.20 -> SYNC_TIP. A short pulse per 1/8 note (2 PPQN),
 *           the Korg/Volca/Pocket-Operator convention.
 * MIDI is 31250 baud, 8N1 = 32 us/bit. Each byte is sent with interrupts locked
 * so its 10 bits keep accurate spacing (~320 us, well within one I2S block of
 * DMA cushion). Driven from the low-priority midi_thread off the engine's
 * 24-PPQN clock counter — no UART peripheral needed.
 *
 * NOTE: untested on real gear yet — verify on a MIDI/PO device; if MIDI is
 * silent/garbled, try flipping MIDI_INVERT. */
#define MIDI_PIN      23u    /* P0.23 BC807_BASE -> SYNC_RING (MIDI)          */
#define POSYNC_PIN    20u    /* P0.20 PO_A       -> SYNC_TIP  (PO/Volca sync) */
#define POSYNC_PIN_B  17u    /* P0.17 PO_B       -> SYNC_TIP (paralleled)     */
#define MIDI_INVERT   1      /* PNP stage inverts; 1 = compensate             */
#define MIDI_BIT_US   32u    /* 31250 baud                                    */
#define PO_PULSE_MS   5      /* sync pulse width                              */
#define PO_DIV        12u    /* 24-PPQN clock / 12 = 2 PPQN (1/8-note pulses) */
/* MIDI/PO SYNC OUT — ENABLED, streaming-safe. The OLD bit-bang held irq_lock()
 * ~320us per byte (10 bits x 32us), masking the eMMC SPIM + I2S DMA ISRs ~32x/s
 * while playing -> stole the streamer's worst-case margin = the >3-track crackle
 * (v1/v2 had no MIDI thread). NOW the 10 UART bits are clocked out by a hardware
 * TIMER, one bit per tiny (~0.5us) ISR, with interrupts LEFT ON the whole time,
 * so the streamer is never starved. The PNP inverts the line, which a hardware
 * UARTE cannot compensate for -- the timer's ISR drives the bit via midi_line()
 * which applies MIDI_INVERT, so the timing is hardware-accurate AND the polarity
 * is right. Set to 0 to compile MIDI out entirely. */
/* MIDI is ON here (timer-driven) alongside the segment looper. Set to 0 to
 * compile the MIDI clock/Start-Stop output out entirely (the line stays idle). */
#define MIDI_SYNC_ENABLE 1

static K_THREAD_STACK_DEFINE(midi_stack, 768);
static struct k_thread   midi_tcb;

static void midi_pins_init(void)
{
	NRF_P0->PIN_CNF[MIDI_PIN]   =
		(GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
		(GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos);
	NRF_P0->PIN_CNF[POSYNC_PIN] =
		(GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
		(GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos);
	NRF_P0->PIN_CNF[POSYNC_PIN_B] =
		(GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
		(GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos);
	NRF_P0->OUTCLR = (1u << POSYNC_PIN) | (1u << POSYNC_PIN_B);
	/* idle the MIDI line at MARK (ring high -> P0.23 low after inversion) */
	if (MIDI_INVERT) NRF_P0->OUTCLR = (1u << MIDI_PIN);
	else             NRF_P0->OUTSET = (1u << MIDI_PIN);
}

static inline void midi_line(int mark)   /* drive the MIDI line; mark=1 is idle/high */
{
	int p = MIDI_INVERT ? !mark : mark;
	if (p) NRF_P0->OUTSET = (1u << MIDI_PIN);
	else   NRF_P0->OUTCLR = (1u << MIDI_PIN);
}

/* Streaming-safe MIDI byte TX: a free hardware timer (TIMER2 — the board binds
 * no TIMER) clocks out the UART bits one per ISR. The start bit is driven when
 * the byte is queued; the timer then drives the 8 data bits (LSB first) + stop
 * bit at MIDI_BIT_US spacing. Interrupts stay ON throughout, so the eMMC/I2S
 * ISRs are never masked (the fix for the >3-track crackle). Only midi_thread
 * calls midi_send, sequentially, and MIDI bytes are >=31ms apart in practice,
 * so the single-byte-in-flight guard (midi_tx_done) never actually contends. */
#define MIDI_TIMER       NRF_TIMER2
#define MIDI_TIMER_IRQn  TIMER2_IRQn
static volatile uint16_t midi_tx_bits;     /* remaining frame, LSB = next bit out */
static volatile uint8_t  midi_tx_left;     /* bits still to clock (0 = done) */
static struct k_sem      midi_tx_done;     /* 1 = line free for the next byte */

static void midi_timer_isr(const void *arg)
{
	ARG_UNUSED(arg);
	MIDI_TIMER->EVENTS_COMPARE[0] = 0;
	(void)MIDI_TIMER->EVENTS_COMPARE[0];        /* flush the clear (nRF anomaly) */
	if (midi_tx_left) {
		midi_line(midi_tx_bits & 1u);       /* drive this bit (PNP-inverted) */
		midi_tx_bits >>= 1;
		midi_tx_left--;
	} else {
		MIDI_TIMER->TASKS_STOP = 1;
		midi_line(1);                       /* leave the line idle at mark */
		k_sem_give(&midi_tx_done);
	}
}

static void midi_timer_init(void)
{
	MIDI_TIMER->MODE      = TIMER_MODE_MODE_Timer;
	MIDI_TIMER->BITMODE   = TIMER_BITMODE_BITMODE_16Bit;
	MIDI_TIMER->PRESCALER = 4;                          /* 16MHz/16 = 1us tick */
	MIDI_TIMER->CC[0]     = MIDI_BIT_US;                /* fire every 32us = 1 bit */
	MIDI_TIMER->SHORTS    = TIMER_SHORTS_COMPARE0_CLEAR_Msk;
	MIDI_TIMER->INTENSET  = TIMER_INTENSET_COMPARE0_Msk;
	k_sem_init(&midi_tx_done, 1, 1);                    /* start with the line free */
	IRQ_CONNECT(MIDI_TIMER_IRQn, 2, midi_timer_isr, NULL, 0);
	irq_enable(MIDI_TIMER_IRQn);
}

static void midi_send(uint8_t b)
{
	/* wait for any in-flight byte to finish (in practice it always has) */
	if (k_sem_take(&midi_tx_done, K_MSEC(5)) != 0) return;   /* stuck -> skip byte */
	/* The ENTIRE 10-bit frame is timer-clocked -- start(0), d0..d7 (LSB first),
	 * stop(1). The START bit is the timer's FIRST event, NOT driven here, so every
	 * edge is timer-paced; a thread preemption between here and TASKS_START can no
	 * longer stretch the start bit and corrupt the framing. */
	midi_tx_bits = ((uint16_t)b << 1) | (1u << 9);   /* bit0=start(0), d0..d7 @1..8, stop @9 */
	midi_tx_left = 10;                                /* start + 8 data + stop */
	midi_line(1);                                     /* hold idle/mark until the 1st ISR */
	MIDI_TIMER->TASKS_CLEAR = 1;
	MIDI_TIMER->TASKS_START = 1;                      /* 1st ISR (+32us) emits the START bit */
}

/* BASIC MIDI ONLY: just Start/Stop + 24-PPQN clock on the MIDI line. The
 * Pocket-Operator / Volca 2-PPQN sync (the POSYNC GPIO pulses + k_uptime polling)
 * has been removed to keep this thread minimal. */
static void midi_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	uint32_t consumed = 0;
	while (1) {
		if (g_midi_start_pending) { g_midi_start_pending = 0; midi_send(0xFA); }
		if (g_midi_stop_pending)  { g_midi_stop_pending  = 0; midi_send(0xFC); }
		uint32_t prod = g_midi_clk_produced;
		if (consumed != prod) {
			if ((uint32_t)(prod - consumed) > 96u) {
				/* absurd backlog (>4 beats — a stall or a counter
				 * glitch): RESYNC instead of blasting the difference,
				 * because each clock byte locks IRQs ~320 us and a huge
				 * catch-up burst starves everything below PREEMPT(6). */
				consumed = prod;
			} else {
				consumed++;
				midi_send(0xF8);               /* MIDI clock, 24 PPQN */
			}
		} else {
			k_msleep(1);
		}
	}
}

static void audio_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	struct i2s_config cfg = {
		/* SLAVE on both clocks (TE native topology): the 3.072 MHz oscillator
		 * drives SCLK, the CS42L42 masters LRCK at exactly 48 kHz (64 SCLK per
		 * frame). We still send 16-bit samples — the nRF shifts the 16 MSBs of
		 * each 32-SCLK half-frame, which both codecs (set for MSB-first slots)
		 * decode correctly; the remaining LSBs are below the 16-bit noise floor. */
		.word_size      = 16,
		.channels       = 2,
		.format         = I2S_FMT_DATA_FORMAT_I2S,
		.options        = I2S_OPT_FRAME_CLK_SLAVE | I2S_OPT_BIT_CLK_SLAVE,
		.frame_clk_freq = I2S_SR,
		.mem_slab       = &tx_slab,
		.block_size     = BLK_BYTES,
		.timeout        = 2000,
	};

	if (!device_is_ready(i2s_dev)) { audio_cfg_rc = -100; return; }

	audio_cfg_rc = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
	if (audio_cfg_rc != 0) return;

	/* Prime a few silent blocks, then START. After this the loop refills the
	 * DMA continuously with NO long gap, so the TX stream never underruns.
	 * The codec is configured separately on the main thread (it needs BCLK,
	 * which is live the moment we signal audio_started). */
	for (int i = 0; i < 4; i++) {
		void *blk;
		if (k_mem_slab_alloc(&tx_slab, &blk, K_FOREVER) != 0)
			continue;
		fill_block(blk);
		if (i2s_write(i2s_dev, blk, BLK_BYTES) != 0)
			k_mem_slab_free(&tx_slab, blk);
	}
	i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	audio_started = true;

	int wfail = 0;                       /* consecutive i2s_write failures */
	while (1) {
		void *blk;
		if (k_mem_slab_alloc(&tx_slab, &blk, K_FOREVER) != 0)
			continue;

		/* Looper engine: drains the live USB input (prebuffer-gated inside;
		 * silence if the host isn't streaming) and mixes the 4 tracks on top.
		 * DWT-timed: worst-case exec must stay far below the 5.33 ms block
		 * budget — aus= in the diag definitively exonerates (or convicts)
		 * the CPU path for the crackle. */
		uint32_t _c0 = DWT->CYCCNT;
		looper_audio_block(blk);
		uint32_t _cus = (DWT->CYCCNT - _c0) / 64u;   /* 64 MHz -> us */
		if (_cus > g_audio_us_max) g_audio_us_max = _cus;
		if (_cus > g_audio_us_win) g_audio_us_win = _cus;

		int wrc = i2s_write(i2s_dev, blk, BLK_BYTES);
		if (wrc != 0) {
			g_i2s_wfail_cnt++;   /* diag: I2S path failure counter */
			k_mem_slab_free(&tx_slab, blk);
			/* FAILSAFE: if the I2S TX ever errors into the stopped state, every
			 * write fails forever and the device latches SILENT until reboot.
			 * After a burst of consecutive failures, drop + re-prime + restart
			 * the stream instead of staying mute. */
			if (++wfail >= 8) {
				wfail = 0;
				(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
				for (int i = 0; i < 4; i++) {
					void *pb;
					if (k_mem_slab_alloc(&tx_slab, &pb, K_NO_WAIT) != 0)
						break;
					fill_block(pb);
					if (i2s_write(i2s_dev, pb, BLK_BYTES) != 0)
						k_mem_slab_free(&tx_slab, pb);
				}
				(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
			}
			continue;
		}
		wfail = 0;
	}
}

/* Bring up the audio output path: osc on, codec configured, stream started. */
static void audio_init(void)
{
	/* The 3.072 MHz oscillator IS the bus bit-clock in TE's topology — turn it
	 * ON. (The old crackle when enabling it came from the nRF ALSO mastering
	 * SCLK = two drivers on one line; the nRF is a clock slave now.) */
	gpio_drive_high(OSC_EN_PORT, OSC_EN_PIN);
	k_msleep(5);
	k_thread_create(&audio_tcb, audio_stack, K_THREAD_STACK_SIZEOF(audio_stack),
			audio_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(ST_PRIO_AUDIO), 0, K_NO_WAIT);
	/* PREEMPT(0), not COOP(7): still outranks every other app thread (main 8,
	 * streamer 5, MIDI 6 — none can preempt it), but the COOP USB/UDC stack
	 * threads can now interrupt the mixer for their ~100 us ISO service.
	 * MEASURED on hardware: with the mixer non-preemptible, the USB
	 * controller lost ~600 incoming audio frames/s ONLY while recording
	 * (SOF heartbeat perfect, rx pool untouched) — silence stitched into
	 * every take = THE 4-track crackle. Shared state with the USB threads is
	 * one SPSC ring buffer and one mem-slab, both preemption-safe. */

	/* eMMC streamer: preemptible + below the audio thread so audio always
	 * wins. Guarded: in the charge-standby path it was already started
	 * early so the gauge can read the saved brightness (v1.2.3). */
	streamer_start();

	/* MIDI clock + PO sync out over the SYNC jack (TimK pins). The MIDI byte TX
	 * is now clocked by a hardware timer (midi_timer_init) so it no longer masks
	 * interrupts -- the >3-track crackle fix. Thread is low priority; it just
	 * flags bytes + drives the PO-sync GPIO pulse. */
#if MIDI_SYNC_ENABLE
	midi_pins_init();
	midi_timer_init();
	k_thread_create(&midi_tcb, midi_stack, K_THREAD_STACK_SIZEOF(midi_stack),
			midi_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(ST_PRIO_MIDI), 0, K_NO_WAIT);
#endif

	/* Wait until the audio thread has the I2S stream running (BCLK live), then
	 * configure the codec here on the main thread. The audio thread keeps the
	 * DMA fed throughout, so its config sleeps never starve the I2S. */
	for (int i = 0; i < 100 && !audio_started; i++)
		k_msleep(2);
	tas2505_configure();
}

/* STEM TAPE: UAC2 -- the explicit-feedback software regulator and every
 * UAC2 application callback (terminal_update/get_recv_buf/data_recv/
 * buf_release/feedback/sof, and the sp1_uac2_ops table registering them)
 * are REMOVED (see this file's own top-of-file comment). */

/* ---- USB MIDI 2.0 (receive-only) application callbacks --------------------
 * GATE 1: KO II pad events arrive here (via a standalone MIDI bridge acting
 * as USB host to the SP-1's class-compliant USB-MIDI device port -- this
 * device never becomes a USB host itself). No outgoing MIDI, no MIDI clock;
 * only Note On/Off + All Notes Off are decoded (st_midi_decode_ump32(), the
 * SAME real decoder host-tested in tests/test_stemtape_player.c against
 * synthetic UMP words -- this callback just unwraps the real Zephyr
 * struct midi_ump into its first 32-bit word and calls it). Cue LEARNING and
 * AUDITION (Gate 3) are not implemented yet; this gate's real, working
 * scope is: reception, decoding, a bounded queue, and a minimal held-note
 * tracker (g_midi_held[]) that already gives correct, testable behavior for
 * "which pad(s) are currently down" -- the exact substrate Gate 3 needs. */
#define USB_MIDI_DT_NODE DT_NODELABEL(usb_midi)
static const struct device *const midi_dev = DEVICE_DT_GET(USB_MIDI_DT_NODE);

static st_midi_queue_t g_midi_rx_q;

/* Bounded "currently held" set -- channel:note identity per
 * docs/FIRMWARE_CONTRACT_V1.md §6. 8 concurrent pads is generous for a
 * single KO II (16 pads, but simultaneous physical presses are limited by
 * hand count) and bounds this table's cost the same way ST_MIDI_QUEUE_
 * CAPACITY bounds the raw event queue. */
#define MIDI_HELD_MAX 8u
struct midi_held_note { uint8_t channel; uint8_t note; bool active; };
static struct midi_held_note g_midi_held[MIDI_HELD_MAX];
static volatile uint8_t g_midi_held_count;
/* diag: most recent event, for controls_diag() visibility (Gate 5 CDC diag). */
static volatile uint8_t g_midi_last_channel = 0xFFu;
static volatile uint8_t g_midi_last_note;
static volatile uint8_t g_midi_last_kind; /* st_midi_evt_kind_t */

static void midi_held_add(uint8_t channel, uint8_t note)
{
	for (uint32_t i = 0; i < MIDI_HELD_MAX; i++) {
		if (g_midi_held[i].active && g_midi_held[i].channel == channel &&
		    g_midi_held[i].note == note) {
			return; /* already held (duplicate Note On): no-op */
		}
	}
	for (uint32_t i = 0; i < MIDI_HELD_MAX; i++) {
		if (!g_midi_held[i].active) {
			g_midi_held[i].active = true;
			g_midi_held[i].channel = channel;
			g_midi_held[i].note = note;
			g_midi_held_count++;
			return;
		}
	}
	/* table full: a real, bounded, silently-ignored overflow (matches the
	 * event queue's own "never grow, never block" policy) -- the pad
	 * physically can't be tracked as held, but nothing corrupts. */
}

static void midi_held_remove(uint8_t channel, uint8_t note)
{
	for (uint32_t i = 0; i < MIDI_HELD_MAX; i++) {
		if (g_midi_held[i].active && g_midi_held[i].channel == channel &&
		    g_midi_held[i].note == note) {
			g_midi_held[i].active = false;
			if (g_midi_held_count > 0) g_midi_held_count--;
			return;
		}
	}
	/* unmatched Note Off (no corresponding held note): deterministic no-op */
}

static void midi_held_clear_all(void)
{
	for (uint32_t i = 0; i < MIDI_HELD_MAX; i++) {
		g_midi_held[i].active = false;
	}
	g_midi_held_count = 0;
}

/* Drains g_midi_rx_q and updates the held-note table. Called once per
 * main()'s ~8 ms control-loop pass (see main(), same cadence as every other
 * control-surface poll) -- single consumer, matching the queue's documented
 * single-producer/single-consumer contract. */
static void midi_service(void)
{
	st_midi_event_t ev;

	while (st_midi_queue_pop(&g_midi_rx_q, &ev)) {
		g_midi_last_channel = ev.channel;
		g_midi_last_note = ev.note;
		g_midi_last_kind = (uint8_t)ev.kind;
		switch (ev.kind) {
		case ST_MIDI_EVT_NOTE_ON:
			midi_held_add(ev.channel, ev.note);
			break;
		case ST_MIDI_EVT_NOTE_OFF:
			midi_held_remove(ev.channel, ev.note);
			break;
		case ST_MIDI_EVT_ALL_NOTES_OFF:
			midi_held_clear_all();
			break;
		}
	}
}

static void midi_rx_packet_cb(const struct device *dev, const struct midi_ump ump)
{
	ARG_UNUSED(dev);
	st_midi_event_t ev;

	if (st_midi_decode_ump32(ump.data[0], &ev)) {
		st_midi_queue_push(&g_midi_rx_q, &ev);
	}
}

static void midi_ready_cb(const struct device *dev, const bool ready)
{
	ARG_UNUSED(dev);
	if (!ready) {
		/* USB reset/disconnect/interface disabled: the same All-Notes-Off
		 * event that a real CC123 would produce, through the SAME queue --
		 * one mechanism, not a special case. Cancels any held pad state
		 * (and, once Gate 3 lands, any active cue audition) safely. */
		st_midi_event_t anof = { .kind = ST_MIDI_EVT_ALL_NOTES_OFF,
					  .channel = 0, .note = 0, .velocity = 0 };
		st_midi_queue_push(&g_midi_rx_q, &anof);
	}
}

static const struct usbd_midi_ops sp1_midi_ops = {
	.rx_packet_cb = midi_rx_packet_cb,
	.ready_cb     = midi_ready_cb,
};

/* Bring up the composite USB device (USB-MIDI 2.0 + CDC console) on
 * device_next. set_ops MUST precede usbd_enable for every class or its
 * init fails. STEM TAPE: no longer brings up UAC2 audio (see this file's
 * own top-of-file comment). */
static void usb_audio_start(void)
{
	struct usbd_context *usbd;

	if (!device_is_ready(midi_dev)) {
		printk("usb-midi device not ready\n");
		return;
	}

	st_midi_queue_init(&g_midi_rx_q);
	usbd_midi_set_ops(midi_dev, &sp1_midi_ops);

	usbd = sample_usbd_init_device(NULL);
	if (usbd == NULL) {
		printk("usbd init failed\n");
		return;
	}

	/* Pin bcdDevice to a new release number. Windows caches USB descriptors
	 * per VID/PID/version — without a version bump a PC that saw the old
	 * (Code-10) audio descriptor keeps judging a re-flashed SP-1 by the
	 * cached copy and can stay broken even after the fix. */
	(void)usbd_device_set_bcd_device(usbd, 0x0200);

	if (usbd_enable(usbd) != 0) {
		printk("usbd enable failed\n");
	}

#if SP1_XFER_ENABLE
	/* Register the CDC RX callback AND enable RX now. On this USB stack the
	 * CDC-ACM class only queues its FIRST receive transfer from
	 * uart_irq_rx_enable() — with it off the endpoint never accepts a single
	 * byte and the transfer site can never connect (GitHub issue #1). The ISR
	 * just moves bytes into a ring; while looping its cost is zero unless the
	 * host actually sends something. */
	uart_irq_callback_user_data_set(cdc, cdc_rx_isr, NULL);
	uart_irq_rx_enable(cdc);
#endif
}

/* Stream the raw ladder codes, but ONLY when a host has opened the port
 * (DTR asserted). That keeps us from ever stalling the watchdog loop when
 * nothing is listening. Throttled by the caller. */
/* =====================================================================
 * SEMITONE grid for the tempo rocker's DOUBLE-CLICK: 2^(k/12) in Q16 for
 * k = -12..+12 (0.5x..2.0x; the BPM clamp bounds the usable range). A
 * double-click jumps the speed to the next exact equal-tempered semitone
 * relative to 1.0x (= 80 BPM) — one musical pitch step instead of forty
 * 1-BPM clicks — and a detuned speed SNAPS ONTO the grid rather than
 * drifting off it. Integer-only; the exact Q16 speed is what the song
 * saves, so semitone speeds survive power-off bit-exact. */
static const uint32_t k_semi_q16[25] = {
	32768u,  34716u,  36781u,  38968u,  41285u,  43740u,  46341u,
	49097u,  52016u,  55109u,  58386u,  61858u,  65536u,  69433u,
	73562u,  77936u,  82570u,  87480u,  92682u,  98193u,  104032u,
	110218u, 116772u, 123715u, 131072u,
};

static uint32_t semitone_next(uint32_t sp, int dir)
{
	/* within ~0.4% of a grid point counts as ON it (absorbs BPM-integer
	 * rounding; far below the 5.9% semitone spacing) */
	if (dir > 0) {
		for (int k = 0; k < 25; k++)
			if (k_semi_q16[k] > sp + sp / 250u)
				return k_semi_q16[k];
		return k_semi_q16[24];
	}
	for (int k = 24; k >= 0; k--)
		if (k_semi_q16[k] < sp - sp / 250u)
			return k_semi_q16[k];
	return k_semi_q16[0];
}

/* Marked unused because the calibration image suppresses its ONE call site
 * (see the control loop) and this would otherwise warn as an unused static.
 * The body is still not dead weight there: Zephyr links with
 * -ffunction-sections and --gc-sections, so an uncalled static is dropped
 * from the image rather than carried in it.
 *
 * Spelled as the bare GCC attribute rather than Zephyr's __maybe_unused:
 * nothing else in this tree uses that macro, so whether it reaches this
 * translation unit is unverified, and the only way to find out would be a
 * CI round. The attribute is unconditionally available on this toolchain. */
/*
 * THE MEASUREMENT WAS CAUSING THE THING IT MEASURED.
 *
 * st49's capture settled this by arithmetic rather than by argument. Per
 * 2-second diagnostic window, 35 of 52 windows moved sil by EXACTLY 120
 * frames and und by exactly 1 -- one episode, one quantum, once per print.
 * That periodicity is not contention; contention is not quantised. And iwf
 * was 0 in all 52 samples, so the audio block was never missing its I2S
 * deadline: the frames were silenced because the STREAMER had not delivered,
 * not because the renderer was late.
 *
 * The mechanism is scheduling. main and the streamer both run at PREEMPT(1),
 * so a k_yield() from the streamer's per-read breather hands main the CPU --
 * and main then holds it for the WHOLE print burst, nine printk lines into a
 * CDC ring, measured at 2% of a 2000 ms window (~40 ms). The stem ring holds
 * G-R = 4 groups = 28.3 ms. Forty milliseconds of no reads empties it, once
 * per window, for exactly as long as it takes the streamer to get back in.
 *
 * A yield between the lines is the whole fix. Each printk becomes its own
 * scheduling point, so the streamer interleaves with the burst instead of
 * queueing behind it, and no single gap can approach the ring's depth. It
 * costs nothing when no thread is ready, and it cannot reorder or drop a
 * line: printk is atomic per call and the yield sits strictly between calls.
 *
 * WORTH SAYING PLAINLY: this affects CAPTURES, not the product. controls_diag()
 * returns at its own DTR check when no host has the port open, so a device
 * playing without a console attached never ran any of this. The reason to fix
 * it anyway is that every remaining diagnosis is read off these captures, and
 * a diagnostic that manufactures its own underruns poisons the evidence.
 */
static void diag_breathe(void)
{
	k_yield();
}

static void controls_diag(void) __attribute__((unused));
static void controls_diag(void)
{
	/* Stream one status line over USB-serial, but ONLY when a host has opened
	 * the port (DTR asserted) — otherwise printk could stall the control loop.
	 * Throttled by the caller. Healthy: tracks PLAY, ovr=0 (no record-buffer
	 * overflow), rerr=0/werr=0 (clean storage bus). */
	uint32_t dtr = 0;
	(void)uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr);
	if (!dtr)
		return;

	static const char *const tsn[] = { "---", "ARM", "REC", "DON", "PLY" };
	int batt = ladder_read(&adc_ladder[LAD_BATT]);   /* raw 12-bit, battery divider */
	uint32_t cpos = g_consume_pos;
	int mg[NTRK];
	for (int _i = 0; _i < NTRK; _i++)
		mg[_i] = (int)((int32_t)(trk[_i].p_w - cpos) / (int)(LOOP_RATE / 1000u));
	printk("LOOPER b=%s %dHz song=%d %s hp=%d hpin=%d usb=%d chg=%d batt=%d bpm=%d detbpm=%d vol=%d "
	       "trk[%s %s %s %s] rec=%d mut=%u%u%u%u ovr=%u rerr=%u werr=%u marg=[%d %d %d %d]ms stv=[%u %u %u %u] len=[%u %u %u %u] st=[%u %u %u %u] spim=%d cache=%d ckb=%u wbi=%u chop=%u/%u\n",
	       ST_BUILD_TAG,
	       (int)LOOP_RATE, (int)g_slot, g_playing ? "PLAY" : "STOP", g_hp_on, g_hp_in,
	       usb_present() ? 1 : 0, charging() ? 1 : 0, batt,
	       g_play_bpm, g_det_bpm, g_master_vol_q8,
	       tsn[trk[0].state % 5], tsn[trk[1].state % 5],
	       tsn[trk[2].state % 5], tsn[trk[3].state % 5],
	       g_rec_track,
	       (unsigned)trk[0].muted, (unsigned)trk[1].muted,
	       (unsigned)trk[2].muted, (unsigned)trk[3].muted,
	       (unsigned)g_rec_overruns,
	       (unsigned)emmc_crc_rd_errs, (unsigned)emmc_crc_wr_errs,
	       mg[0], mg[1], mg[2], mg[3],
	       (unsigned)g_starve_cnt[0], (unsigned)g_starve_cnt[1], (unsigned)g_starve_cnt[2], (unsigned)g_starve_cnt[3],
	       (unsigned)trk[0].len_blocks, (unsigned)trk[1].len_blocks, (unsigned)trk[2].len_blocks, (unsigned)trk[3].len_blocks,
	       (unsigned)trk[0].start_blk, (unsigned)trk[1].start_blk, (unsigned)trk[2].start_blk, (unsigned)trk[3].start_blk,
	       emmc_spim_active() ? 1 : 0, g_cache_on ? 1 : 0, (unsigned)g_cache_kb, (unsigned)emmc_dbg_wr_busy_max,
	       (unsigned)g_chop_div, (unsigned)g_chop_off);
	diag_breathe();
	{
		/* CPU= per-thread share of the last window, in percent: audio,
		 * streamer, midi, main, everything-else(usb/idle/isr). Answers
		 * WHERE the cycles actually go when refill can't build surplus. */
		static uint64_t l_aud, l_str, l_mid, l_mai, l_all;
		k_thread_runtime_stats_t rs;
		uint64_t aud = 0, str = 0, mid = 0, mai = 0, all = 0;
		if (!k_thread_runtime_stats_get(&audio_tcb, &rs))    aud = rs.execution_cycles;
		if (!k_thread_runtime_stats_get(&streamer_tcb, &rs)) str = rs.execution_cycles;
		if (!k_thread_runtime_stats_get(&midi_tcb, &rs))     mid = rs.execution_cycles;
		if (!k_thread_runtime_stats_get(k_current_get(), &rs)) mai = rs.execution_cycles;
		if (!k_thread_runtime_stats_all_get(&rs))            all = rs.execution_cycles;
		uint64_t d_all = all - l_all;
		if (d_all) {
			/* diag= is main's own share spent PRINTING, as a
			 * percentage of the window it was measured over -- so
			 * main-minus-diag is the real control workload, and the
			 * question "is the deficit partly the act of measuring"
			 * is answered by one number instead of inferred.
			 *
			 * prio= makes the capture self-describing: audio /
			 * streamer / main / midi, so a log can never be read
			 * against the wrong scheduling assumption again. */
			uint32_t dcyc = g_diag_cyc_win;
			uint32_t dwin_us = (uint32_t)(g_diag_window_ms * 1000);

			g_diag_cyc_win = 0u;
			printk("CPU aud=%u%% str=%u%% midi=%u%% main=%u%% diag=%u%% prio=%d/%d/%d/%d\n",
			       (unsigned)((aud - l_aud) * 100u / d_all),
			       (unsigned)((str - l_str) * 100u / d_all),
			       (unsigned)((mid - l_mid) * 100u / d_all),
			       (unsigned)((mai - l_mai) * 100u / d_all),
			       (unsigned)(dwin_us ? ((dcyc / 64u) * 100u) / dwin_us : 0u),
			       ST_PRIO_AUDIO, ST_PRIO_STREAMER, ST_PRIO_MAIN, ST_PRIO_MIDI);
			diag_breathe();
		}
		l_aud = aud; l_str = str; l_mid = mid; l_mai = mai; l_all = all;
	}
	{
		/* STACK= bytes of each thread stack that have NEVER been touched,
		 * from the 0xAA fill CONFIG_INIT_STACKS lays down at creation.
		 *
		 * THIS IS A MEASUREMENT, NOT A BUDGET, and it exists because the
		 * RAM audit is not permitted to shrink a stack from a guess.
		 * 12,672 bytes are allocated across these four threads plus idle
		 * (128) and the ISR stack (512), and every one of those numbers
		 * was chosen rather than derived -- the streamer's 4096 says "the
		 * eMMC driver is -O2 here", the audio thread's 3072 says "+1K
		 * margin over the historical 2048". None has ever been measured
		 * against real use, so none may be changed until this line has
		 * been read off real hardware.
		 *
		 * HOW TO READ IT. Run a session that has looped, changed loop
		 * length, held chords, transferred a song and played one to the
		 * end; the SMALLEST unused figure seen across all of that is the
		 * only honest headroom. A value at or near the full size means
		 * the thread has barely run yet, not that it is safe. */
		size_t un_aud = 0, un_str = 0, un_mid = 0, un_mai = 0;

		(void)k_thread_stack_space_get(&audio_tcb, &un_aud);
		(void)k_thread_stack_space_get(&streamer_tcb, &un_str);
		(void)k_thread_stack_space_get(&midi_tcb, &un_mid);
		(void)k_thread_stack_space_get(k_current_get(), &un_mai);
		/* NOT WHILE PLAYING. Unused-stack watermarks are a development
		 * metric that moves once a boot, and this is the one line of the
		 * block that no diagnosis needs at playback time -- so it is the
		 * one line that can be dropped for free when the CPU is the
		 * scarce resource. Everything the analyzer parses stays. */
		if (!g_playing)
		printk("STACK unused aud=%u/%u str=%u/%u midi=%u/%u main=%u/%u\n",
		       (unsigned)un_aud, (unsigned)K_THREAD_STACK_SIZEOF(audio_stack),
		       (unsigned)un_str, (unsigned)K_THREAD_STACK_SIZEOF(streamer_stack),
		       (unsigned)un_mid, (unsigned)K_THREAD_STACK_SIZEOF(midi_stack),
		       (unsigned)un_mai, (unsigned)CONFIG_MAIN_STACK_SIZE);
		diag_breathe();
	}
	/* The PASS2 line is GONE with PASS 2 itself. Every counter it printed
	 * (per-track refilled blocks, dead-history snaps, round aborts, read
	 * failures) belonged to the classic play-ring read-ahead, so on this
	 * firmware it could only ever have printed zeros -- a diagnostic that
	 * reports nothing but zeros about work that cannot happen is worse than
	 * no diagnostic, because it reads as evidence. The command-retry count
	 * it also carried is real and moves to the EMMC48 line below. */
	extern volatile uint32_t emmc_dbg_cmd_retries;
	{
		/* THE stall numbers, finally wall-clock: wus=write-busy window/session
		 * max (us), rus=read-access wait, sus=CMD6 busy (cache flush / future
		 * TRIM+BKOPS), bto=busy-poll expiries, low=worst play margin this
		 * window (ms), hiw=worst rec fill (ms), gl=stored glitches (REPEATING
		 * artifacts), iwf=i2s failures, aus=worst audio-block exec us,
		 * rt=command-response retries recovered (moved here from the
		 * removed PASS2 line), ec=EXT_CSD[167,166,231,502,503,198,246,192,175]. */
		int32_t _lwv = g_play_lowat;
		int _lw = (_lwv == 0x7FFFFFFF) ? -1
			  : (int)(_lwv / (int32_t)(LOOP_RATE / 1000u));
		printk("EMMC48 wus=%u/%u rus=%u sus=%u bto=%u low=%dms hiw=%ums gl=%u iwf=%u aus=%u rt=%u rr=%x flt=%x@%x hi=%u,%u ec=%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x\n",
		       (unsigned)emmc_dbg_wr_busy_us_max, (unsigned)emmc_dbg_wr_busy_us_peak,
		       (unsigned)emmc_dbg_rd_wait_us_max, (unsigned)emmc_dbg_switch_busy_us_max,
		       (unsigned)emmc_dbg_busy_timeouts, _lw,
		       (unsigned)(g_rec_hiwat / (LOOP_RATE / 1000u)),
		       (unsigned)g_stored_glitch_cnt, (unsigned)g_i2s_wfail_cnt,
		       (unsigned)g_audio_us_max,
		       (unsigned)emmc_dbg_cmd_retries,   /* rt=: was on the removed PASS2 line */
		       (unsigned)g_resetreas,
		       (unsigned)g_last_fault_reason, (unsigned)g_last_fault_pc,
		       (unsigned)g_hpi_on, (unsigned)emmc_dbg_hpi_fires,
		       g_extcsd_dump[0], g_extcsd_dump[1], g_extcsd_dump[2],
		       g_extcsd_dump[3], g_extcsd_dump[4], g_extcsd_dump[5],
		       g_extcsd_dump[6], g_extcsd_dump[7], g_extcsd_dump[8]);
		diag_breathe();
	}
	/* STEM TAPE: the "USBIN" diag block (UAC2 receive-rate/ring/feedback
	 * health) is REMOVED along with UAC2 itself -- see this file's own
	 * top-of-file comment. */
#if SP1_XFER_ENABLE
	/* STEM TAPE Phase 2 slice 3B.1: internal-only stored-song streaming
	 * diagnostics (no LEDs, no user-facing indication -- see this
	 * codebase's own USB-serial diagnostic convention). Session-
	 * cumulative (never reset here), matching g_rec_overruns/g_audio_
	 * us_max's own convention, not the per-window-reset PASS2 line's:
	 * rdby=total bytes read (successful reads only), rdc=read-call
	 * count (every attempt), rdus=latest read duration (us), rdusmx=
	 * worst read duration this session (us), rate=calculated sustained
	 * read rate (bytes/s, successful-read time only -- see stem_diag_
	 * sustained_read_bytes_per_sec()'s own doc comment), und=buffer
	 * SIL= IS THE ONE THAT SAYS WHETHER ANYTHING WAS AUDIBLE. und= counts
	 * the TRANSITION into underrun and nothing about its length, so one
	 * stalled frame and one stalled block both count as 1 -- read as a
	 * dropout count it is off by up to 256x, and it was: a run that sounded
	 * perfect reported 384 "underruns", anywhere between 8 ms of inaudible
	 * silence and 2 full seconds, with no way to tell which. sil= is frames
	 * actually silenced, which is the quantity a listener hears. It was
	 * previously printed only by the retired planar A/B; it lives here now
	 * so it survives that harness.
	 *
	 * und=underrun episode count, corr=corrupt-sector count, reloadfail=Slice
	 * C3 post-commit runtime reloads audio_thread's own validation
	 * rejected (should be ~impossible in practice -- see g_stem_reload_
	 * fail_count's own doc comment). */
	/* READ-AND-CLEAR, so rduswin= is the worst fetch since the LAST time
	 * this line printed rather than since boot. rdusmx= sticks at whatever
	 * outlier boot produced and stops moving; a windowed figure is what
	 * actually tracks whether the read path is degrading under a change. */
	printk("STEMIO rdby=%u rdc=%u rdus=%u rdusmx=%u rduswin=%u rate=%uBps "
	       "sil=%u und=%u corr=%u reloadfail=%u\n",
	       (unsigned)atomic_get(&g_stem_diag_bytes_total), (unsigned)atomic_get(&g_stem_diag_read_calls),
	       (unsigned)atomic_get(&g_stem_diag_read_us_last), (unsigned)atomic_get(&g_stem_diag_read_us_max),
	       (unsigned)atomic_set(&g_stem_diag_read_us_win, 0),
	       (unsigned)stem_diag_sustained_read_bytes_per_sec(),
	       (unsigned)atomic_get(&g_stem_underrun_frames), (unsigned)atomic_get(&g_stem_underrun_count),
	       (unsigned)atomic_get(&g_stem_corrupt_count), (unsigned)atomic_get(&g_stem_reload_fail_count));
	diag_breathe();
	/* THE SAME FAULTS, BROKEN OUT BY STEM, in stem order
	 * (0 vocal, 1 drums, 2 bass, 3 instrument). miss= is "the group this
	 * stem needed was not resident"; bad= is "the fetch came back as some
	 * other stem or span". A single non-zero column is a per-stem fault --
	 * exactly the shape of one track dropping out while the rest play on --
	 * and four columns moving together is an ordinary shared stall. */
	printk("STEMPS miss=%u,%u,%u,%u bad=%u,%u,%u,%u\n",
	       (unsigned)atomic_get(&g_stem_miss[0]), (unsigned)atomic_get(&g_stem_miss[1]),
	       (unsigned)atomic_get(&g_stem_miss[2]), (unsigned)atomic_get(&g_stem_miss[3]),
	       (unsigned)atomic_get(&g_stem_badhdr[0]), (unsigned)atomic_get(&g_stem_badhdr[1]),
	       (unsigned)atomic_get(&g_stem_badhdr[2]), (unsigned)atomic_get(&g_stem_badhdr[3]));
	diag_breathe();
	/* THE DROPOUT LINE. zeroms= is the longest stretch each stem decoded as
	 * pure silence, in milliseconds; at= is the song frame it started on.
	 * Only runs past ST_STEM_ZERO_RUN_MIN are reported, so ordinary quiet
	 * passages and gaps between notes never appear here -- an entry means a
	 * stem was digitally, exactly zero for about a second or more while its
	 * gain was up. See ST_STEM_ZERO_RUN_MIN for what each reading proves. */
	{
		/* Stride samples -> ms. FRAMES FIRST, then divide: one stride
		 * is 32 frames and 32*1000/48000 is ZERO in integer arithmetic,
		 * so a per-sample constant would have reported every dropout as
		 * 0 ms. Multiplying first keeps it exact and cannot overflow --
		 * a full hour of silence is 5.4 million frames. */
		uint32_t zw[ST11_STEM_COUNT], za[ST11_STEM_COUNT], zi;
		uint32_t zms[ST11_STEM_COUNT];

		for (zi = 0; zi < ST11_STEM_COUNT; zi++) {
			zw[zi] = (uint32_t)atomic_get(&g_stem_zero_worst[zi]);
			za[zi] = (uint32_t)atomic_get(&g_stem_zero_at[zi]);
			zms[zi] = (zw[zi] * ST_STEM_METER_STRIDE) /
				  (ST11_SAMPLE_RATE_HZ / 1000u);
		}
		printk("STEMZ zeroms=%u,%u,%u,%u at=%u,%u,%u,%u\n",
		       (unsigned)zms[0], (unsigned)zms[1],
		       (unsigned)zms[2], (unsigned)zms[3],
		       (unsigned)za[0], (unsigned)za[1], (unsigned)za[2], (unsigned)za[3]);
		diag_breathe();
	}
	/* WHERE A STEM READ'S TIME GOES, AVERAGED OVER THIS PRINT WINDOW (see
	 * sp1_emmc.h's own "READ-PATH PHASE BREAKDOWN"). Every figure is
	 * per-read: the window's sum divided by the reads in it, then the
	 * counters are cleared. One read is R groups = 4R blocks, so n= says
	 * how much this average rests on.
	 *
	 *   avg=  the whole read call. The identity that matters: four 24-bit
	 *         stereo stems at 48 kHz need 1,152,000 B/s, a read delivers
	 *         ST_PL_REFILL_GROUPS * 2048 bytes, so avg must stay under
	 *         (bytes / 1152000) or the streamer cannot keep up even at
	 *         100% duty. At R=2 that ceiling is 3555 us.
	 *   hunt= the bit-banged start-bit search, all blocks, and
	 *   spin= the part of it actually issuing clock pulses. hunt-spin is
	 *         time this thread held the bus and clocked nothing -- the
	 *         k_usleep long-stall yield, or preemption between bursts.
	 *   clk=  the pulse count, and pulseavg= = spin/clk is the average cost
	 *         of one GPIO toggle. pulsemax= is the worst SINGLE pulse in
	 *         the window. Together these three settle the 30x anomaly:
	 *
	 *           hunt >> spin            -> not clocking. Descheduled
	 *                                      between bursts, or yielding.
	 *           pulseavg ~= pulsemax    -> the loop itself is slow, and
	 *             and both large           uniformly so. A bus/GPIO cost.
	 *           pulsemax >> pulseavg    -> preemption INSIDE the burst.
	 *                                      One pulse ate the whole window.
	 *           pulseavg ~ 300 ns       -> the loop is fine and the hunt is
	 *             and hunt ~= spin         genuinely the card making us
	 *                                      clock. Then clk= is the story.
	 *
	 *         A pulse is a GPIO write, three nops and a GPIO read: at
	 *         64 MHz that is roughly 300-400 ns. The measured hunt implies
	 *         ~6000 ns, which is why this split exists.
	 *   dma=  SPIM3 hardware transfer: 514 bytes per block at 32 MHz is
	 *         ~129 us and cannot go lower on a 1-bit bus.
	 *   crc=  copy-out + CRC16 verify, pure CPU, overlapped into the DMA
	 *         window (which is why dma= reads lower than 129 us/block).
	 * Their sum plus the CMD18/CMD12 handshake is the whole of avg=. */
	{
		uint32_t phn = (uint32_t)atomic_set(&g_stem_diag_ph_reads, 0);
		uint32_t phus = (uint32_t)atomic_set(&g_stem_diag_ph_us, 0);
		uint32_t phhunt = (uint32_t)atomic_set(&g_stem_diag_ph_hunt_us, 0);
		uint32_t phspin = (uint32_t)atomic_set(&g_stem_diag_ph_spin_us, 0);
		uint32_t phdma = (uint32_t)atomic_set(&g_stem_diag_ph_dma_us, 0);
		uint32_t phcrc = (uint32_t)atomic_set(&g_stem_diag_ph_crc_us, 0);
		uint32_t phclk = (uint32_t)atomic_set(&g_stem_diag_ph_clks, 0);
		uint32_t phpk = (uint32_t)atomic_set(&g_stem_diag_ph_pulse_ns, 0);
		/* n=0 means no stem read happened in this window at all --
		 * stopped, or between songs. Print it as such rather than
		 * dividing by zero or repeating a stale average. */
		uint32_t den = phn ? phn : 1u;

		printk("STEMRD n=%u avg=%uus hunt=%uus spin=%uus dma=%uus crc=%uus clk=%u "
		       "pulseavg=%uns pulsemax=%uns\n",
		       (unsigned)phn, (unsigned)(phus / den), (unsigned)(phhunt / den),
		       (unsigned)(phspin / den), (unsigned)(phdma / den),
		       (unsigned)(phcrc / den), (unsigned)(phclk / den),
		       /* spin is us and clk is a count, so ns per pulse is
		        * spin*1000/clk. Guarded: a window with no pulses at
		        * all reports 0 rather than dividing by zero. */
		       (unsigned)(phclk ? (uint32_t)(((uint64_t)phspin * 1000ull) / phclk) : 0u),
		       (unsigned)phpk);
		diag_breathe();
	}
	/* SUSTAINED REAL-TIME FEASIBILITY, stated in the terms the acceptance
	 * test is judged in, so the physical run answers the question directly
	 * instead of handing back raw counters to do arithmetic on afterwards.
	 * Every figure here is DERIVED from counters printed above -- this line
	 * measures nothing new and costs the audio path nothing (it runs in the
	 * diagnostic printer, not in looper_audio_block()).
	 *
	 *   aus=    worst looper_audio_block() execution time this session, in
	 *           us (the same g_audio_us_max the EMMC48 line reports), and
	 *           budget= that same number as a percentage of the 5333 us a
	 *           256-frame block at 48 kHz is allowed to take. Over 100%
	 *           means the audio thread alone cannot keep up and no amount
	 *           of read-ahead will help.
	 *   need=   the stream rate four 24-bit stereo stems at 48 kHz demand:
	 *           48000 * 24 = 1152000 B/s. Not a target, an identity.
	 *   have=   the measured sustained read rate (STEMIO rate= above), and
	 *           margin= have/need as a percentage. This is bytes divided by
	 *           time spent INSIDE successful reads, so it is the read
	 *           path's own capacity, not an idle-inclusive average: below
	 *           100% the streamer cannot feed the playhead even at 100%
	 *           duty, and no read-ahead depth changes that.
	 *
	 *           WHAT A SMALL SHORTFALL SOUNDS LIKE. An earlier version of
	 *           this comment said it "shows up as a SLOW song rather than
	 *           as dropouts", because st_stream_advance_frame() freezes the
	 *           playhead on underrun instead of skipping frames. That is
	 *           true of a large sustained deficit and wrong about a small
	 *           one, and the hardware said so: at margin=93% the missing
	 *           6.7% arrived as 3762 separate freezes averaging 91 frames
	 *           (1.9 ms) each. Nobody hears 1.9 ms as tempo. It is heard as
	 *           a constant crackle, which is exactly what was reported.
	 *   ahead=  the read-ahead the ring actually holds when full, in whole
	 *           sectors and in milliseconds of audio. This is the outage it
	 *           can absorb, not evidence of throughput: a deeper ring buys
	 *           time, it does not buy bytes per second. margin= is the term
	 *           that has to be over 100%; ahead= only says how long a
	 *           transient may last before it is audible.
	 *   und=    steady-state underrun episodes must be ZERO. Any non-zero
	 *           value here invalidates the run regardless of every other
	 *           number on this line. */
	{
		uint32_t aus = g_audio_us_max;
		uint32_t auswin = g_audio_us_win;

		g_audio_us_win = 0u;
		uint32_t have = stem_diag_sustained_read_bytes_per_sec();
		/* Whole-percent integer math throughout: no floating point in
		 * this firmware's diagnostics, and none needed. BLK_FRAMES
		 * frames at ST11_SAMPLE_RATE_HZ is the block period in us. */
		uint32_t budget_us = (uint32_t)((1000000ull * (uint64_t)BLK_FRAMES) / ST11_SAMPLE_RATE_HZ);
		uint32_t need = ST11_SAMPLE_RATE_HZ * ST11_BYTES_PER_FRAME;
		uint32_t ahead_sectors = ST_STEM_MBOX_SLOTS - 1u;
		uint32_t ahead_us = (uint32_t)((1000000ull * (uint64_t)ahead_sectors *
						 (uint64_t)ST11_FRAMES_PER_SECTOR) / ST11_SAMPLE_RATE_HZ);

		printk("STEMRT aus=%uus auswin=%uus budget=%u%% need=%uBps have=%uBps margin=%u%% "
		       "ahead=%usec/%uus und=%u\n",
		       (unsigned)aus, (unsigned)auswin,
		       (unsigned)((aus * 100u) / budget_us),
		       (unsigned)need, (unsigned)have,
		       /* need is a nonzero compile-time constant (48000 * 24),
		        * so no divide-by-zero guard is possible or needed. */
		       (unsigned)(uint32_t)(((uint64_t)have * 100ull) / need),
		       (unsigned)ahead_sectors, (unsigned)ahead_us,
		       (unsigned)atomic_get(&g_stem_underrun_count));
		diag_breathe();
	}
#endif
	emmc_dbg_wr_busy_max = 0u;   /* per-window worst, reset each print */
	emmc_dbg_wr_busy_us_max = 0u;
	emmc_dbg_rd_wait_us_max = 0u;
	g_play_lowat = 0x7FFFFFFF;
	g_rec_hiwat = 0u;
}

/* ---- decode the ladders into named buttons (verified thresholds) ---- */
enum trk_btn { TRK_NONE = -1, TRK_1, TRK_2, TRK_3, TRK_4, TRK_PLAY };
/* enum vol_btn, the AIN1 band edges and their decode now live in
 * src/st_vol_ladder.h (included at the top of this file) so the host test can
 * exercise the SAME function this control loop calls. See that header for the
 * measurement and docs/ain1-measured.json for its full provenance. */

static enum trk_btn decode_tracks(int v)
{
	if (v <  110) return TRK_NONE;
	if (v <  300) return TRK_1;     /* ~213  */
	if (v <  560) return TRK_2;     /* ~403  */
	if (v <  950) return TRK_3;     /* ~733  */
	if (v < 1500) return TRK_4;     /* ~1220 */
	return TRK_PLAY;                /* ~1823 */
}



/* ================= ALWAYS-DIM LEDs (soft PWM) =========================
 * Adapted unchanged from TechnicsOP's dimmed-LED build (shared on the SP-1
 * Discord 2026-07-15, MIT) — merged into this fork as ALWAYS-ON dimming.
 * The panel LEDs are plain on/off GPIO with no current control, so "dim" =
 * software PWM: every LED write (led_service, sweeps, gauges, our two-light
 * song display) goes into a shadow mask; a tiny TIMER3 ISR renders that
 * shadow at a low duty cycle. Single writer (control thread), ISR only
 * reads. ~1 kHz frame = flicker-free. LED_PWM_ON_US is the brightness. */
#define LED_PWM_PERIOD_US 1000u    /* 1 kHz frame */
#define LED_PWM_ON_US       52u    /* ~5.2% duty — v1.2.2: a hair dimmer than
                                    * the old 60 on the track row. Floor: at
                                    * 6 us, IRQ-entry jitter of +/-3 us is a
                                    * 10-80x brightness swing = flicker; at
                                    * 36 us it is +/-8% before the eye's ~10-
                                    * frame averaging — invisible. */
#define LED_STATUS_ON_US    66u    /* the SONG/status row runs a longer window
                                    * than the track row: slightly brighter
                                    * side lights relative to the tracks. CC2
                                    * mechanism, wide-window = jitter-immune. */
/* (LED_GHOST_FRAME_DIV is gone: "faint" is now simply a level, 51/255, on
 * the same per-LED sigma-delta every other brightness uses. The old
 * one-frame-in-five divider existed only because the renderer had no
 * per-LED duty; it does now.) */
#define LED_PWM_TIMER      NRF_TIMER3
#define LED_PWM_TIMER_IRQn TIMER3_IRQn
/* every LED pin on each port (leds[]+track_leds[]) — for the OFF phase */
#define LED_ALL_P0 ((1u<<0)|(1u<<1)|(1u<<29)|(1u<<26))
#define LED_ALL_P1 ((1u<<13)|(1u<<12)|(1u<<15)|(1u<<14))
/* ---- THE PHYSICAL LED STATE: one brightness per LED ---------------------
 * Eight levels, 0..255. Index 0..3 are the Track LEDs, 4..7 the side row --
 * the SAME order st_led_mvp.h's frame uses, so led_apply_frame() is a copy
 * rather than a remapping that could silently transpose a row.
 *
 * This replaces the previous on/ghost bitmask pair plus a separate
 * track-only level array. The semantic owner needs real brightness on every
 * LED now: the beat envelope, the boot/shutdown fade and per-stem activity
 * scaling all produce intermediate values, and the side row needs them as
 * much as the track row does -- a fade on S1..S4 is impossible with an
 * on/ghost/off vocabulary. One representation for all eight is both simpler
 * and the only thing that can express it.
 *
 * The ISR gives each pin its own sigma-delta accumulator, reusing the exact
 * proven 52 us / 66 us dim windows for the frames a pin is on -- no new edge
 * timing whatsoever, the same reason the old GHOST class was built as a
 * frame divider rather than a second, narrower compare window. Sigma-delta
 * rather than a block divider because it spreads the on-frames evenly: at
 * level 128 that is a clean 500 Hz alternation, where a divider would bunch
 * 128 frames on and 128 off = 3.9 Hz of visible flicker. */
#define LED_PHYS_COUNT 8u
#define LED_PHYS_TRACK_FIRST 0u
#define LED_PHYS_SIDE_FIRST  4u
/* "Faint" as a level: 1/5 of full, matching the old GHOST class's own
 * one-frame-in-five duty exactly. */
#define LED_GHOST_LEVEL 51u
static volatile uint8_t g_led_level[LED_PHYS_COUNT];
/* Per-LED pin masks split by port, precomputed at init so the ISR needs no
 * port comparison and no branch: exactly one of the two is nonzero for any
 * given LED, so both can be OR-ed unconditionally. */
static uint32_t g_led_bit_p0[LED_PHYS_COUNT];
static uint32_t g_led_bit_p1[LED_PHYS_COUNT];
static uint32_t g_led_sta_p0, g_led_sta_p1;   /* status-row pins (init-computed) */
static uint32_t g_led_trk_p0, g_led_trk_p1;   /* track-row pins  (init-computed) */



/* DIRECT ISR (required for IRQ_ZERO_LATENCY): pure register IO, no kernel
 * calls, returns 0 = never asks for a reschedule. */
ISR_DIRECT_DECLARE(led_pwm_isr)
{
	if (LED_PWM_TIMER->EVENTS_COMPARE[1]) {         /* period wrap: render */
		LED_PWM_TIMER->EVENTS_COMPARE[1] = 0;
		(void)LED_PWM_TIMER->EVENTS_COMPARE[1];
		static uint16_t acc[LED_PHYS_COUNT];
		uint32_t s0 = 0, s1 = 0;

		/* One sigma-delta accumulator per LED: each pin is lit for the
		 * fraction of frames equal to its level. */
		for (uint32_t li = 0; li < LED_PHYS_COUNT; li++) {
			uint8_t lv = g_led_level[li];

			if (lv == 0u) {
				acc[li] = 0u;   /* fully dark: no residue to carry */
				continue;
			}
			acc[li] = (uint16_t)(acc[li] + lv);
			if (acc[li] >= 256u) {
				acc[li] = (uint16_t)(acc[li] - 256u);
				s0 |= g_led_bit_p0[li];
				s1 |= g_led_bit_p1[li];
			}
		}
		NRF_P0->OUTSET = s0;
		NRF_P0->OUTCLR = LED_ALL_P0 & ~s0;
		NRF_P1->OUTSET = s1;
		NRF_P1->OUTCLR = LED_ALL_P1 & ~s1;
	}
	if (LED_PWM_TIMER->EVENTS_COMPARE[0]) {         /* track-row on-time up */
		LED_PWM_TIMER->EVENTS_COMPARE[0] = 0;
		(void)LED_PWM_TIMER->EVENTS_COMPARE[0];
		if (g_led_dim) {   /* dim: the track row goes dark here; the
				    * status row stays lit until CC2 */
			NRF_P0->OUTCLR = g_led_trk_p0;
			NRF_P1->OUTCLR = g_led_trk_p1;
		}
	}
	if (LED_PWM_TIMER->EVENTS_COMPARE[2]) {         /* status-row on-time up */
		LED_PWM_TIMER->EVENTS_COMPARE[2] = 0;
		(void)LED_PWM_TIMER->EVENTS_COMPARE[2];
		if (g_led_dim) {
			NRF_P0->OUTCLR = g_led_sta_p0;
			NRF_P1->OUTCLR = g_led_sta_p1;
		}
	}
	return 0;
}

static void led_pwm_init(void)
{
	LED_PWM_TIMER->MODE      = TIMER_MODE_MODE_Timer;
	LED_PWM_TIMER->BITMODE   = TIMER_BITMODE_BITMODE_16Bit;
	LED_PWM_TIMER->PRESCALER = 4;                    /* 16 MHz/16 = 1 us tick */
	LED_PWM_TIMER->CC[0]     = LED_PWM_ON_US;        /* -> OFF phase */
	LED_PWM_TIMER->CC[1]     = LED_PWM_PERIOD_US;    /* -> wrap + ON phase */
	LED_PWM_TIMER->CC[2]     = LED_STATUS_ON_US;     /* -> status-row OFF */
	LED_PWM_TIMER->SHORTS    = TIMER_SHORTS_COMPARE1_CLEAR_Msk;
	LED_PWM_TIMER->INTENSET  = TIMER_INTENSET_COMPARE0_Msk |
				   TIMER_INTENSET_COMPARE1_Msk |
				   TIMER_INTENSET_COMPARE2_Msk;
	for (int li = 0; li < NUM_LEDS; li++) {
		if (leds[li].port == NRF_P0) g_led_sta_p0 |= (1u << leds[li].pin);
		else                         g_led_sta_p1 |= (1u << leds[li].pin);
	}
	for (int li = 0; li < NUM_TRACK_LEDS; li++) {
		if (track_leds[li].port == NRF_P0) g_led_trk_p0 |= (1u << track_leds[li].pin);
		else                               g_led_trk_p1 |= (1u << track_leds[li].pin);
	}
	/* Per-LED split masks for the ISR's branch-free sigma-delta (see
	 * g_led_bit_p0's own note): exactly one of the pair is nonzero for any
	 * LED, the other stays 0. Index order is track row then side row --
	 * the SAME order st_led_mvp.h's frame uses. */
	for (int li = 0; li < NUM_TRACK_LEDS; li++) {
		uint32_t bit = 1u << track_leds[li].pin;

		g_led_bit_p0[LED_PHYS_TRACK_FIRST + li] =
			(track_leds[li].port == NRF_P0) ? bit : 0u;
		g_led_bit_p1[LED_PHYS_TRACK_FIRST + li] =
			(track_leds[li].port == NRF_P0) ? 0u : bit;
	}
	for (int li = 0; li < NUM_LEDS; li++) {
		uint32_t bit = 1u << leds[li].pin;

		g_led_bit_p0[LED_PHYS_SIDE_FIRST + li] =
			(leds[li].port == NRF_P0) ? bit : 0u;
		g_led_bit_p1[LED_PHYS_SIDE_FIRST + li] =
			(leds[li].port == NRF_P0) ? 0u : bit;
	}
	IRQ_DIRECT_CONNECT(LED_PWM_TIMER_IRQn, 0, led_pwm_isr, IRQ_ZERO_LATENCY);
	irq_enable(LED_PWM_TIMER_IRQn);
	LED_PWM_TIMER->TASKS_CLEAR = 1;
	LED_PWM_TIMER->TASKS_START = 1;
}

/* ---------- LED helpers ---------- */
static void led_cfg_output(const struct led *l)
{
	l->port->PIN_CNF[l->pin] =
		(GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)   |
		(GPIO_PIN_CNF_DRIVE_S0S1    << GPIO_PIN_CNF_DRIVE_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
}
/* Legacy names, now thin wrappers onto the ONE level array -- the same
 * renderer, not a competing one. They remain for the CDC diagnostic sweep
 * and the FUNCTION-menu gauges, which are not part of normal running; the
 * normal-running display goes through led_apply_frame() alone. */
static void led_on(int i)  { g_led_level[LED_PHYS_SIDE_FIRST + i] = 255u; }
static void led_off(int i) { g_led_level[LED_PHYS_SIDE_FIRST + i] = 0u; }
static void all_off(void)  { for (int i = 0; i < NUM_LEDS; i++) led_off(i); }

/* show_song_leds() IS DELETED, not merely bypassed.
 *
 * It painted the side row as a 16-song bank/position display: the POSITION
 * LED (song % 4) solid and the BANK LED (song / 4) blinking, with the two
 * roles collapsing onto one blinking LED when they agree. That is correct
 * Tape Looper behaviour and completely wrong here -- Stem Tape's MVP holds
 * exactly ONE song, so position and bank were both zero on every boot, the
 * two roles always agreed, and the device permanently blinked a side LED to
 * announce "song 1 of 16". That blink is the physical symptom that started
 * this work.
 *
 * Deleting the function rather than removing its call site is deliberate:
 * it had exactly one caller (led_service()), so with the definition gone
 * there is no code path anywhere in this firmware that can drive the side
 * row from g_slot -- a structural guarantee rather than a promise to
 * maintain. The side row now belongs to st_led_mvp_decide() alone:
 * transport on the LED nearest PLAY, battery on the other three.
 */

static void track_led_on(int i)    { g_led_level[LED_PHYS_TRACK_FIRST + i] = 255u; }
static void track_led_off(int i)   { g_led_level[LED_PHYS_TRACK_FIRST + i] = 0u; }
/* "Faint": the contract's word for a loaded-but-silent lane. Retained for
 * the FUNCTION-menu gauges; the normal-running display no longer uses it. */
static void track_led_ghost(int i) { g_led_level[LED_PHYS_TRACK_FIRST + i] = LED_GHOST_LEVEL; }
static void track_all_off(void)  { for (int i = 0; i < NUM_TRACK_LEDS; i++) track_led_off(i); }

/* Clear BOTH LED rows. Used on power-off so nothing is left lit when SYSTEM_OFF
 * freezes the GPIO levels (the old power_off cleared only the status row, which
 * is exactly why the track/fader lights stayed on after powering down). */
static void shutdown_leds(void)
{
	all_off(); track_all_off();          /* clear the shadow */
	LED_PWM_TIMER->TASKS_STOP = 1;       /* stop the dimmer */
	NRF_P0->OUTCLR = LED_ALL_P0;         /* force every LED pin low */
	NRF_P1->OUTCLR = LED_ALL_P1;
}

/* ==========================================================================
 * THE SINGLE SEMANTIC OWNER OF ALL EIGHT LEDs
 *
 *   real runtime state  ->  one complete eight-LED semantic frame
 *                       ->  the existing TIMER3/GPIO soft-PWM renderer
 *
 * There is exactly one of these. Before this there were three overlapping
 * owners -- the inherited song-bank side display and standby track chase,
 * an ad-hoc per-stem peak meter, and (unlinked, so never actually running)
 * a separate semantic renderer -- and which one you saw depended on state
 * none of them agreed about. All of the DECISIONS now live in
 * st_led_mvp_decide(), which is pure and host-testable; everything below is
 * state-gathering and pin-pushing.
 *
 * The renderer's HARDWARE control is deliberately unchanged -- TIMER3, the
 * 52 us track / 66 us side dim windows, the same GPIO ports. The product
 * owner directed that proven hardware control be kept. What changed inside
 * it is only the duty computation: a per-LED sigma-delta over eight
 * independent 0..255 levels replaced the old whole-mask on/ghost split and
 * its 1-in-5 frame divider, because a three-name vocabulary cannot express a
 * beat envelope, a fade, or an activity-scaled brightness.
 * ========================================================================== */

/* Apply one decided frame to the physical shadow masks the TIMER3 ISR
 * renders. Every one of the eight LEDs is written on every call, so no LED
 * can retain a value from a previous frame. */
/* Apply one decided frame. A straight copy: st_led_mvp.h's frame indices and
 * g_led_level[]'s indices are deliberately the same order, so nothing here
 * can silently transpose a row. All eight are written every call, so no LED
 * can retain a value from a previous frame. */
static void led_apply_frame(const st_led_frame_t *f)
{
	for (uint32_t i = 0; i < LED_PHYS_COUNT; i++) {
		g_led_level[i] = f->level[i];
	}
}

/* The battery gauge's own sticky state. Sampled on the control thread at
 * 1 Hz -- half the rate controls_diag() already reads the same ADC channel,
 * so this adds strictly less ADC load than the firmware already carries.
 * That matters: ladder_read() blocks on the main thread, which preempts the
 * eMMC streamer, and over-sampling it is what once starved the card. */
static st_led_batt_gauge_t g_led_batt;

/* ---- one-shot sequence state -------------------------------------------
 * Boot and shutdown are driven from firmware TIME, not from sleep loops:
 * led_service() converts "ms since the sequence started" into a frame and
 * the control loop keeps running at its normal 8 ms cadence throughout.
 * Nothing about audio, USB or the streamer is blocked by an animation. */
static st_led_seq_t g_led_seq = ST_LED_SEQ_BOOT;
static uint32_t     g_led_seq_start_ms;
static bool         g_led_seq_started;

/* True once the running sequence has played out. power_off() uses this to
 * know the shutdown animation has finished before it latches SYSTEM_OFF. */
static bool led_seq_finished(uint32_t now)
{
	uint32_t total;

	if (g_led_seq == ST_LED_SEQ_NONE) {
		return true;
	}
	total = (g_led_seq == ST_LED_SEQ_BOOT) ? ST_LED_BOOT_TOTAL_MS
					       : ST_LED_SHUTDOWN_TOTAL_MS;
	return (uint32_t)(now - g_led_seq_start_ms) >= total;
}

static void led_seq_begin(st_led_seq_t seq)
{
	g_led_seq = seq;
	g_led_seq_start_ms = k_uptime_get_32();
	g_led_seq_started = true;
}


static void led_service(void)
{
	st_led_inputs_t in;
	st_led_frame_t frame;
	uint32_t now = k_uptime_get_32();
	int i;

	memset(&in, 0, sizeof(in));

	/* The boot sequence's clock starts the first time this runs, not at
	 * some earlier init, so its 0 ms really is the first frame shown. */
	if (!g_led_seq_started) {
		led_seq_begin(ST_LED_SEQ_BOOT);
	}
	if (g_led_seq != ST_LED_SEQ_NONE) {
		if (g_led_seq == ST_LED_SEQ_BOOT && led_seq_finished(now)) {
			g_led_seq = ST_LED_SEQ_NONE;   /* shutdown never clears itself */
		} else {
			in.sequence = g_led_seq;
			in.sequence_ms = (uint32_t)(now - g_led_seq_start_ms);
		}
	}

	/* ---- battery: real charger pins + real ADC, or nothing ------------ */
	{
		static bool     seeded;
		static uint32_t last_batt_ms;

		if (!seeded) {
			st_led_batt_reset(&g_led_batt);
			seeded = true;
			last_batt_ms = now - 1000u;   /* sample on the first pass */
		}
		/* Never sample mid-transfer: the streamer owns the card then and
		 * a blocking ADC read on this thread would steal from it. The
		 * gauge is sticky, so it simply holds its last reading. */
		if (!g_xfer_mode && (uint32_t)(now - last_batt_ms) >= 1000u) {
			int raw = ladder_read(&adc_ladder[LAD_BATT]);

			last_batt_ms = now;
			st_led_batt_update(&g_led_batt, raw >= 0, (int32_t)raw);
		}
	}
	in.batt_state    = st_led_batt_classify(&g_led_batt, usb_present(), charging());
	in.batt_level    = g_led_batt.level;
	in.batt_blink_on = ((now / 500u) & 1u) == 0u;    /* ~1 Hz charging blink */

	/* ---- transport / selection ---------------------------------------- */
#if SP1_XFER_ENABLE
	in.song_selected   = atomic_get(&g_stem_song_selected) != 0;
	in.transfer_active = (g_xfer_mode != 0u);
#endif
	in.playing           = in.song_selected && (g_playing != 0);
	in.transfer_blink_on = ((now / 160u) & 1u) == 0u;   /* ~3 Hz: clearly busy */

#if SP1_XFER_ENABLE
	/* ---- beat phase, envelope and bar position ------------------------
	 * ONE call, on the authoritative song position. g_stem_beat_timing
	 * comes from the selected STIX record's own bpm_q8/downbeat_frame;
	 * g_stem_song_frame_pub is the master song_frame's atomic mirror,
	 * refreshed every audio block. There is no second LED clock: phase is
	 * re-derived from whatever the mirror holds right now, so a loop wrap
	 * cannot desync anything. */
	if (in.playing) {
		uint32_t song_frame = (uint32_t)atomic_get(&g_stem_song_frame_pub);

		st_beat_pulse(&g_stem_beat_timing, song_frame, &in.beat);
	}

	/* GLOBAL LOOP marker. Read from the SAME published window the audio
	 * thread loops on, so the light cannot claim a loop the audio path is
	 * not actually running. g_stem_loop_latched is set by the control block
	 * beside st_loop_tick(). */
	if (atomic_get(&g_stem_loop_active) != 0) {
		in.loop_state = (atomic_get(&g_stem_loop_latched) != 0)
				? ST_LED_LOOP_LATCHED : ST_LED_LOOP_MOMENTARY;
	}

	/* THE FX OVERLAY, from the same st_fx_ctl output the audio path reads,
	 * so the lights cannot claim a rack the DSP is not running. Ranked above
	 * solo inside st_led_mvp_decide(): while the overlay is open the Track
	 * buttons are effects, not solos. */
	in.fx_open      = g_stem_fx_out.fx_open;
	in.fx_global    = g_stem_fx_out.scope == ST_FX_SCOPE_GLOBAL;
	in.fx_target    = g_stem_fx_out.target_stem;
	in.fx_momentary = g_stem_fx_out.momentary_mask;
	in.fx_latched   = g_stem_fx_out.latch_mask;
	/* THE FAST FLASH PHASE. 1/16 notes, square, from the SAME song frame
	 * and the SAME beat timing st_beat_pulse() above already used -- no
	 * second clock, which the LED contract forbids and which would drift
	 * against the music anyway. At 120 BPM that is 8 flashes a second.
	 *
	 * Fails closed to SOLID: with no trustworthy tempo (frames_per_beat 0,
	 * the same condition st_beat_pulse() fails on) an active effect stays
	 * lit rather than inventing a grid to flash on. */
	{
		const uint32_t fpb = g_stem_beat_timing.frames_per_beat;

		if (fpb >= 4u) {
			const uint32_t cell = fpb / 4u;   /* 1/16 note */
			const uint32_t pos  = (uint32_t)atomic_get(&g_stem_song_frame_pub);

			in.fx_flash_on = ((pos % cell) * 2u) < cell;
		} else {
			in.fx_flash_on = true;
		}
	}

	/* ---- per-stem activity, and the immediate momentary solo ----------
	 *
	 * THE FOUR METERS. One per stem, each following only its own audio --
	 * this is what makes the Track row an image of the arrangement rather
	 * than four copies of the same clock. Nothing is summed and nothing is
	 * shared: the mix is never looked at here, because a mix would give
	 * all four lights the same value and lose the whole point.
	 *
	 * READ AND CLEAR. atomic_set() returns the previous value, so this is
	 * one exchange that takes the largest peak the audio thread has seen
	 * since the last pass and arms it to accumulate the next one. See the
	 * publishing side in stem_audio_block() for why a peak HOLD rather
	 * than the last block's value.
	 *
	 * The magnitude is the raw stored-domain stem sample, and it is already
	 * zero for a stem the mixer silenced (stem_render_run() skips metering a stem
	 * whose prepared gain is zero), so a muted or solo-silenced stem
	 * decays dark through the same envelope as one that simply stopped
	 * playing. There is no separate rule for it and no state to keep in
	 * step. */
	if (in.song_selected) {
		const uint32_t now_led = k_uptime_get_32();
		const uint32_t dt_led  = s_meter_last_ms ?
					  (now_led - s_meter_last_ms) : 0u;

		s_meter_last_ms = now_led;
		for (i = 0; i < (int)ST_LED_TRACK_COUNT && i < (int)ST11_STEM_COUNT; i++) {
			const uint32_t peak =
				(uint32_t)atomic_set(&g_stem_peak_pub[i], 0);

			st_stem_meter_update(&s_stem_meters[i], peak, dt_led);
			in.stem_activity[i] =
				st_stem_meter_brightness(&s_stem_meters[i]);
			/* THE SAME BITS THE MIXER IS USING. Rebuilt from
			 * trk[].solo rather than from the chord decoder's own
			 * output, so the lights are driven by the value that
			 * actually reached the channel strip -- if the two ever
			 * disagreed, the LEDs would show what is HEARD, not what
			 * was decoded. One or several bits; a chord is not a
			 * special case. */
			if (trk[i].solo) {
				in.solo_mask |= (uint8_t)(1u << i);
			}
		}
	} else {
		/* NO SONG, NO METERS. Zeroed rather than left to decay, so
		 * selecting a song later cannot inherit a level from the one
		 * before it, and so the elapsed-time base restarts from the
		 * first pass that has audio to measure. */
		for (i = 0; i < (int)ST11_STEM_COUNT; i++) {
			st_stem_meter_reset(&s_stem_meters[i]);
		}
		s_meter_last_ms = 0u;
	}
#endif

	st_led_mvp_decide(&in, &frame);
	led_apply_frame(&frame);
}

#if SP1_XFER_ENABLE
/*
 * Turn one st_ctl_service() result into the real device: the mixer's solo
 * bits, the transport, the loop atomics and the two pinned sector regions.
 *
 * THE ONLY PLACE any of those are written for Stem Tape. Called immediately
 * after the dispatcher, so everything downstream in the same pass -- and the
 * audio thread's very next block -- sees one consistent picture.
 */
static void stem_ctl_apply(void)
{
	const st_ctl_out_t *o = &g_stem_ctl_out;
	int k;

	/* ---- THE TRACK MASK, straight through to the channel strip -------
	 * The same bits the LED path reads back out of trk[].solo, so what is
	 * lit and what is heard are one value rather than two kept in step. */
	for (k = 0; k < NTRK; k++) {
		trk[k].solo = ((o->track_mask >> k) & 1u) ? 1u : 0u;
	}

	/* ---- the loop window: bounds FIRST, `active` LAST ----------------
	 * The audio thread must never observe an active loop with a
	 * half-written window. A RESIZE moves only end_frame -- start_frame is
	 * never recomputed -- so a block that lands mid-update sees the old
	 * start with either the old or the new end, both of which are valid
	 * windows. */
	if (o->loop_enter || o->loop_resize) {
		atomic_set(&g_stem_loop_start_fr,  (atomic_val_t)o->loop_start);
		atomic_set(&g_stem_loop_end_fr,    (atomic_val_t)o->loop_end);
		atomic_set(&g_stem_loop_active, 1);
	}
	if (o->loop_enter) {
		/* NO ENTRY FRAME IS PUBLISHED. Entering does not move the
		 * transport, so there is nothing to seek to; the request
		 * survives only as the one-shot edge the audio thread consumes.
		 * Publishing the window above IS the whole of entry. */
		atomic_set(&g_stem_loop_enter_req, 1);
	}
	if (o->reverse_toggle) {
		/* FUNCTION + double-tap TRACK. The control thread does not know
		 * which way any head is going and deliberately does not try to:
		 * it publishes WHICH TRACK the player named, and the audio
		 * thread -- the sole owner of every playhead -- decides what
		 * that means. A second copy of "is track 2 reversed" living
		 * here is exactly how a surface and an engine come to
		 * disagree. */
		atomic_set(&g_stem_reverse_req, (atomic_val_t)(o->reverse_track + 1u));
	}

	/*
	 * THE SCRATCH, published as a LEVEL every pass -- not an edge.
	 *
	 * A scratch is a continuous manipulation, so the audio thread needs to
	 * know what the hand is asking RIGHT NOW, including "nothing", which is
	 * a real answer meaning the hand is resting and the head should slow.
	 * Republishing every pass also means a missed pass costs one stale
	 * block rather than a stuck gesture.
	 *
	 * Same division of labour as reverse above: this says what the player
	 * did, never what any head is doing. The audio thread owns the heads.
	 */
	{
		const uint8_t tgt = !o->scratch_active ? ST_SCR_T_NONE
				     : (o->scratch_target == ST_CTL_SCRATCH_MASTER)
					? ST_SCR_T_MASTER
					: (uint8_t)o->scratch_target;

		atomic_set(&g_stem_scratch_req,
			    ST_SCR_PACK(tgt, o->scratch_drive_q16));
	}
	if (o->loop_latch) {
		atomic_set(&g_stem_loop_latched, 1);
	}
	if (o->loop_exit) {
		/* Clearing `active` is the ENTIRE exit as far as the transport
		 * is concerned: the wrap arm stops firing and the playhead runs
		 * on through the loop end into the song. o->loop_resume is
		 * deliberately NOT consumed -- see the exit block in the audio
		 * thread for why a release may not move the playhead. */
		atomic_set(&g_stem_loop_active, 0);
		atomic_set(&g_stem_loop_latched, 0);
		atomic_set(&g_stem_loop_exit_req, 1);
	}

	/* ---- THE TWO PINNED REGIONS, requested from the ARM onwards ------
	 * Sector geometry is main.c's, not the dispatcher's: it publishes
	 * frames and this is where they become sector indices. Both are asked
	 * for on the PLAY-DOWN edge, a full hold before the loop can start, so
	 * the entry seek and any immediately-following exit both land on
	 * resident data. */
	if (o->pin_valid) {
		atomic_set(&g_stem_loop_pin_want[ST_LOOP_PIN_ENTRY],
			   (atomic_val_t)(o->pin_entry_frame / ST11_FRAMES_PER_SECTOR));
		atomic_set(&g_stem_loop_pin_want[ST_LOOP_PIN_EXIT],
			   (atomic_val_t)(o->pin_exit_frame / ST11_FRAMES_PER_SECTOR));
	} else {
		atomic_set(&g_stem_loop_pin_want[ST_LOOP_PIN_ENTRY], -1);
		atomic_set(&g_stem_loop_pin_want[ST_LOOP_PIN_EXIT], -1);
	}

	/* ---- the transport, from the ONLY PLAY owner ---------------------
	 *
	 * A BARE PLAY TAP, and nothing else, toggles the transport.
	 *
	 * Slow playback used to be claimed here, on FX + PLAY. It moved to
	 * FUNCTION + PLAY to match the companion's own spec (src/input/
	 * gestures.ts), and with it went the cost of that placement: FX is a
	 * LATCHED mode rather than a held button, so claiming PLAY inside it
	 * meant the transport could not be stopped while the overlay was open.
	 * FUNCTION is a real held modifier, so the qualified and unqualified
	 * gestures separate cleanly and this branch is back to doing one thing.
	 *
	 * The FUNCTION-qualified tap never reaches here at all: the FUNCTION
	 * branch in main()'s control loop owns the button while FUNCTION is
	 * down and continues before the ladder decode that produces play_tap.
	 */
	if (o->play_tap) {
		g_playing = !g_playing;
		if (g_playing) {
			g_midi_start_pending = 1;
		} else {
			g_midi_stop_pending = 1;
		}
	}

	/* ---- a hold that produced nothing SAYS SO ------------------------
	 * Never a silent no-op, and never a false claim that a loop started. */
	if (o->refused == ST_CTL_REFUSE_NO_TEMPO) {
		printk("STEMTAPE loop: refused -- this song has no trustworthy "
		       "tempo (frames_per_beat=0); no loop was started\n");
	} else if (o->refused == ST_CTL_REFUSE_NO_ROOM) {
		printk("STEMTAPE loop: refused -- no room left before the song "
		       "end at frame %u; no loop was started\n",
		       (unsigned)o->loop_start);
	}
}
#endif /* SP1_XFER_ENABLE */

/* FN+PLAY mode toggle (v1.2.2: fires on PLAY RELEASE, 0.7-5 s of hold —
 * holding through 5 s becomes the brightness toggle instead). M7c two-layer
 * semantics + the LED confirm, verbatim from the old in-hold body. */
static void feed_wdt(void);
static void fnp_mode_toggle(void)
{
	g_fixed_len ^= 1u;
	{
		int has = 0;
		for (int k = 0; k < NTRK; k++)
			if (trk[k].state != TS_EMPTY ||
			    (g_slot < NUM_SLOTS &&
			     g_meta.slot[g_slot].present[k]))
				has = 1;
		if (has && g_slot < NUM_SLOTS) {
			g_meta.song_mode[g_slot] = (uint8_t)
				((g_meta.song_mode[g_slot] & 0xF0u) |
				 (g_fixed_len ? 2u : 1u));
		} else {
			g_mode_pref = g_fixed_len;
			g_meta.fixed_len = g_fixed_len;
		}
	}
	g_meta_save_req = 1;
	all_off(); track_all_off();
	if (g_fixed_len) {
		for (int r = 0; r < 2; r++) {
			for (int i = 0; i < NUM_LEDS; i++) led_on(i);
			feed_wdt(); k_msleep(150);
			for (int i = 0; i < NUM_LEDS; i++) led_off(i);
			feed_wdt(); k_msleep(120);
		}
	} else {
		for (int i = 0; i < NUM_LEDS; i++) {
			led_on(i); feed_wdt(); k_msleep(110); led_off(i);
		}
		for (int i = NUM_LEDS - 2; i >= 0; i--) {
			led_on(i); feed_wdt(); k_msleep(90); led_off(i);
		}
	}
	all_off();
}

/* ---------- watchdog ---------- */
static void feed_wdt(void)
{
	for (int ch = 0; ch < 8; ch++)
		NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
}

/* ---------- power button ---------- */
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

/* arm the button to wake the chip out of SYSTEM_OFF (sense the low level) */
static void pwr_btn_arm_wake(void)
{
	PWR_PORT->PIN_CNF[PWR_PIN] =
		(GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos)|
		(GPIO_PIN_CNF_SENSE_Low     << GPIO_PIN_CNF_SENSE_Pos);
}

/* ========================================================================
 *  POWER / PERSISTENCE  —  battery charger control, the graceful
 *  stop_and_flush() (finalize any take, then flush the card's volatile write
 *  cache so loops + the slot index survive a power cut), power_off() ->
 *  SYSTEM_OFF (clean return to the bootloader; there is no reset pin), and
 *  song-slot switching. (The Tape Looper's enter_dfu() track combo is gone --
 *  see its removal note below; the UF2 bootloader's own reset-time button
 *  scan is the recovery path and needs nothing from this image.)
 * ======================================================================== */
/* ---------- battery charger ---------- */
/* Explicitly enable charging by driving the BQ24232 /CE pin low, and set the
 * two status pins as inputs with pull-ups (they are open-drain on the charger). */
static void charger_init(void)
{
	BQ_PORT->PIN_CNF[BQ_NCHG_PIN] =
		(GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
	BQ_PORT->PIN_CNF[BQ_NPGOOD_PIN] =
		(GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);

	BQ_PORT->OUTCLR = (1u << BQ_NCE_PIN);          /* drive low first  */
	BQ_PORT->PIN_CNF[BQ_NCE_PIN] =
		(GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_DRIVE_S0S1    << GPIO_PIN_CNF_DRIVE_Pos)|
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
	BQ_PORT->OUTCLR = (1u << BQ_NCE_PIN);          /* /CE low = charge enabled */
}

/* BQ24232 status (per the SP-1-dev wiki): open-drain, LOW = active */
static bool usb_present(void)
{
	return (BQ_PORT->IN & (1u << BQ_NPGOOD_PIN)) == 0u;   /* low = USB power good */
}
static bool charging(void)
{
	return (BQ_PORT->IN & (1u << BQ_NCHG_PIN)) == 0u;     /* low = charging */
}

/* ---------- graceful stop before power-off / DFU ----------
 * If a take is mid-record, end it and give the streamer a bounded window to
 * flush the rec ring and persist the song metadata, so powering off or dropping
 * into the bootloader can't lose the loop or its saved BPM/length. WDT-fed. */
static void stop_and_flush(void)
{
	g_stop_req = 1;                       /* finalize any in-progress take */
	for (int i = 0; i < 300; i++) {      /* bounded ~3 s (WDT is 4 s, fed each pass) */
		feed_wdt();
		int busy = (g_rec_track >= 0) || g_meta_save_req;
		for (int t = 0; t < NTRK; t++)
			if (trk[t].state == TS_REC || trk[t].state == TS_DONE) busy = 1;
		if (!busy) break;
		k_msleep(10);
	}
	/* Now flush the card's volatile write cache so the just-finished take + the
	 * slot index are durable across the power cut. The recording is finalized and
	 * we're shutting down, so the bus-blocking flush has nothing live to starve.
	 * The streamer (only eMMC user) does it; we wait, feeding the WDT. */
	if (g_cache_on) {
		g_cache_flush_req = 1;
		for (int i = 0; i < 1000 && g_cache_flush_req; i++) {  /* bounded ~10 s (flush itself is allowed 8 s) */
			feed_wdt();
			k_msleep(10);
		}
	}
}

/* ---------- power off ---------- */
static void power_off(void)
{
	stop_and_flush();                    /* never lose an in-progress recording */

	/* SHUTDOWN ANIMATION, through the ONE semantic owner.
	 *
	 * Side row flashes together, track row blinks once, side row fades out,
	 * everything ends dark -- and SYSTEM_OFF is not entered until it has
	 * played out, so the animation is always complete before the GPIO
	 * levels latch. st_led_mvp_decide() renders each instant from elapsed
	 * ms; this loop only advances time and feeds the watchdog.
	 *
	 * BOUNDED BY CONSTRUCTION: the loop cannot run longer than the
	 * sequence's own total (a fixed 400 ms) plus one 8 ms tick, and the
	 * watchdog is fed every pass. It ends by forcing every LED pin
	 * physically low regardless of how it exited. */
	led_seq_begin(ST_LED_SEQ_SHUTDOWN);
	{
		uint32_t guard = 0;

		while (!led_seq_finished(k_uptime_get_32()) &&
		       guard++ < (ST_LED_SHUTDOWN_TOTAL_MS / 8u) + 4u) {
			led_service();
			feed_wdt();
			k_msleep(8);
		}
	}
	shutdown_leds();

	/* Wait for the finger to come off the button first, otherwise the
	 * level-sense we are about to arm would instantly wake us again. */
	while (pwr_pressed()) {
		feed_wdt();
		k_msleep(20);
	}
	k_msleep(60);             /* debounce the release */

	shutdown_leds();          /* re-assert dark immediately before sleep */

	/* POWER DOWN THE EXTERNAL CHIPS. SYSTEM_OFF only stops the nRF — the
	 * speaker amp, the headphone codec and the eMMC I/O rail are separate
	 * chips, and the retained GPIO levels would otherwise keep them powered
	 * for days: the "battery drains overnight" reports. A powered amp whose
	 * clock has been removed can also murmur on its own — the "sound after
	 * shutdown" reports. Order: amp first, then codec, then the flash rail
	 * (its cache was flushed in stop_and_flush above). */
	tas_page(0x00);
	(void)tas_wr(0x01, 0x01);        /* TAS2505 software reset: every block
	                                  * back to its powered-down default */
	gpio_drive_low(CS42_RST_PORT, CS42_RST_PIN);   /* CS42L42 held in reset */
	emmc_power_down();               /* bus pins released, VCCQ rail off */

	gpio_drive_low(OSC_EN_PORT, OSC_EN_PIN);   /* osc off: it would otherwise
	                              keep drawing battery through SYSTEM_OFF */
	pwr_btn_arm_wake();
	feed_wdt();
	if (usb_present()) {
		/* v1.2.4: powering OFF while PLUGGED lands in the charge-standby
		 * gauge, exactly like plugging in an off device. SYSTEM_OFF with
		 * VBUS already high has no wake edge — the device just went dark
		 * until a replug (user request). A clean soft reset boots into
		 * standby instead: RESETREAS is cleared every boot, so only SREQ
		 * is set and the standby gate (!(OFF|DOG)) admits it; the external
		 * chips we just powered down stay down through standby, same as a
		 * cold plug-in. Unplugging from that standby SYSTEM_OFFs cleanly. */
		NVIC_SystemReset();
	}
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;   /* best practice before SYSTEM_OFF */
	__DSB();
	NRF_POWER->SYSTEMOFF = 1u;
	__DSB();
	for (;;) { /* CPU is now off; wakes via the bootloader on button press */ }
}

/* STEM TAPE: enter_dfu() -- the Tape Looper's in-firmware "hold Track1+Track4
 * for 1.2 s to reset into the UF2 bootloader" recovery -- is REMOVED (product
 * ruling; see the control scanner's own note where the combo was detected).
 * Track 1 and Track 4 are ordinary performance controls on this instrument.
 *
 * This removes NO recovery capability, and that is not an inference -- it is
 * the device owner's own confirmed procedure:
 *
 *   With the SP-1 OFF, hold Track 1 + Track 4 and plug in USB.
 *   ONE track light comes on: that is UF2 bootloader mode.
 *
 * That scan lives in the BOOTLOADER image and runs before this firmware is
 * entered at all, so nothing here can affect it. Note the distinct cue: the
 * bootloader lights ONE track LED, whereas the removed enter_dfu() lit all
 * FOUR -- visible proof they were always two different code paths, and that
 * the surviving one is not ours to break.
 *
 * All the removed code ever did was let the ALREADY-RUNNING firmware reset
 * itself into that same bootloader mid-performance -- exactly the behavior
 * being removed. GPREGRET is untouched by this image now.
 *
 * (firmware/README.md's "must always offer a path back to the bootloader ...
 * do not remove those" rule still holds and is still satisfied twice over:
 * the boot-time combo above, and FUNCTION held 2.5 s -> power_off() ->
 * SYSTEM_OFF, which is unchanged in this file.) */

/* Jump to song slot ns (M4b: FUNCTION+Track bank jump, and the tap-advance).
 * Saves the current song's BPM, loads the target's, signals the audio thread
 * to reload that slot's tracks. Refuses while a take is armed/recording/
 * flushing — the reload would trample the take and strand unflushed audio. */
static void jump_to_slot(uint32_t ns)
{
	if (!g_meta_loaded || g_slot_switch_req) return;    /* ignore until the last switch lands */
	if (g_rec_track >= 0) return;
	for (int i = 0; i < NTRK; i++) {
		uint8_t st = trk[i].state;
		if (st == TS_ARMED || st == TS_REC || st == TS_DONE) return;
	}
	if (ns >= NUM_SLOTS) return;
	if (g_slot >= NUM_SLOTS) g_slot = 0;
	if (ns == g_slot) return;
	g_meta.slot[g_slot].speed_q16 = g_play_speed_q16;   /* remember where you left it */
	g_meta.cur_slot = ns;
	g_slot = ns;
	g_play_speed_q16 = g_meta.slot[ns].speed_q16;        /* resume the new song's BPM */
	g_play_bpm = (int)(((uint64_t)g_play_speed_q16 * LOOP_BPM_BASE + 32768u) / 65536u);
	if (g_play_bpm < BPM_MIN) g_play_bpm = BPM_MIN;
	if (g_play_bpm > BPM_MAX) g_play_bpm = BPM_MAX;
	{	/* M7: restore the target song's persisted chop + effective mode */
		uint32_t cd = g_meta.chop[ns][0]; if (cd < 1u || cd > 64u) cd = 1u;
		uint32_t co = g_meta.chop[ns][1]; if (co >= cd) co = 0u;
		g_chop_div = cd; g_chop_off = co;
		g_fixed_len = (g_meta.song_mode[ns] & 0x0Fu)
			    ? ((g_meta.song_mode[ns] & 0x0Fu) == 2u ? 1u : 0u) : g_mode_pref;
		/* M8a: the song's grid tempo follows it; phase re-anchors
		 * provisionally to "now" (a fresh tap run re-anchors properly). */
		if (g_grid_bpm_q8[ns]) {
			g_grid_beat_frames = (uint32_t)((48000ULL * 60u * 256u) /
			                                g_grid_bpm_q8[ns]);
			g_grid_anchor = g_sample_clock;
			g_grid_next_tick = g_sample_clock;
			g_grid_active = 1;
			{ uint64_t _bar = (uint64_t)g_grid_beat_frames * 4u;
			  g_grid_next_bar = g_grid_anchor +
				(((g_sample_clock - g_grid_anchor) / _bar) + 1u) * _bar; }
		} else {
			g_grid_active = 0;
			g_grid_next_bar = 0;
		}
		g_grid_resync_at = 0;
	}
	g_slot_switch_req = 1;
	g_meta_save_req = 1;
}

/* Advance to the next song slot (FUNCTION tap). */
static void next_slot(void)
{
	if (g_slot >= NUM_SLOTS) g_slot = 0;
	jump_to_slot((g_slot + 1u) % NUM_SLOTS);
}

/* WDT PRE-WARNING (nRF52: fires ~61 us before the reset): the reported crash
 * was rr=2 = a WATCHDOG reset — something kept main (the feeder) off the CPU
 * for 4 s. Stamp WHO was running into the fault breadcrumb: 'A'udio,
 * 'S'treamer, 'M'IDI, 'm'ain (stuck in its own loop), 'I'dle (CPU idle =>
 * main is BLOCKED on something, not starved) — printed next boot as
 * flt=d09000XX@tcb. */
extern struct k_thread z_main_thread;
extern struct k_thread z_idle_threads[];
static void wdt_prewarn(const struct device *dev, int ch)
{
	ARG_UNUSED(dev); ARG_UNUSED(ch);
	k_tid_t t = k_current_get();
	uint32_t who = '?';
	if      (t == &audio_tcb)        who = 'A';
	else if (t == &streamer_tcb)     who = 'S';
	else if (t == &midi_tcb)         who = 'M';
	else if (t == &z_main_thread)    who = 'm';
	else if (t == &z_idle_threads[0]) who = 'I';
	g_fault_reason = 0xD0900000u | who;
	g_fault_pc = (uint32_t)t;
	g_fault_key = 0xFA17FA17u;
	/* RAM breadcrumbs did NOT survive a real WDT reset (the bootloader runs
	 * first and scrubs that RAM) — GPREGRET2 is a RETAINED register that
	 * survives every soft/WDT reset and the bootloader leaves it alone. */
	NRF_POWER->GPREGRET2 = (uint8_t)who;
}

int main(void)
{
	/* Why did the last boot end? (bit0 pin reset, bit1 watchdog, bit2 soft
	 * reset, bit3 CPU lockup — see nRF52840 POWER.RESETREAS.) */
	g_resetreas = NRF_POWER->RESETREAS;
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;
	if (g_fault_key == 0xFA17FA17u) {
		g_last_fault_reason = g_fault_reason;   /* previous boot CRASHED */
		g_last_fault_pc = g_fault_pc;
		g_fault_key = 0u;
	} else if (NRF_POWER->GPREGRET2 != 0u) {
		/* RAM breadcrumb lost (bootloader scrub) but the retained register
		 * survived: recover the watchdog culprit letter from it. */
		g_last_fault_reason = 0xD0900000u | NRF_POWER->GPREGRET2;
		g_last_fault_pc = 0u;
	}
	NRF_POWER->GPREGRET2 = 0u;
	/* DWT cycle counter: feeds the audio-block exec-time watermark (aus=). */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	/* Main runs at PREEMPT(1): BELOW the audio engine (0), LEVEL WITH the
	 * streamer, ABOVE MIDI (6).
	 *
	 * History, kept because both previous settings failed in a way this one
	 * has to avoid. main once defaulted to 0 and its blocking ladder-ADC
	 * reads preempting the streamer caused rec overflows; a rescue round
	 * demoted it to (8), which turned "streamer busy" into "lights, buttons
	 * and the WATCHDOG FEED all crawl" and made a 4 s busy stretch a
	 * watchdog reset -- the field-reported freeze (rr=2, lights slow). It
	 * was then raised to (1), above the streamer at (5).
	 *
	 * WHAT WENT STALE. The comment justifying (1) said "preempting the
	 * streamer is harmless NOW: the rings ride 341 ms". That is the Tape
	 * Looper's 16384-sample ring. Stem Tape's read-ahead is G-R = 4 groups
	 * = 28.3 ms, twelve times shallower, and the justification did not
	 * survive the format change. Hardware measured the consequence: reads
	 * that take 1829 us unpreempted averaged 3750 us, and the streamer held
	 * the CPU 49% of a wall clock it was busy for 98.6% of.
	 *
	 * SO: LEVEL, not inverted. The streamer no longer sits below control
	 * work, and control work no longer sits below the streamer -- which is
	 * what would re-create the (8) watchdog failure. main() keeps its own
	 * 8 ms k_msleep() cadence and now waits at most one sector read to be
	 * scheduled, because the streamer yields after every read. Neither
	 * thread can starve the other, and audio outranks both. */
	k_thread_priority_set(k_current_get(), K_PRIO_PREEMPT(ST_PRIO_MAIN));

	const struct device *wdt = DEVICE_DT_GET(WDT_NODE);

	/* Wake cause: captured ONCE at main() entry into g_resetreas (the register
	 * is write-1-to-clear and is already cleared there — a second read here
	 * returned 0 and broke this gate, parking watchdog recoveries in standby
	 * and SYSTEM_OFF-wiping the crash breadcrumb on battery). OFF = woken from
	 * SYSTEM_OFF by the power button; DOG = watchdog recovery (resume fast). */
	uint32_t wake_reas = g_resetreas;

	pwr_btn_cfg_input();
	charger_init();                 /* make sure the battery actually charges */
	for (int i = 0; i < NUM_LEDS; i++)
		led_cfg_output(&leds[i]);
	for (int i = 0; i < NUM_TRACK_LEDS; i++)
		led_cfg_output(&track_leds[i]);
	all_off();
	track_all_off();
	led_pwm_init();   /* ALWAYS-DIM: start the LED soft-PWM now, before the
	                   * charge-standby loop, so the battery gauge is dim too
	                   * (TechnicsOP's build started it later in boot). */

	if (device_is_ready(wdt)) {
		wdt_install_timeout(wdt, &(struct wdt_timeout_cfg){
			.window.max = 4000, .callback = wdt_prewarn,
		});
		wdt_setup(wdt, 0);
	}
	feed_wdt();

	/* EARLY controls_init: the battery gauge in charge-standby below needs the
	 * ladder rail + ADC channels, which used to come up only after standby.
	 * Idempotent (pure register config); the original call later is unchanged. */
	controls_init();

	/* EARLY streamer start (v1.2.3): the saved brightness lives in the song
	 * index, and only the streamer reads the eMMC — but it used to be created
	 * AFTER standby, so the charging gauge could never see the setting and
	 * always showed the dim default (user report). Started here it inits the
	 * eMMC, loads the index (g_meta_loaded -> the standby loop applies
	 * led_full), then idles; the audio_init call is guarded against a double
	 * create, and its transfer polling waits for USB (g_usb_up). */
	streamer_start();

	/* ---- CHARGE-STANDBY: the device no longer springs to life on its own ----
	 * Plugging USB in (or finishing a flash, or inserting a battery) lands here:
	 * silent, looper untouched, LED 1 blinking while charging / solid when full.
	 * HOLD the power button ~0.6 s to actually switch ON. On battery with no
	 * button held there is nothing to do -> clean SYSTEM_OFF (button wakes).
	 * A power-button wake or watchdog recovery skips straight to full boot —
	 * and even if the bootloader scrubs RESETREAS, the user waking the device
	 * is already holding the button, so the hold path turns it on anyway. */
	if (!(wake_reas & (POWER_RESETREAS_OFF_Msk | POWER_RESETREAS_DOG_Msk)) &&
	    g_last_fault_reason == 0xFFFFFFFFu) {
		/* (a valid fault breadcrumb also skips standby: the user was
		 * mid-session, and the battery standby path would SYSTEM_OFF and
		 * wipe the very forensics we just preserved) */
		int64_t hold_t = -1;
		uint32_t blink = 0;
		while (1) {
			feed_wdt();
			/* v1.2.3-r7: apply the saved brightness at the TOP of the
			 * standby loop so EVERY branch honors it — the r5 apply sat
			 * inside the gauge branch only, so the hold-to-turn-on
			 * feedback and the turn-on transition stayed dim (user
			 * report). The early streamer (r6) has the index loaded
			 * well inside the 600 ms hold. */
			if (g_meta_loaded)
				g_led_dim = g_meta.led_full ? 0u : 1u;
			if (pwr_pressed()) {
				if (hold_t < 0) hold_t = k_uptime_get();
				else if (k_uptime_get() - hold_t >= 600)
					break;                    /* -> full power-on */
				led_on(0);                        /* press feedback */
			} else {
				hold_t = -1;
				if (!usb_present())
					power_off();              /* battery, idle -> off */
				/* BATTERY GAUGE (plan §3.5): 1-4 LEDs = approximate
				 * charge level. LEDs below the level are solid; the top
				 * one blinks while charging and goes solid when the
				 * charger reports done (all four solid = full).
				 * Thresholds are RAW 12-bit readings of the AIN4
				 * battery divider (gain 1/6, 0.6 V internal ref) —
				 * PLACEHOLDERS until calibrated: note the diag line's
				 * batt= value when full and when nearly empty, then
				 * space these three between those readings. If the ADC
				 * read fails (<0), lvl stays 1 and this degrades to the
				 * original single-LED blink/solid display. */
				/* Interim calibration 2026-07-20: full anchor MEASURED
				 * at raw ~2380 (resting, plugged-not-charging = ~4.21 V);
				 * empty end is a ~3.35 V physics estimate pending a real
				 * low reading. Spread at 25/50/75% of that range. Refine
				 * batt_thr once a near-empty batt= value is logged. */
				/* v1.2.3: standby (charging) runs BEFORE the boot
				 * block that applies the saved brightness, so the
				 * gauge always showed the dim default even in full
				 * mode (user report). Apply it here as soon as the
				 * streamer has the index; idempotent per pass. */
				if (g_meta_loaded)
					g_led_dim = g_meta.led_full ? 0u : 1u;
				static const int batt_thr[3] = { 2020, 2140, 2260 };
				static int bavg = -1;   /* smoothed reading (EMA over ~10 passes) */
				static int blvl = 0;    /* sticky displayed level (hysteresis) */
				int braw = ladder_read(&adc_ladder[LAD_BATT]);
				if (braw >= 0)
					bavg = (bavg < 0) ? braw
					     : bavg + (braw - bavg) / 8;
				if (bavg >= 0) {
					/* v1.2.1 gauge fix (user report: LED 2 flickered
					 * while charging near-empty): a SINGLE raw sample
					 * per pass with no hysteresis let ADC noise +
					 * charger ripple flip the level ~25x/s at a
					 * threshold — the boundary LED strobed between
					 * off and blinking. Smooth first, then only move
					 * the level once the average clears a threshold
					 * by ±18 counts (a step is ~120 counts wide). */
					int nl = 1;
					for (int k = 0; k < 3; k++)
						if (bavg > batt_thr[k]) nl = k + 2;
					if (blvl == 0)      blvl = nl;   /* first read seeds */
					else if (nl > blvl && bavg > batt_thr[blvl - 1] + 18)
						blvl = nl;
					else if (nl < blvl && bavg < batt_thr[blvl - 2] - 18)
						blvl = nl;
				}
				int lvl = blvl ? blvl : 1;
				int bl = ((++blink / 12u) & 1u) == 0u;
				for (int i = 0; i < NUM_LEDS; i++) {
					int on;
					if (i < lvl - 1)       on = 1;
					else if (i == lvl - 1) on = charging() ? bl : 1;
					else                   on = 0;
					on ? led_on(i) : led_off(i);
				}
			}
			k_msleep(40);
		}
		all_off();
		/* wait for release so the hold doesn't bleed into the FUNCTION logic */
		while (pwr_pressed()) { feed_wdt(); k_msleep(20); }
	}

	controls_init();                /* power the button ladders + ADC + serial */
#if SP1_XFER_ENABLE
	/* COLD BOOT, once and only once. This is where the loop division is
	 * defaulted to exactly one bar; calling it again mid-session would
	 * silently discard a division the player had chosen. */
	st_ctl_reset(&g_stem_ctl);
#endif
	codec_init();                   /* release codec resets + scan the I2C bus */
	audio_init();                   /* osc on, TAS2505 configured, I2S running  */
	hp_init();                      /* headphone codec on (always-on, TimK's driver) */
	usb_audio_start();              /* device_next: CDC console only (Phase 1: no UAC2) */
	g_usb_up = 1;                   /* streamer may poll the transfer page now */
	feed_wdt();

	/* HEADPHONE AUTO-MUTE boot state: start muted if headphones are already in. */
#if HP_TIM_TEST
	if (g_hp_on == 1) {
		int votes = 0, reads = 0;
		for (int i = 0; i < 5; i++) {
			int c = hp_detect_connected();
			if (c >= 0) { reads++; votes += c; }
			k_msleep(8);
		}
		g_hp_in = (reads > 0 && votes * 2 > reads) ? 1 : 0;
		tas_set_speaker(!g_hp_in);
	}
#endif

	/* v1.2.3-r8: apply the saved brightness BEFORE the power-ON sweep.
	 * Button wakes skip standby entirely, so none of the standby-loop
	 * applies run on the battery power-on path — the sweep rendered three
	 * lines before the meta-wait and always used the dim default (user
	 * report, third location of the same boot-ordering gap). The early
	 * streamer has the index long before this point; the bounded wait is
	 * effectively zero. */
	for (int bw = 0; bw < 100 && !g_meta_loaded; bw++) { feed_wdt(); k_msleep(5); }
	if (g_meta_loaded)
		g_led_dim = g_meta.led_full ? 0u : 1u;

	/* ---- power-ON indication: sweep the LEDs on, then clear ---- */
	for (int i = 0; i < NUM_LEDS; i++) {
		led_on(i);
		feed_wdt();
		k_msleep(90);
	}
	k_msleep(160);
	all_off();

	/* wait for the streamer to load the song metadata (block 0), then select the
	 * last-used song and its saved BPM and load its tracks. */
	for (int i = 0; i < 200 && !g_meta_loaded; i++) { feed_wdt(); k_msleep(5); }
	if (g_meta_loaded) {
		g_slot = (g_meta.cur_slot < NUM_SLOTS) ? g_meta.cur_slot : 0;   /* defensive clamp */
		g_play_speed_q16 = g_meta.slot[g_slot].speed_q16;
		g_play_bpm = (int)(((uint64_t)g_play_speed_q16 * LOOP_BPM_BASE + 32768u) / 65536u);
		if (g_play_bpm < BPM_MIN) g_play_bpm = BPM_MIN;
		if (g_play_bpm > BPM_MAX) g_play_bpm = BPM_MAX;
		g_led_dim = g_meta.led_full ? 0u : 1u;   /* restore brightness mode */
		{	/* M7: current song's persisted chop + effective mode */
			uint32_t cd = g_meta.chop[g_slot][0]; if (cd < 1u || cd > 64u) cd = 1u;
			uint32_t co = g_meta.chop[g_slot][1]; if (co >= cd) co = 0u;
			g_chop_div = cd; g_chop_off = co;
			g_fixed_len = (g_meta.song_mode[g_slot] & 0x0Fu)
				    ? ((g_meta.song_mode[g_slot] & 0x0Fu) == 2u ? 1u : 0u)
				    : g_mode_pref;
			if (g_grid_bpm_q8[g_slot]) {   /* M8a: boot grid tempo */
				g_grid_beat_frames = (uint32_t)((48000ULL * 60u * 256u) /
				                                g_grid_bpm_q8[g_slot]);
				g_grid_anchor = g_sample_clock;
				g_grid_next_tick = g_sample_clock;
				g_grid_active = 1;
				{ uint64_t _bar = (uint64_t)g_grid_beat_frames * 4u;
			  g_grid_next_bar = g_grid_anchor +
				(((g_sample_clock - g_grid_anchor) / _bar) + 1u) * _bar; }
			}
		}
		g_slot_switch_req = 1;
	}

	int64_t press_start = -1;
	int64_t tap_first = 0, tap_last = 0;  /* M8a FN-tap tempo run */
	int      tap_n = 0;
	uint64_t tap_first_s = 0;             /* sample-clock at first tap */
	int      fnp_low = 0;                 /* PLAY-release debounce (passes) */
	int64_t combo_start = -1;   /* FUNCTION+PLAY: when the combo was first seen */
	uint8_t combo_fired = 0;    /* mode already toggled this combo press */
	uint8_t combo_seen  = 0;    /* PLAY was seen at all during this FUNCTION press */
	uint8_t suppress_play = 0;  /* swallow a trailing PLAY held past combo exit */
	enum trk_btn bj_cand = TRK_NONE; /* FUNCTION+Track bank jump: sticky candidate band */
	int bj_cnt = 0;                  /*   consecutive passes the candidate has held     */
	int bj_fired = -1;               /*   band already jumped during this FUNCTION press */
	enum vol_btn cp_cand = VOL_NONE; /* FUNCTION+rocker/Vol chop: sticky candidate */
	int cp_cnt = 0;                  /*   consecutive passes it has held */
	int cp_dcl_band = -1;            /*   last committed rocker band (double-click) */
	int64_t cp_dcl_t = 0;            /*   when it committed */
	uint8_t ctl_flush = 0;      /* looper decode state went stale (FUNCTION page / USB transfer owned the loop) */
	int64_t last_diag = 0;      /* throttle the control read-out */
	/* THE FX OVERLAY'S OWN TRACK LADDER. It cannot share st_ctl's, and this
	 * is a real bug fix, not duplication.
	 *
	 * The overlay used to take its held-Track mask from
	 * g_stem_ctl_out.track_mask, which st_ctl_service() derives from the
	 * ladder value main.c hands it -- and main.c ZEROES that value on any
	 * pass the overlay claims a Track. So: overlay claims T1 -> the
	 * dispatcher is fed 0 -> track_mask settles to 0 -> on the next pass the
	 * overlay reads 0 and sees the button RELEASED -> the momentary bit
	 * drops -> nothing is claimed -> the real reading returns -> the bit is
	 * set again. The effect oscillated on and off every few 8 ms passes.
	 *
	 * Physically that is not "an effect": it is the source being chopped at
	 * ~30-60 Hz, which sounds much the same whichever effect is underneath
	 * it -- exactly the reported "all of the effects sound the same". It
	 * also made the momentary LED mask flicker, so the lights could not
	 * settle either.
	 *
	 * Fixed by giving the overlay a ladder of its own, fed the TRUE raw
	 * reading every pass, before and regardless of any consumption. Same
	 * proven decoder, same measured bands; only the input is honest. The
	 * dispatcher keeps being fed the zeroed value, which is what stops a
	 * claimed Track from also soloing. */
	st_ladder_t fx_track_ladder;

	st_ladder_reset(&fx_track_ladder);

	while (1) {
		feed_wdt();

		/* USB block-transfer in progress: audio is paused and the streamer is
		 * servicing reads/writes. Ignore the controls, but keep the LEDs on
		 * the ONE owner rather than painting the track row here.
		 *
		 * This used to write track_led_on()/track_led_off() directly, which
		 * made it a second owner: it left the side row showing whatever the
		 * previous owner had put there, and on completion the display was
		 * whatever the next pass happened to compute -- the "restores an old
		 * snapshot" failure mode. led_service() now reads g_xfer_mode itself
		 * and applies the blink as an overlay on a frame computed from LIVE
		 * state, so the transfer cannot strand any LED and normal state
		 * returns on the very next pass with nothing to restore. */
		/*
		 * THE PLANAR GATE ADVANCES ON EVERY PASS, before anything that
		 * can `continue` past it and WITHOUT the diagnostic's DTR gate.
		 *
		 * controls_diag() only runs with a serial monitor attached. If
		 * the gate were advanced there, unplugging the console mid-run
		 * would freeze it mid-TEST and leave the streamer paying the
		 * divergent read cost on every sector until reboot. Advancing
		 * here means the experiment finishes, and disarms itself, with
		 * or without anyone watching; the printing stays in the diag,
		 * where it belongs.
		 *
		 * Running it inside transfer mode too is deliberate: playback is
		 * paused there, so the gate sees !playing and correctly restarts
		 * its settle rather than measuring a stopped transport.
		 */

		if (g_xfer_mode) {
			led_service();
			ctl_flush = 1;
			k_msleep(20);
			continue;
		}

		/* Print one status line ~twice a second (the 500 ms gate below) for
		 * monitoring. Only prints when a serial monitor is attached (DTR). */
		int64_t now = k_uptime_get();
#if ST_VOL_CAL
		/* THE CALIBRATION IMAGE PRINTS NOTHING BUT ITS CAPTURE. The status
		 * block is seven lines twice a second -- EMMC48, STEMIO, STEMRD,
		 * STEMRT, LOOPER, CPU, STACK -- which buries the one line this build
		 * exists to produce and makes the console unreadable in practice.
		 * That is fine in a shipped image, where the block IS the point, and
		 * useless here. Suppressed rather than throttled: a calibration image
		 * has exactly one job.
		 *
		 * feed_wdt() is not needed in its place -- the loop already feeds the
		 * watchdog at the top of every pass; the call inside the branch below
		 * exists only because the diag print itself can be slow. */
		(void)last_diag;
#else
		/*
		 * PRODUCTION-STYLE RATE WHILE PLAYING.
		 *
		 * Seven printk lines over USB CDC twice a second is a monitoring
		 * rate, not a shipping one, and playback is exactly when the CPU
		 * cannot spare it: audio 41% + streamer 49% + main 8% measured at
		 * 98% used, with the streamer about 3 points short of real time.
		 * Every one of those points spent printing is a point the reader
		 * does not get.
		 *
		 * Four times slower while the transport runs. That is still 30
		 * samples a minute -- ample for the analyzer, which differences
		 * consecutive samples and needs density, not rate -- and it cuts
		 * the CDC work per second by the same factor. Stopped, the old
		 * 500 ms cadence stays: nothing is competing for the CPU then,
		 * and that is when a person is reading the console live.
		 *
		 * The cost that remains is MEASURED rather than assumed: the DWT
		 * span below feeds diag= on the CPU line, so the next capture
		 * says how much of main() is control work and how much is the
		 * act of watching.
		 */
		g_diag_window_ms = g_playing ? 2000u : 500u;
		if (now - last_diag >= (int64_t)g_diag_window_ms) {
			uint32_t d0 = DWT->CYCCNT;

			last_diag = now;
			controls_diag();
			feed_wdt();      /* the diag print path can be slow; never starve the WDT */
			g_diag_cyc_win += DWT->CYCCNT - d0;
		}
#endif

		/* STEM TAPE: the USB feedback-format auto-negotiation watchdog
		 * (UAC2-only) is REMOVED along with UAC2 itself -- see this
		 * file's own top-of-file comment. */

		/* (track LEDs are driven by the looper beat clock below) */

		/* ================= STEM TAPE CONTROL ARBITRATION =============
		 * ONE sample of each rail per pass, ONE classifier, and -- with a
		 * Stem Tape song selected -- sole ownership of the Track buttons
		 * and of PLAY. Everything downstream (the mixer's solo bits, the
		 * loop atomics, the pinned sectors, the LEDs) consumes what this
		 * publishes; nothing re-samples or re-interprets the rails.
		 *
		 * PLACED HERE, ABOVE THE FUNCTION BRANCH, DELIBERATELY. That
		 * branch ends every path in `continue`, so anything below it can
		 * never see FUNCTION held. In st15 the loop was ticked below it
		 * and function_down was therefore a structural constant false --
		 * which is exactly why holding FUNCTION could not latch a loop on
		 * real hardware. */
		/* FUNCTION + PLAY's deferred single tap, committed here.
		 *
		 * ABOVE THE FUNCTION BRANCH FOR THE SAME REASON THE ARBITRATION
		 * IS: that branch ends every path in `continue`, so a tick below
		 * it would only ever run when FUNCTION was NOT held -- and the
		 * window it is waiting out is 450 ms, far longer than anyone
		 * keeps FUNCTION down after tapping PLAY. Placed here it runs on
		 * every pass, held or not, which is what lets a single tap
		 * commit after the player has let go of both buttons. */
		if (st_fnplay_tick(&s_stem_fnplay, (uint32_t)k_uptime_get()) ==
		    ST_FNPLAY_ACT_SLOW) {
			stem_slow_toggle();
		}

		int  st_trk_raw = ladder_read(&adc_ladder[LAD_TRACKS]);
		int  st_vol_raw = ladder_read(&adc_ladder[LAD_VOL]);
		enum vol_btn st_vraw = st_vol_decode(st_vol_raw);

		/* Tick the overlay's ladder on the TRUE reading, here, before
		 * anything can zero st_trk_raw. See its declaration for why the
		 * overlay cannot read the dispatcher's mask instead. */
		st_ladder_update(&fx_track_ladder, st_trk_raw);
#if ST_VOL_CAL
		/* AIN1 CALIBRATION CAPTURE -- st20-VOLCAL images only. Temporary,
		 * exactly like the st16-cal build that produced
		 * docs/ladder-measured.json for AIN0, and removed the same way
		 * once the number is recorded.
		 *
		 * PRINTS UNCONDITIONALLY, ON A FIXED ~250 ms CADENCE. The st19
		 * version printed only when the rail moved more than 12 counts,
		 * which collapsed four completely different failures into one
		 * indistinguishable symptom -- silence: the wrong image flashed,
		 * the console not carrying output, the buttons not reaching AIN1,
		 * and the capture itself broken all looked identical from the
		 * operator's side. A steady stream separates them. If no VOLCAL
		 * line ever appears, the image or the console is wrong. If lines
		 * appear and `raw` never moves while a button is held, that
		 * button is not reaching this rail -- which is a hardware finding,
		 * not a firmware one.
		 *
		 * NOT gated on DTR, unlike controls_diag() just above. That is
		 * deliberate: whether output survives without an asserted DTR is
		 * one of the things under test. feed_wdt() follows the print for
		 * the same reason controls_diag()'s caller does it -- a slow
		 * console write must never starve the watchdog.
		 *
		 * Every line begins with the literal "VOLCAL " so the whole
		 * capture is one grep. Fields:
		 *   raw  instantaneous ladder_read() of AIN1
		 *   set  the SETTLED value: raw held within +/-8 counts for 6
		 *        consecutive 8 ms passes (~48 ms), i.e. the plateau of a
		 *        real press rather than a sample caught on the edge
		 *   lo   lowest raw seen since boot (the resting rail)
		 *   hi   highest raw seen since boot, because parallel ladder
		 *        resistors read HIGHER than either button alone
		 *   dec  what st_vol_decode() returns (see enum vol_btn:
		 *        -1 none, 0 tempo-, 1 vol-, 2 tempo+, 3 vol+, 4 both)
		 *
		 * THE VOLUME BUTTONS ARE NOW MEASURED -- see st_vol_ladder.h and
		 * docs/ain1-measured.json -- so this build is no longer what
		 * unblocks FX entry. It is kept because two buttons on this rail,
		 * FWD and RWD, were never pressed during that capture and their
		 * bands remain inherited guesses. This is how they get measured
		 * when someone wants them proven.
		 */
		{
			static int64_t cal_next;
			static int cal_ref = -9999, cal_set = -1;
			static int cal_lo = 99999, cal_hi = -1;
			static int cal_run;

			if (st_vol_raw < cal_lo) cal_lo = st_vol_raw;
			if (st_vol_raw > cal_hi) cal_hi = st_vol_raw;

			if (st_vol_raw <= cal_ref + 8 && st_vol_raw >= cal_ref - 8) {
				if (++cal_run >= 6) cal_set = st_vol_raw;
			} else {
				cal_ref = st_vol_raw;
				cal_run = 0;
			}

			if (now >= cal_next) {
				cal_next = now + 250;
				printk("VOLCAL raw=%d set=%d lo=%d hi=%d dec=%d\n",
				       st_vol_raw, cal_set, cal_lo, cal_hi,
				       (int)st_vraw);
				feed_wdt();
			}
		}
#endif
		/* THE FX CONTROL OVERLAY, before st_ctl_service() so anything it
		 * claims cannot also reach the loop/track dispatcher. */
#if SP1_XFER_ENABLE
		{
			st_fx_in_t fi;
			bool chord = st_vol_is_chord(st_vol_raw);

			memset(&fi, 0, sizeof(fi));
			/* A ladder reports ONE state, so a real chord arrives as
			 * VOL_BOTH rather than as two independent downs. Both
			 * flags are raised together and the arrival window sees
			 * a zero-gap chord, which is the same-scan case the
			 * overlay's state machine already handles. */
			fi.vol_minus_down = chord || (st_vraw == VOL_DOWN);
			fi.vol_plus_down  = chord || (st_vraw == VOL_UP);
			fi.function_down  = pwr_pressed();
			/* From the overlay's OWN ladder, fed the true raw above.
			 * NOT g_stem_ctl_out.track_mask: that is derived from the
			 * value this block zeroes a few lines further down, which
			 * fed the release/re-press oscillation described where
			 * fx_track_ladder is declared. */
			fi.track_down     = st_ladder_mask(&fx_track_ladder);
			fi.now_ms         = (uint32_t)k_uptime_get();

			st_fx_ctl_service(&g_stem_fx_ctl, &fi, &g_stem_fx_out);

			s_fx_target       = g_stem_fx_out.target_stem;
			/* NO fx_open TERM. The overlay decides which effects
			 * are ACTIVE; the rack decides how long it is still
			 * making sound. Gating the insertion on the overlay
			 * cut a latched effect off mid-ramp -- see these two
			 * flags' own declaration for the whole defect. */
			s_fx_stem_scope   = g_stem_fx_out.scope == ST_FX_SCOPE_STEM;
			s_fx_global_scope = g_stem_fx_out.scope == ST_FX_SCOPE_GLOBAL;
			s_fx_active_mask  = g_stem_fx_out.active_mask;

			/* CONSUMPTION. A volume press the overlay claimed must
			 * not also step master volume, and a Track button it
			 * claimed must not also solo. */
			if (g_stem_fx_out.vol_minus_consumed ||
			    g_stem_fx_out.vol_plus_consumed) {
				st_vraw = VOL_NONE;
			}
			/* The claim is carried as a MASK, not by zeroing the
			 * rail. PLAY shares this rail with the four Track
			 * buttons, so st_trk_raw = 0 erased PLAY as well --
			 * which made the loop gesture invisible whenever an
			 * effect was held, and looping inside FX mode
			 * impossible. st_ctl_service() now subtracts these
			 * bits after decoding and leaves PLAY alone. */
			s_fx_track_claim = g_stem_fx_out.track_consumed;
		}
#endif
		bool stem_ctl = false;
#if SP1_XFER_ENABLE
		stem_ctl = atomic_get(&g_stem_song_selected) != 0;
		{
			st_ctl_in_t ci;

			memset(&ci, 0, sizeof(ci));

			/*
			 * ---- THE SCRATCH CONTROLS -------------------------
			 *
			 * THE ROCKER, as a direction. Raw: st_ctl settles it
			 * itself over two agreeing passes, which is shorter
			 * than the volume path's three because a scratch feels
			 * the latency in the hand -- see st_ctl_t's own note.
			 */
			ci.rocker_dir = (st_vraw == VOL_TEMPO_UP)   ?  1 :
					 (st_vraw == VOL_TEMPO_DOWN) ? -1 : 0;

			/*
			 * THE FADERS, and this is where the ~32 ms round-robin
			 * is not good enough.
			 *
			 * A volume slider updating every 32 ms is imperceptible.
			 * A hand on a record sampled at 31 Hz is not: the
			 * movement between samples is the whole signal, and at
			 * that rate most of a scratch falls between two reads.
			 *
			 * So while FUNCTION is held the cadence changes. Before
			 * a gesture has an owner, ALL FOUR are read every pass,
			 * because which fader the hand will move is exactly
			 * what is not yet known. Once one is chosen, only that
			 * one is read -- the other three go back to -1, "not
			 * sampled", and st_ctl correctly treats them as not
			 * moved rather than as sitting at zero.
			 *
			 * The cost is bounded and brief: four blocking ADC
			 * reads instead of one, only while FUNCTION is down and
			 * only until the hand commits. Ordinary playback never
			 * takes this path, and its round-robin below is
			 * untouched.
			 */
			for (uint32_t fk = 0; fk < ST_PL_STEMS; fk++) {
				ci.fader_raw[fk] = -1;
			}
			if (pwr_pressed() && stem_ctl) {
				const uint8_t tgt = g_stem_ctl_out.scratch_active
						     ? g_stem_ctl_out.scratch_target
						     : ST_CTL_SCRATCH_NONE;

				if (tgt < ST_PL_STEMS) {
					ci.fader_raw[tgt] =
						ladder_read(&adc_ladder[LAD_FADER0 + tgt]);
				} else if (tgt == ST_CTL_SCRATCH_NONE) {
					for (uint32_t fk = 0; fk < ST_PL_STEMS; fk++) {
						ci.fader_raw[fk] = ladder_read(
							&adc_ladder[LAD_FADER0 + fk]);
					}
				}
				/* tgt == MASTER: the rocker owns the gesture and
				 * no fader is read at all. */
			}
			ci.ladder_raw     = st_trk_raw;
			ci.track_consumed_mask = s_fx_track_claim;
			/* Only the master-volume pair maps to a loop division. The
			 * varispeed rocker (VOL_TEMPO_UP/DOWN) is a different
			 * control and must never resize a loop. */
			ci.vol_dir        = (st_vraw == VOL_DOWN) ? -1
					     : (st_vraw == VOL_UP) ? 1 : 0;
			ci.function_down  = pwr_pressed();
			ci.stem_song      = stem_ctl;
			ci.playing        = (g_playing != 0);
			/* THE AUDIO THREAD'S OWN playhead, via the atomic mirror
			 * it refreshes every block -- not a control-thread
			 * estimate. This is the frame a PLAY-down edge captures. */
			ci.song_frame     = (uint32_t)atomic_get(&g_stem_song_frame_pub);
			ci.song_frames    = ST_STEM_GEOM.frames;
			ci.frames_per_beat = g_stem_beat_timing.frames_per_beat;
			ci.now_ms         = (uint32_t)k_uptime_get();

			st_ctl_service(&g_stem_ctl, &ci, &g_stem_ctl_out);
		}
		stem_ctl_apply();
#endif

		/* FUNCTION button: a SHORT tap changes song; a long HOLD powers off
		 * (the same button does both, like the original device).
		 *
		 * A FUNCTION press the loop has CONSUMED (it latched a loop, or it
		 * is modifying a division) never reaches this branch: it must not
		 * also change song, start a power-off countdown, step brightness
		 * or move a bank. */
		if (pwr_pressed() && !g_stem_ctl_out.function_consumed) {
			ctl_flush = 1;
			if (press_start < 0)
				press_start = k_uptime_get();

			/* MODE TOGGLE — FUNCTION + PLAY held together ~0.7 s flips the
			 * fixed/variable loop-length mode. The normal ladder decode below
			 * is skipped while FUNCTION is held, so read PLAY here. PLAY is at
			 * the TOP of the AIN0 ladder (~1823); require >1600 so a Track-4
			 * (~1220) or the ambiguous 1+4 band (~1325) can never be mistaken
			 * for it. FUNCTION is a separate GPIO, so holding it does not shift
			 * the ladder voltage. While the combo is engaged the power-off
			 * countdown/shutdown is suppressed (this gesture must never risk a
			 * power-off), and the FUNCTION-release song-change is suppressed. */
			/* The pass already sampled this rail. Reuse it rather than
			 * blocking on a second conversion -- ladder_read() preempts
			 * the eMMC streamer, and the value cannot have changed
			 * meaningfully within one 8 ms pass. */
			int fraw = st_trk_raw;
			if (fraw > 1600) {
				fnp_low = 0;
				combo_seen = 1;
				if (combo_start < 0) {           /* fresh PLAY press edge */
					int64_t fnp_now = k_uptime_get();

					/* THE TAP GESTURE, x1 slow / x2 snap home,
					 * arbitrated by st_fnplay (host-tested in
					 * tests/test_fnplay.c). The 450 ms double-tap
					 * window that used to be spelled out here is
					 * now the module's own ST_FNPLAY_DOUBLE_MS --
					 * the same figure, moved rather than changed,
					 * so the gesture feels identical.
					 *
					 * x2 still fires IMMEDIATELY on the second
					 * press edge, exactly as before, and still
					 * blocks the hold tiers for this press so one
					 * gesture cannot do two things. x1 is the new
					 * half: it is DEFERRED to the tick above,
					 * because firing it optimistically would dive
					 * the song an octave before snapping home. */
					if (st_fnplay_press(&s_stem_fnplay,
							     (uint32_t)fnp_now) ==
					    ST_FNPLAY_ACT_SNAP && !combo_fired) {
						/* M8c: SNAP HOME — instant return to
						 * native speed/pitch after beatmatching
						 * or rocker wandering ("tap to match,
						 * double-tap to come home"). Now clears
						 * the STEM transport's own semitones and
						 * slow mode, which is what the player
						 * actually hears; before it only wrote
						 * the classic speed, which the stem
						 * transport does not read. */
						stem_snap_home();
						combo_fired = 1;
					}
					combo_start = fnp_now;
				}
				if (!combo_fired &&
				    k_uptime_get() - combo_start >= 5000) {
					/* v1.2.2: HOLD THROUGH 5 s = BRIGHTNESS toggle,
					 * firing WITHOUT release — the light change is
					 * the confirm and the press is spent. The mode
					 * toggle moved to PLAY-RELEASE (0.7-5 s), so a
					 * hold that reaches 5 s never flips the mode. */
					g_led_dim ^= 1u;
					g_meta.led_full = g_led_dim ? 0u : 1u;
					g_meta_save_req = 1;
					combo_fired = 1;
					/* This press is spent on the light. Its
					 * release must not also be read as a tap
					 * -- st_fnplay_release() would drop it on
					 * duration anyway, but saying so here
					 * means the brightness tier does not
					 * depend on that ceiling to be safe. */
					st_fnplay_cancel(&s_stem_fnplay);
				}
				k_msleep(25);
				continue;                /* combo owns the button */
			}
			if (combo_start >= 0) {
				/* v1.2.2-r4: DEBOUNCED release — the shared ladder can
				 * dip below the PLAY band for a stray pass mid-hold,
				 * which used to reset the 5 s clock (user: "brightness
				 * takes ~7 s"). Only 3 consecutive low passes count as
				 * a real release. */
				if (++fnp_low < 3) { k_msleep(25); continue; }
				/* THE TAP'S RELEASE EDGE. st_fnplay decides for
				 * itself whether this was short enough to be a
				 * tap; a longer press belongs to the mode or
				 * brightness tier below and the module drops it,
				 * so one press can never arm the slow toggle AND
				 * flip the mode. */
				st_fnplay_release(&s_stem_fnplay,
						   (uint32_t)k_uptime_get());
				if (!combo_fired) {
					int64_t fnp_held = k_uptime_get() - combo_start;
					if (fnp_held >= 350 && fnp_held < 5000)
						fnp_mode_toggle();  /* mode fires on RELEASE */
				}
			}
			fnp_low = 0;
			combo_start = -1;            /* PLAY not held */

			/* BANK JUMP — FUNCTION + Track N -> first song of bank N (M4b).
			 * POWER-OFF SAFETY (the whole point): committing a track band
			 * during a FUNCTION hold sets combo_seen — the same flag the
			 * FUNCTION+PLAY combo uses — which suppresses the power-off
			 * countdown, the shutdown itself, and the release song-advance
			 * for the remainder of this press. Turning the device off now
			 * requires a CLEAN FUNCTION-only hold, exactly as before.
			 * Sticky commit: the same band must be seen on 3 consecutive
			 * passes (~75 ms) — a finger transiting the ladder can't fire.
			 * Keeping FUNCTION held and pressing another track jumps again
			 * (bank surfing). While recording, jump_to_slot() refuses, as
			 * the tap-advance always has. Note: this FUNCTION-held path
			 * gates on `fraw < 1500`, so the ambiguous T1+T4 band (~1325)
			 * still decodes as Track 4 -> bank 4 here. That is unchanged
			 * and harmless (a bank jump, not a reset), but it is the same
			 * ladder-aliasing class the DFU removal addressed, and it goes
			 * away for good when st_gesture.c takes over control decoding
			 * with a real multi-press model. */
			{
				enum trk_btn tb = (fraw >= 110 && fraw < 1500)
						  ? decode_tracks(fraw) : TRK_NONE;
				if (tb >= TRK_1 && tb <= TRK_4) {
					if (tb == bj_cand) bj_cnt++;
					else { bj_cand = tb; bj_cnt = 1; }
					if (bj_cnt == 3) {   /* exact edge: once per press */
						combo_seen = 1;      /* never a power-off now */
						bj_fired = (int)tb;
						uint32_t bank = (uint32_t)tb * 4u;
						/* M8a: pressing the SAME track again steps
						 * through that bank's four songs; another
						 * track jumps to its bank. All 16 songs
						 * under one FUNCTION hold (FN-tap is tap
						 * tempo now, not next-song). */
						if (g_slot / 4u == (uint32_t)tb)
							jump_to_slot(bank + ((g_slot % 4u) + 1u) % 4u);
						else
							jump_to_slot(bank);
					}
					midi_service();
					led_service();           /* live song display mid-hold */
					k_msleep(25);
					continue;                /* track held: combo owns the button */
				}
				bj_cand = TRK_NONE; bj_cnt = 0;
			}

			/* LOOP CHOP (scheme A', collision-audited): while FUNCTION is
			 * held the Vol/rocker ladder — which stock never reads during
			 * FUNCTION holds — becomes the chop surface:
			 *   FWD  = window /2 (shorter)   RWD  = window x2 (longer)
			 *   Vol+ = shift window right    Vol- = shift window left
			 *   rocker DOUBLE-CLICK = reset to the full loop
			 * Sticky 3-pass commit (transit-proof); every commit sets
			 * combo_seen so the press can never become a power-off; bare
			 * rocker/Vol behavior outside FUNCTION holds is untouched. */
			{
				enum vol_btn vb = st_vraw;   /* the pass's ONE reading of this rail */
				/* VOL_BOTH IS NOT A CHOP GESTURE. It is the FX overlay's
				 * entry chord -- and under a FUNCTION hold, specifically
				 * the GLOBAL-scope one -- which the overlay above claims
				 * and normally consumes into VOL_NONE before this runs.
				 * The guard is here for the passes where it does not:
				 * the dispatch below ends in a bare `else` commented
				 * "VOL_DOWN", so an unconsumed VOL_BOTH would silently
				 * shift the chop window LEFT. It became reachable the
				 * moment the chord got a real ladder value; before that
				 * the chord decoded as VOL_UP and this branch could not
				 * see it. Hold the button so nothing else in this
				 * FUNCTION branch reinterprets it, and change nothing. */
				if (vb == VOL_BOTH) {
					cp_cand = VOL_NONE; cp_cnt = 0;
					midi_service();
					led_service();
					k_msleep(25);
					continue;
				}
				/*
				 * THE SCRATCH OWNS FUNCTION + ROCKER WHENEVER A
				 * STEM SONG IS SELECTED.
				 *
				 * This branch is the inherited Tape Looper's
				 * CHOP divider, and it is classic-only: the stem
				 * path clears g_chop_req every block, and with no
				 * classic content (the source-absence gate proves
				 * that fail-closed) it is already a no-op here.
				 * So nothing audible is lost -- but leaving it
				 * running would still let a shuttle rewrite
				 * g_chop_div and persist it to g_meta, which is
				 * state churn on a gesture that means something
				 * else entirely.
				 *
				 * Same ownership rule st_ctl.h states for the
				 * Track buttons and PLAY: with a stem song, the
				 * stem instrument owns the surface; without one,
				 * the Looper is untouched.
				 */
				if (vb != VOL_NONE && !g_stem_ctl_out.rocker_consumed) {
					if (vb == cp_cand) { if (cp_cnt < 1000) cp_cnt++; }
					else { cp_cand = vb; cp_cnt = 1; }
					if (cp_cnt == 3) {          /* committed press edge */
						int64_t cnow = k_uptime_get();
						combo_seen = 1;
						uint32_t d = g_chop_div, o = g_chop_off;
						if (vb == VOL_TEMPO_UP || vb == VOL_TEMPO_DOWN) {
							if (cp_dcl_band == (int)vb &&
							    cnow - cp_dcl_t <= 400) {
								d = 1u; o = 0u;   /* double-click: RESET */
							} else if (vb == VOL_TEMPO_UP) {
								if (d < 64u) { d <<= 1; o <<= 1; }
							} else {
								if (d > 1u) { d >>= 1; o >>= 1; }
							}
							cp_dcl_band = (int)vb; cp_dcl_t = cnow;
						} else if (vb == VOL_UP) {
							o = (o + 1u) % d;
						} else {                  /* VOL_DOWN */
							o = (o + d - 1u) % d;
						}
						g_chop_off = (d > 1u) ? (o % d) : 0u;
						g_chop_div = d;
						if (g_slot < NUM_SLOTS) { /* M7a: persist per song */
							g_meta.chop[g_slot][0] = (uint8_t)d;
							g_meta.chop[g_slot][1] = (uint8_t)g_chop_off;
							g_meta_save_req = 1;
						}
						g_chop_req = 1;           /* engine: snap to it */
					}
					midi_service();
					led_service();
					k_msleep(25);
					continue;                 /* chord owns the button */
				}
				cp_cand = VOL_NONE; cp_cnt = 0;
			}
			if (combo_seen) {
				/* The combo has been engaged this FUNCTION press: once PLAY
				 * is lifted, do NOTHING further for the rest of the hold —
				 * no power-off countdown, no shutdown (press_start still
				 * dates from the original FUNCTION-down, so the 2.5 s
				 * power-off would otherwise fire). The FUNCTION press is
				 * spent; it ends cleanly on release below. */
				midi_service();
				led_service();
				k_msleep(25);
				continue;
			}

			int64_t held = k_uptime_get() - press_start;

			/* M8a: a HOLD right after a tap run = CLEAR this song's grid.
			 * The run also spends the press — never a power-off. */
			if (tap_n > 0 && k_uptime_get() - tap_last < 1500) {
				if (held >= 1000 && !combo_seen) {
					g_grid_bpm_q8[g_slot] = 0;
					g_grid_active = 0;
					g_grid_save_req = 1;
					tap_n = 0;
					combo_seen = 1;      /* spend the press */
				}
				midi_service();
				led_service();
				k_msleep(25);
				continue;
			}

			if (held >= HOLD_MS_TO_OFF)
				power_off();             /* never returns */

			/* show the power-off countdown only once it's clearly a hold, so a
			 * quick tap (song change) doesn't flash it. Clear BOTH rows so the
			 * countdown fills cleanly against a dark track row. */
			if (held > 400) {
				int lit = (int)((held * NUM_LEDS) / HOLD_MS_TO_OFF) + 1;
				if (lit > NUM_LEDS) lit = NUM_LEDS;
				all_off();
				track_all_off();
				for (int i = 0; i < lit; i++) led_on(i);
			}
			k_msleep(25);
			continue;
		}

		if (press_start >= 0) {                  /* just released */
			/* v1.2.2-r4: releasing FUNCTION first (or both together —
			 * the natural way to end the chord) must ALSO fire the
			 * release-toggle; before, only a PLAY-first release did,
			 * so the gesture silently aborted most of the time (user:
			 * "mode takes ~4 s" = retries until a lucky stagger). */
			if (combo_start >= 0) {
				/* FUNCTION let go first, which ends the gesture
				 * just as surely as releasing PLAY. The tap edge
				 * has to be reported on THIS path too, or a
				 * player who lifts FUNCTION first would never get
				 * their slow toggle. */
				st_fnplay_release(&s_stem_fnplay,
						   (uint32_t)k_uptime_get());
			}
			if (combo_start >= 0 && !combo_fired) {
				int64_t fnp_held2 = k_uptime_get() - combo_start;
				if (fnp_held2 >= 350 && fnp_held2 < 5000)
					fnp_mode_toggle();
			}
			if (!combo_seen &&
			    (k_uptime_get() - press_start) < 600) {
				/* M8a: FN-tap = TAP TEMPO (navigation moved into the
				 * FN hold). 1-3 taps: nothing. 4+ taps in steady
				 * rhythm: commit the grid — tempo from mean spacing,
				 * downbeat = the first tap. Every further tap refines. */
				int64_t tnow = k_uptime_get();
				uint64_t snow = g_sample_clock;
				if (tap_n > 0 && (tnow - tap_last > 1500 ||
				                  tnow - tap_last < 200)) tap_n = 0;
				if (tap_n > 1) {
					int64_t mean = (tap_last - tap_first) / (tap_n - 1);
					int64_t dvi = (tnow - tap_last) - mean;
					if (dvi < 0) dvi = -dvi;
					if (dvi * 5 > mean) tap_n = 0;  /* >20% off: new run */
				}
				if (tap_n == 0) { tap_first = tnow; tap_first_s = snow; }
				tap_last = tnow; tap_n++;
				if (tap_n >= 4) {
					int64_t mean = (tap_last - tap_first) / (tap_n - 1);
					if (mean >= 300 && mean <= 1200) {  /* 50..200 BPM */
						uint32_t bpmq8 =
							(uint32_t)((60000LL << 8) / mean);
						/* M8c BEATMATCH: if this song already has
						 * loops, the tap run means "match THIS" —
						 * capture their native tempo first. */
						uint32_t native_q8 = 0;
						if (g_loop_len > 0u) {
							if (g_grid_bpm_q8[g_slot])
								native_q8 = g_grid_bpm_q8[g_slot];
							else if (g_beat_samples)
								native_q8 = (uint32_t)
									((48000ULL * 60u * 256u) /
									 g_beat_samples);
						}
						g_grid_bpm_q8[g_slot] = (uint16_t)bpmq8;
						g_grid_beat_frames = (uint32_t)
							((48000ULL * 60u * 256u) / bpmq8);
						g_grid_anchor = tap_first_s;
						g_grid_next_tick = g_sample_clock;
						g_grid_active = 1;
						g_grid_save_req = 1;
						{ uint64_t _bar = (uint64_t)g_grid_beat_frames * 4u;
			  g_grid_next_bar = g_grid_anchor +
				(((g_sample_clock - g_grid_anchor) / _bar) + 1u) * _bar; }
						if (native_q8) {
							/* retune the tape so the loops play at
							 * the tapped tempo (vinyl rules: pitch
							 * moves too), clamped to the physical
							 * 0.5-1.5x range, and restart the loops
							 * on the tapped downbeat at the next
							 * bar line — tempo AND phase matched. */
							uint64_t sp = ((uint64_t)bpmq8 << 16) /
								      native_q8;
							if (sp < 32768u) sp = 32768u;
							else if (sp > 98304u) sp = 98304u;
							g_play_speed_q16 = (uint32_t)sp;
							g_play_bpm = (int)((sp * 80u + 32768u) >> 16);
							if (g_play_bpm < BPM_MIN) g_play_bpm = BPM_MIN;
							if (g_play_bpm > BPM_MAX) g_play_bpm = BPM_MAX;
							g_grid_resync_at = g_grid_next_bar;
						}
					}
				}
			}
			all_off();
			/* If the combo was ended by lifting FUNCTION FIRST while PLAY is
			 * still down, swallow that trailing PLAY until it is released, so
			 * it can't leak into the normal decode as a restart / play-stop. */
			/* A FRESH conversion, deliberately, and the one place in the
			 * file that reads this rail twice in a pass: the combo above
			 * can have held for several seconds, so the sample taken at
			 * the top of the pass says nothing about whether PLAY is
			 * still down NOW. It runs only on the pass a FUNCTION combo
			 * ends, never per-pass. */
			if (combo_seen &&
			    ladder_read(&adc_ladder[LAD_TRACKS]) >= 110) suppress_play = 1;
		}
		press_start = -1;
		combo_start = -1;
		combo_fired = 0;
		combo_seen  = 0;
		bj_cand = TRK_NONE; bj_cnt = 0; bj_fired = -1;
		cp_cand = VOL_NONE; cp_cnt = 0; cp_dcl_band = -1;

		/* ---- looper controls + LEDs ---- */
		{
			/* STEM TAPE: the inherited Tape Looper's Track1+Track4 DFU combo
			 * is REMOVED (product ruling). Track 1 and Track 4 are ordinary
			 * performance controls here -- Stem Tape's own control matrix
			 * gives them mute (tap), momentary solo (hold), lane loop
			 * (double-tap) and reverse (FN + double-tap) -- so a gesture that
			 * silently reset the device out of the running firmware was never
			 * compatible with playing the instrument. Nothing about firmware
			 * recovery depends on this block: the UF2 bootloader runs its OWN
			 * button scan at reset, entirely outside this image, so holding
			 * 1+4 through a power-on still reaches DFU exactly as before.
			 *
			 * The BAND ITSELF still has to be recognised and rejected. PLAY
			 * and TRACK1-4 share one resistor ladder (see decode_tracks()),
			 * and decode_tracks() maps every voltage onto some button -- it
			 * has no notion of "two pressed". Pressing 1+4 lands at ~1325,
			 * which would otherwise decode as a Track-4 press (< 1500) that
			 * the user never made. Reading it as TRK_NONE is the truthful
			 * answer for an ambiguous ladder voltage: 1+4 now does nothing
			 * at all, rather than doing something else. */
			/* THE SINGLE LADDER SAMPLE for this pass, taken at the top
			 * of the control loop and shared by everything that needs it.
			 * There is no second ladder_read() of AIN0 anywhere in a
			 * pass: ladder_read() blocks this thread, which preempts the
			 * eMMC streamer, and a duplicate read is real risk to
			 * playback for no information. */
			int trk_raw = st_trk_raw;
			enum trk_btn raw;

			if (stem_ctl) {
				/* STEM TAPE: st_ctl_service() is the SOLE owner of this
				 * rail. It has already classified this very sample into
				 * a Track mask and a PLAY state and published both. The
				 * inherited single-button decode is therefore given
				 * nothing at all -- not a track, not PLAY -- so no
				 * classic gesture can fire underneath the dispatcher and
				 * the two can never disagree about what is held. */
				raw = TRK_NONE;
			} else if (trk_raw >= 1280 && trk_raw <= 1390) {
				/* Inherited-engine path only: the ambiguous Track1+Track4
				 * DFU band decodes as nothing rather than as a Track-4
				 * press the player never made. */
				raw = TRK_NONE;
			} else {
				raw = decode_tracks(trk_raw);
			}
			/* trailing-PLAY guard (see the FUNCTION+PLAY combo exit): ignore
			 * the ladder until the RAW reading goes fully idle once, so a PLAY
			 * still held after the mode toggle — and its whole release sweep
			 * down through the track bands — never reaches the decode. Idle
			 * means the reading itself: 1280-1390 decodes as NONE but is NOT
			 * idle, and clearing there would expose the rest of the sweep. */
			if (suppress_play) {
				if (trk_raw >= 0 && trk_raw < 110) suppress_play = 0;
				else raw = TRK_NONE;
			}

			/* STICKY DEBOUNCE -> `committed` (the stable, settled button). Recording
			 * stops on RELEASE, so a single noisy ADC sample (audio/USB activity
			 * couples into the button ladder while a loop streams) must NOT look like
			 * a release: the committed button only changes after a DIFFERENT value is
			 * seen on 3 consecutive reads (~24 ms); a lone glitch back to the held
			 * value resets the counter, so a steady hold can never false-trigger. */
			static enum trk_btn committed = TRK_NONE, cand = TRK_NONE;
			static int cand_cnt;
			static int64_t press_t[NTRK];        /* when committed first named this track */
			/* STEM TAPE Phase 3 control-matrix (momentary hold-to-
			 * solo, corrected): one independent st_track_hold_t
			 * per track -- see st_track_hold.h's own doc comment.
			 * Ticked every pass for every track, below, from the
			 * SAME press_t[]/committed this block already tracks
			 * for its own, unrelated purposes -- no new debounce,
			 * no new time source. */
			static st_track_hold_t track_hold[NTRK];
			static int64_t tap_deadline[NTRK];   /* >0: a single tap awaiting a possible 2nd */
			static uint8_t armed_press[NTRK];    /* this press already armed a take */
			static int stop_tap_trk = -1;        /* R1: stop already fired at press;
			                                      * swallow that press's release */
			static int64_t ep_time[TRK_PLAY + 1];/* committed ms per button, this episode */
			static int64_t ep_since;             /* when `committed` last changed */
			static uint8_t ep_open;              /* a press episode is in progress */
			static uint8_t ep_play_held;         /* this episode's PLAY press became a hold */
			static int64_t play_t = -1;          /* when PLAY was committed (hold timing) */
			static int     play_held;            /* this PLAY press already fired the restart */
			/* FUNCTION (or a USB transfer) owned the loop since the last pass
			 * here, so every static above is stale: a PLAY committed just
			 * before the combo froze this block would otherwise look like a
			 * long hold (phantom restart) and its open episode would fire a
			 * phantom play/stop on release. Reset everything and swallow the
			 * ladder until it reads idle. */
			if (ctl_flush) {
				ctl_flush = 0;
				committed = TRK_NONE; cand = TRK_NONE; cand_cnt = 0;
				for (int k = TRK_1; k <= TRK_PLAY; k++) ep_time[k] = 0;
				ep_open = 0; ep_play_held = 0;
				play_t = -1; play_held = 0;
				for (int k = 0; k < NTRK; k++) { tap_deadline[k] = 0; armed_press[k] = 0; }
				stop_tap_trk = -1;
				if (!(trk_raw >= 0 && trk_raw < 110)) suppress_play = 1;
				raw = TRK_NONE;
			}
			enum trk_btn before = committed;
			if (raw == committed) {
				cand_cnt = 0;
			} else if (raw == cand) {
				if (++cand_cnt >= 3) { committed = raw; cand_cnt = 0; }
			} else {
				cand = raw; cand_cnt = 1;
			}

			/* TRACK buttons:
			 *   HOLD (button physically down >= HOLD_RECORD_MS) -> RECORD (auto-start
			 *      then captures from the first sound). A quick tap never lasts this long.
			 *   TAP (released before that) -> MUTE / unmute.
			 *   DOUBLE-TAP (a 2nd tap within DTAP_GAP_MS of the 1st tap's release) -> DELETE.
			 * Tap-vs-hold is decided by the PHYSICAL down-time and double-tap by the
			 * rhythm of two quick taps, so taps/double-taps stay reliable regardless of
			 * how fast recording arms (a quick ~HOLD_RECORD_MS hold instead of 300ms).
			 *
			 * PRESS EPISODE tracker: one episode = the ladder leaving idle
			 * until it settles back at idle. A finger pressing or releasing a
			 * HIGHER ladder button sweeps the voltage THROUGH the lower
			 * buttons' bands, and the debounce can commit one of them for a
			 * beat (~24-32 ms) on the way — the old code treated every
			 * committed change as a real release edge and fired PHANTOM taps
			 * ("recording track 4 muted track 1"). Now committed-time is
			 * accumulated per button and the release action fires ONCE, at
			 * episode end, for the DOMINANT (longest-committed) button.
			 * Three rules keep the phantom window closed:
			 *   - a press edge wipes the accumulated time of every band BELOW
			 *     it (provably the up-sweep in transit, not a press);
			 *   - the episode only ends when the RAW reading is idle, so a
			 *     slow release dwelling in the 1280-1390 no-man's band (which
			 *     decodes as NONE) can't split one gesture into two;
			 *   - a dominant under 40 ms fires nothing (a real tap commits
			 *     ~40 ms+, a transit blip caps at ~32 ms per traversal).
			 * Hold actions (arm, restart) are duration-based and transit-immune. */
			if (committed != before) {
				int64_t tnow = k_uptime_get();
				if (before != TRK_NONE)
					ep_time[(int)before] += tnow - ep_since;
				ep_since = tnow;
				if (committed != TRK_NONE) {
					ep_open = 1;
					for (int k = TRK_1; k < (int)committed; k++)
						ep_time[k] = 0;  /* below = up-sweep transit */
				}
				if (committed >= TRK_1 && committed <= TRK_4) { /* PRESS edge */
					int ti = (int)committed;
					press_t[ti] = tnow;
					armed_press[ti] = 0;
				}
			}
			if (ep_open && committed == TRK_NONE &&
			    trk_raw >= 0 && trk_raw < 110) {
				/* EPISODE END (ladder settled at idle): attribute the
				 * release to the button that was committed the longest. */
				ep_open = 0;
				int64_t tnow = k_uptime_get();
				int b = -1; int64_t bt = 0;
				for (int k = TRK_1; k <= TRK_PLAY; k++) {
					if (ep_time[k] > bt) { bt = ep_time[k]; b = k; }
				}
				/* Order matters: first decide the episode is REAL (its
				 * dominant out-lasts any possible transit blip), THEN decide
				 * which button owns it. */
				if (bt < 40) {
					b = -1;          /* pure transit blip: fire nothing */
				} else {
					/* ROLL-OFF ATTRIBUTION: a release sweep only ever dwells
					 * on bands BELOW the button that was really pressed (the
					 * ladder cannot overshoot above it, and up-sweep transit
					 * is wiped at the press edge). So when a lower band
					 * out-dwelt the HIGHEST committed button, prefer the
					 * highest — provided it was committed >=24 ms (a real
					 * contact, longer than debounce noise) and at least half
					 * the dominant's time. This keeps a quick stop-tap on the
					 * recording track from becoming a phantom mute with the
					 * take left running, and equally protects the taps right
					 * AFTER a take finalizes and lazy PLAY releases — the old
					 * rule only guarded the recording track, so the "did it
					 * stop?" and delete taps had no protection at all. */
					int H = -1;
					for (int k = TRK_PLAY; k >= TRK_1; k--)
						if (ep_time[k] >= 24) { H = k; break; }
					if (H > b && ep_time[H] * 2 >= bt) {
						b = H; bt = ep_time[H];
					}
				}
				for (int k = TRK_1; k <= TRK_PLAY; k++) ep_time[k] = 0;
				/* PHANTOM-ARM SWEEP GUARD: the empty-track 40 ms instant
				 * arm can be tripped by a slow roll toward a HIGHER button
				 * dwelling on an empty track in transit. The episode's
				 * dominant button tells the truth at release: any track
				 * that armed during this episode but is NOT the dominant
				 * was a transit artifact — cancel it (an ARMED take
				 * cancels losslessly; one that already caught sound
				 * finalizes tiny and double-tap deletes). */
				for (int x = 0; x < NTRK; x++) {
					if (!armed_press[x] || x == b) continue;
					armed_press[x] = 0;
					if (g_rec_track == x) {
						g_stop_req = 1;
						tap_deadline[x] = 0;
					}
				}
				if (b >= TRK_1 && b <= TRK_4) {
					int ti = b;
					if (ti == stop_tap_trk) {
						stop_tap_trk = -1;   /* R1: stop fired at press;
						                      * this release is spent */
					} else if (armed_press[ti]) {
						armed_press[ti] = 0;
						/* LATCHED RECORDING: releasing the arming hold
						 * does NOT stop the take — it records hands-free
						 * until the same track is tapped again or the
						 * region fills. (See the HOLD-ARM comment for why
						 * the momentary variant was rolled back.) */
					} else if ((g_rec_track == ti &&
						    (trk[ti].state == TS_ARMED ||
						     trk[ti].state == TS_REC)) ||
						   trk[ti].state == TS_DONE) {
						/* tap on the recording track = STOP
						 * (on ARMED-but-silent = cancel; on a
						 * just-auto-finalized TS_DONE take the
						 * tap is swallowed — never a mute or
						 * delete window on a fresh take). */
						g_stop_req = 1;
						tap_deadline[ti] = 0;
					} else if (tap_deadline[ti] > 0 && tnow <= tap_deadline[ti]) {
						/* STEM TAPE PHASE 1: double-tap delete is removed --
						 * this phase has no destructive track-deletion
						 * capability (and nothing to delete: tracks are only
						 * ever loaded read-only from existing storage). The
						 * gesture is still recognized (so the 2nd tap doesn't
						 * fall through to mute-toggle) but g_del_req is never
						 * set, so trk[i].state = TS_EMPTY (main.c's delete
						 * handler) is never reached. */
						tap_deadline[ti] = 0;   /* 2nd tap: recognized, no-op */
						trk[ti].muted = 0;
					} else if (track_hold[ti].solo_active) {
						/* STEM TAPE Phase 3 (corrected, momentary):
						 * this hold crossed TRACK_HOLD_SOLO_MS while
						 * still down -- the per-pass loop below (R1-
						 * STOP-ON-PRESS's own neighbor) already set
						 * trk[ti].solo = true the pass it crossed,
						 * and will clear both trk[ti].solo and
						 * track_hold[ti].solo_active on ITS OWN next
						 * pass (committed no longer names ti). Read
						 * track_hold[ti].solo_active HERE, on THIS
						 * pass, before that clear runs -- correct
						 * because this release-episode block always
						 * runs before that per-pass loop within one
						 * iteration (see st_track_hold.h's own doc
						 * comment on this exact ordering). Momentary,
						 * not latched: clear trk[ti].solo immediately
						 * too (belt-and-suspenders with the per-pass
						 * loop's own clear) and, critically, do NOT
						 * fall through to mute -- a long hold must
						 * never also toggle mute. No double-tap
						 * window is armed here -- a held gesture was
						 * never a tap. */
						trk[ti].solo = 0;
					} else if (
#if SP1_XFER_ENABLE
						   atomic_get(&g_stem_song_selected) != 0
#else
						   false
#endif
						  ) {
						/* STEM TAPE: A TAP IS NOT A MUTE.
						 *
						 * A Track press is momentary solo and nothing
						 * else, so a quick tap is simply a very short
						 * solo -- it must never leave persistent state
						 * behind. This branch exists precisely to stop
						 * the classic tap-to-mute below from running:
						 * mute is neither toggled nor persisted to
						 * g_meta.song_mode[], and no double-tap window
						 * is armed. Nothing at all happens on release,
						 * which is the correct behaviour for a gesture
						 * whose entire effect lived in the hold. */
						tap_deadline[ti] = 0;
					} else {
						/* CLASSIC engine only (no stem song selected):
						 * tap -> mute, INSTANT on gridded and
						 * ungridded songs alike (v2.0.0: the M8c
						 * bar-wait was removed after live testing —
						 * see the bar-service note). */
						trk[ti].muted = !trk[ti].muted;
						if (g_slot < NUM_SLOTS) {  /* M7-r4: remember per song */
							uint8_t mb = (uint8_t)(0x10u << ti);
							if (trk[ti].muted) g_meta.song_mode[g_slot] |= mb;
							else               g_meta.song_mode[g_slot] &= (uint8_t)~mb;
							g_meta_save_req = 1;
						}
						tap_deadline[ti] = tnow + DTAP_GAP_MS;
					}
				} else if (b == TRK_PLAY) {
					/* PLAY tap -> toggle play/stop. ep_play_held was set
					 * the instant the hold-restart fired (a hold is not a
					 * tap). Ignored while a take is in progress: stopping
					 * would freeze the recording mid-take. */
					if (!ep_play_held && g_rec_track < 0) {
						g_playing = !g_playing;
						if (g_playing) g_midi_start_pending = 1;
						else           g_midi_stop_pending  = 1;
					}
				}
				ep_play_held = 0;
			}
			/* R1 STOP-ON-PRESS (perfect-loop): on the RECORDING track a
			 * press can only mean STOP — no mute/delete/arm ambiguity —
			 * so fire it once the commit has SUSTAINED ~48 ms (transit
			 * grazes commit for at most ~32 ms per the episode notes)
			 * instead of waiting for the release: the tap's physical
			 * duration (50-150 ms, different every time) no longer
			 * stretches the loop. CRITICAL: armed_press excludes the
			 * press that ARMED this take — releasing the arming hold
			 * stays latched (it must never read as a stop; without this
			 * the arm cancelled itself ~50 ms after arming). The
			 * episode-end handler above swallows this press's release;
			 * R2 backdates the remaining constant. */
			if (committed >= TRK_1 && committed <= TRK_4) {
				int ti = (int)committed;
				if (ti != stop_tap_trk && !armed_press[ti] &&
				    ((g_rec_track == ti &&
				      (trk[ti].state == TS_ARMED ||
				       trk[ti].state == TS_REC)) ||
				     trk[ti].state == TS_DONE) &&
				    k_uptime_get() - press_t[ti] >= 48) {
					g_stop_req = 1;
					tap_deadline[ti] = 0;
					stop_tap_trk = ti;
				}
			}
			/* STEM TAPE Phase 3 control-matrix (momentary hold-to-
			 * solo, corrected): ticks EVERY track's own st_track_
			 * hold_t every pass -- not just the currently-committed
			 * one -- since a track that is no longer `committed`
			 * this pass is exactly how st_track_hold_tick() learns
			 * it was released (pressed=false) and clears. This runs
			 * AFTER the release-episode handler above in program
			 * order, which matters: on the exact pass a release is
			 * detected, that handler already read track_hold[ti].
			 * solo_active (see its own comment) BEFORE this loop can
			 * clear it -- so the ordering here is load-bearing, not
			 * incidental. On every earlier pass, while still held,
			 * this is what actually flips trk[ti].solo true the
			 * instant held time crosses TRACK_HOLD_SOLO_MS -- solo
			 * takes effect DURING the hold, not only at release,
			 * which is what makes it read as momentary rather than a
			 * delayed toggle. */
			{
				/* ---- MOMENTARY SOLO, from the ONE published mask
				 *
				 * With a stem song selected the Track row belongs to
				 * st_ctl_service(): it publishes a settled 4-bit mask
				 * from a single ladder sample, and stem_ctl_apply()
				 * has already written it into trk[].solo -- the exact
				 * bits the mixer multiplies by and the exact bits the
				 * LED path lights. Nothing here re-decides it, and
				 * there is no second interpretation of the rail to
				 * disagree with.
				 *
				 * The classic path keeps st_track_hold_tick() and its
				 * threshold unchanged: this firmware still builds the
				 * inherited engine for the no-song case, and that
				 * behaviour is not ours to redefine. */
				if (!stem_ctl) {
					for (int k = 0; k < NTRK; k++) {
						bool held_now =
							(committed == (enum trk_btn)k) &&
							!pwr_pressed();
						uint32_t held_ms = held_now
							? (uint32_t)(k_uptime_get() -
								      press_t[k]) : 0u;

						trk[k].solo = st_track_hold_tick(
							&track_hold[k], held_now,
							held_ms, TRACK_HOLD_SOLO_MS)
							? 1u : 0u;
					}
				}
			}
			/* HOLD-ARM, always LATCHED on release. EMPTY tracks arm after
			 * just 100 ms: a tap has no meaning there (nothing to mute or
			 * delete), so there is nothing to disambiguate — and 100 ms is
			 * above the realistic transit-graze range (blips commit
			 * 24-32 ms; only a deliberately lazy roll dwells ~100 ms+, and
			 * the episode-end sweep guard cancels those losslessly).
			 * Unlike the rolled-back 40 ms instant-arm there is NO
			 * provisional RAM-only phase here — flushing starts
			 * immediately, so the write pattern is identical to the
			 * release (the provisional's clumped catch-up burst was what
			 * starved playback at high tape speed). Content tracks keep
			 * the full HOLD_RECORD_MS so tap-mute stays instant. The
			 * hold-duration MOMENTARY variant stays rolled back: with a
			 * slow arm its latch window collapsed to a sliver and broke
			 * hands-free recording. */
			/* STEM TAPE PHASE 1: HOLD-ARM (hold-to-record) is removed -- this
			 * phase has no recording capability, so a hold on a track button is
			 * simply not a recognized gesture (armed_press[] stays 0; g_arm_req[]
			 * is never set, so looper_audio_block()'s ARM handler is never
			 * reached, and trk[i].state can never become TS_ARMED/TS_REC).
			 * Phase 2 reintroduces recording behind a real, validated write
			 * path -- this is a placeholder for that gesture's future spot, not
			 * a functional no-op inserted mid-chain. */
			g_dbg_btn = (int)committed;                      /* diag: settled button */

			/* PLAY/STOP button: a short TAP toggles play/stop in place (tape ramp);
			 * a HOLD (>=400 ms) jumps to the START of the song and plays — a reliable
			 * "play the whole thing from the top" that never depends on current state. */
			/* ---- THE GLOBAL LOOP lives in st_ctl_service() -----
			 * It is called at the TOP of this control loop, above
			 * the FUNCTION branch, and its result was applied by
			 * stem_ctl_apply() before this block ran.
			 *
			 * It was here in st15, BELOW the FUNCTION branch, and
			 * that is why FUNCTION could never latch a loop: every
			 * path out of that branch is a `continue`, so
			 * function_down was a structural constant false. Moving
			 * the arbitration above it is the fix; leaving a second
			 * copy here would recreate the collision. */

			/* ---- THE INHERITED HOLD-TO-RESTART, FENCED OFF -----
			 * "hold PLAY >= 400 ms -> jump to the top and play" is
			 * the Tape Looper's gesture. In Stem Tape a PLAY hold
			 * means "loop from where I am", and st_ctl_service()
			 * above owns the whole button -- the tap included.
			 *
			 * These two used to coexist, and the 400 ms one always
			 * fired first: it dropped the transport to re-seek and
			 * re-prime the read-ahead ring, and the loop could not
			 * enter until playback came back. That is the three to
			 * four seconds of dead air the player felt.
			 *
			 * `committed` can never be TRK_PLAY with a stem song
			 * selected (the ladder decode above forces TRK_NONE),
			 * so this body is already unreachable; the explicit
			 * gate states the rule rather than leaving it implied. */
			if (!stem_ctl && committed == TRK_PLAY) {
				if (play_t < 0) { play_t = k_uptime_get(); play_held = 0; }
				else if (!play_held && (k_uptime_get() - play_t) >= 400) {
					g_restart_req = 1; play_held = 1;
					/* mark the episode a hold NOW -- a clean
					 * PLAY->idle release dispatches the episode
					 * end before this block runs again. */
					ep_play_held = 1;
				}
			} else {
				play_t = -1;
			}

#if HP_TIM_TEST
			/* HEADPHONE AUTO-MUTE: poll the codec jack-detect ~5x/s and mute the
			 * speaker while headphones are in. Debounced (3 consecutive equal
			 * reads) so a single noisy read can't flip it; failed reads hold. */
			if (g_hp_on == 1) {
				static int hp_poll, hp_cand = -1, hp_cnt;
				if (++hp_poll >= 5) {            /* ~40 ms */
					hp_poll = 0;
					int c = hp_detect_connected();
					if (c >= 0) {
						if (c == hp_cand) {
							if (++hp_cnt >= 3 && c != g_hp_in) {
								g_hp_in = c;
								tas_set_speaker(!c);
							}
						} else { hp_cand = c; hp_cnt = 1; }
					}
				}
			}
#endif

			/* faders -> per-track volume (Q8); ~0..3700 maps to 0..256 (unity).
			 * ROUND-ROBIN one fader per pass (each still updates every ~32 ms —
			 * imperceptible for a volume slider) to keep the main loop's blocking
			 * ADC time low; see the ladder_read comment for why that matters. */
			{
				static int fi;
				/*
				 * A FADER BEING SCRATCHED IS NOT A VOLUME
				 * CONTROL. Without this, moving a stem's fader
				 * under FUNCTION would scratch it AND fade it
				 * out at the same time -- one physical movement
				 * doing two things, which is the bug st_ctl's
				 * whole arbitration exists to prevent. The mask
				 * says which fader the gesture has claimed.
				 */
				const bool claimed =
					(g_stem_ctl_out.fader_consumed_mask &
					 (uint8_t)(1u << fi)) != 0u;
				int fv = claimed ? -1
						  : ladder_read(&adc_ladder[LAD_FADER0 + fi]);
				if (fv >= 0) {        /* ADC error -> hold the last volume */
					uint32_t q = (uint32_t)fv * 256u / 3700u;
					trk[fi].vol_q8 = (uint16_t)(q > 256u ? 256u : q);
				}
				fi = (fi + 1) & 3;
			}

			/* VOL ladder (master vol buttons + FWD/RWD varispeed rocker), DEBOUNCED
			 * the same sticky way as the tracks — it sits on the same noisy rail and
			 * single raw reads were causing spurious volume/tempo jumps. */
			static enum vol_btn vcommit = VOL_NONE, vcand = VOL_NONE;
			static int vcnt;
			/* The pass's ONE reading of this rail (taken at the top,
			 * where the dispatcher could also see it). No second
			 * conversion. */
			enum vol_btn vraw = st_vraw;
			enum vol_btn vbefore = vcommit;
			if (vraw == vcommit)       { vcnt = 0; }
			else if (vraw == vcand)    { if (++vcnt >= 3) { vcommit = vraw; vcnt = 0; } }
			else                       { vcand = vraw; vcnt = 1; }

			/* master volume: one perceptual (~3 dB) step per fresh press, along
			 * g_vol_table[] — gradual from full (256) down to fully muted (0).
			 * Hold to repeat for a quick sweep. */
			{
				static int64_t vrep_t = -1, vrep_last;
				/* True once the overlay's edge has already stepped
				 * this press, so the level path below does not step
				 * it a second time. Cleared when the rail returns to
				 * idle. */
				static bool vfired;
				int vdir = (vcommit == VOL_UP) ? 1 : (vcommit == VOL_DOWN) ? -1 : 0;

				/* A Volume press the loop is using as a division
				 * change is NOT also a master-volume change. One
				 * press does one thing. */
				if (g_stem_ctl_out.vol_consumed) {
					vdir = 0;
				}
				int vstep = 0;
				int64_t tnow = k_uptime_get();

				/* ---- ONE CLICK, ONE STEP ---------------------------
				 * A TAP MUST WORK. Volume shares its buttons with the
				 * FX entry chord, so every press is withheld for the
				 * 120 ms arrival window while the overlay decides
				 * whether a second button is joining it. A press
				 * released inside that window -- an ordinary quick
				 * click -- therefore never reaches the level-based
				 * path below at all: by the time the rail is handed
				 * back the button is already up.
				 *
				 * That is why volume behaved like a slider you had to
				 * HOLD. The overlay always emitted the right signal
				 * for it (vol_*_fire, "this was an ordinary press,
				 * act on it once") and main.c simply never read it.
				 *
				 * Read now, as one step, with no debounce of its own:
				 * st_fx_ctl.c has already resolved the gesture, and
				 * re-debouncing a decision would just delay it. */
				{
					int fdir = 0;

					if (g_stem_fx_out.vol_plus_fire)  fdir = 1;
					if (g_stem_fx_out.vol_minus_fire) fdir = -1;
					if (fdir != 0 && !g_stem_ctl_out.vol_consumed) {
						g_vol_idx += fdir;
						if (g_vol_idx < 0) g_vol_idx = 0;
						if (g_vol_idx > VOL_STEPS) g_vol_idx = VOL_STEPS;
						g_master_vol_q8 = g_vol_table[g_vol_idx];
						/* Hold-to-repeat starts from HERE, so a held
						 * press sweeps after the usual delay without
						 * the level path re-stepping it first. */
						vfired = true;
						vrep_t = tnow;
						vrep_last = tnow;
					}
				}

				if (vdir != 0) {
					if (vcommit != vbefore) {
						/* Fresh commit. Step unless the overlay's
						 * edge already did it for this press. */
						if (!vfired) { vstep = 1; vrep_t = tnow; vrep_last = tnow; }
					} else if (tnow - vrep_t >= 500 && tnow - vrep_last >= 110) {
						vstep = 1; vrep_last = tnow;
					}
				} else { vrep_t = -1; }
				/* Armed again only when the button is PHYSICALLY up.
				 * Tested against the raw rail, not st_vraw: the
				 * overlay sets st_vraw to VOL_NONE whenever it claims
				 * a press, so using it here would re-arm mid-hold and
				 * let the level path fire a second step for the same
				 * click. */
				if (st_vol_decode(st_vol_raw) == VOL_NONE) {
					vfired = false;
				}
				/* ---- LOOP DIVISION vs MASTER VOLUME ----------------
				 * The division change is st_ctl_service()'s: it sees
				 * the same rail sample this block does, debounces it
				 * to one edge per press, and reports back through
				 * vol_consumed (applied to `vdir` above) whether it
				 * took the press. There is no second edge flag and no
				 * one-pass hand-off between blocks -- the st15 pair of
				 * g_stem_loop_vol_*_edge globals is gone.
				 *
				 * The rule the dispatcher implements: a Volume press
				 * resizes the loop only while a modifier is physically
				 * held -- PLAY during a momentary loop, FUNCTION once
				 * it is latched. A bare Volume press is always master
				 * volume, which is the behaviour a player relies on
				 * constantly. */
				if (vstep) {
					g_vol_idx += vdir;
					if (g_vol_idx < 0) g_vol_idx = 0;
					if (g_vol_idx > VOL_STEPS) g_vol_idx = VOL_STEPS;
					g_master_vol_q8 = g_vol_table[g_vol_idx];
				}
			}
			/* FWD/RWD rocker -> tempo, 1 BPM PER CLICK for fine control (the old
			 * version ramped ~37 BPM/s — way too coarse). Holding repeats slowly
			 * (~12 BPM/s) after 600 ms so big jumps don't need 40 clicks. Speed is
			 * derived exactly from the integer BPM, so 80 = exactly 1.0x.
			 * DOUBLE-CLICK (a 2nd click within 350 ms, same direction) = jump a
			 * SEMITONE: snap to the next 2^(k/12) grid point (see k_semi_q16),
			 * computed from the speed BEFORE the first click so the +/-1 BPM
			 * that click already applied is absorbed, not compounded. Further
			 * quick clicks chain more semitones. Single click and hold are
			 * exactly as before. */
			{
				static int64_t tempo_t = -1, tempo_last;
				static int64_t dclick_t;        /* last fresh click (0 = none) */
				static int     dclick_dir;      /* its direction */
				static uint32_t dclick_base;    /* the speed BEFORE that click */
				/* tempo LOCKED while a take is in flight: a mid-take speed
				 * glide records the warp into the loop (tape-bend artifact) */
				/*
				 * A ROCKER THE SCRATCH HAS CLAIMED IS NOT A
				 * PITCH CONTROL. Without this, a master shuttle
				 * would also transpose the song a half semitone
				 * per press -- and the transposition would still
				 * be there after the hand came off the record,
				 * which is the worst kind of side effect: silent
				 * at the time, audible afterwards, and with no
				 * gesture to undo it.
				 */
				int dir = (g_rec_track >= 0) ? 0 :
					  g_stem_ctl_out.rocker_consumed ? 0 :
					  (vcommit == VOL_TEMPO_UP) ? 1 :
					  (vcommit == VOL_TEMPO_DOWN) ? -1 : 0;
				int step = 0;

				/* ---- STEM TAPE OWNS THIS ROCKER ------------
				 * With a stem song selected the rocker is the
				 * song's SEMITONE control, not the classic
				 * engine's tempo. The gesture vocabulary is the
				 * one this block has always used -- single
				 * click, double click within the same 350 ms --
				 * but the quantity is pitch, so a single is a
				 * half semitone and a double a whole one.
				 *
				 * st_pitch owns the recognition, including the
				 * rule that a double must never also emit the
				 * single. The click EDGE is what is fed; a held
				 * rocker deliberately does NOT repeat, because
				 * a pitch that runs away under a resting finger
				 * is not a control. The tick runs every pass,
				 * click or not, because that is what commits a
				 * single once its window closes.
				 *
				 * EXCLUSIVE, and it has to be: below this the
				 * classic g_play_bpm/g_play_speed_q16 are
				 * written, and in stem mode nothing reads them
				 * -- the stem fast path bypasses that engine
				 * entirely. Running both would make two owners
				 * of one rocker, one of them doing dead work.
				 *
				 * Structured as an if/else rather than an early
				 * `continue`: this loop ends in feed_wdt() and
				 * led_service(), so skipping the rest of the
				 * pass would starve the watchdog and freeze the
				 * lights every time the rocker was touched. */
				const bool pitch_owns_rocker =
					atomic_get(&g_stem_song_selected) != 0;

				if (pitch_owns_rocker) {
					const uint32_t pnow = k_uptime_get_32();

					if (dir != 0 && vcommit != vbefore) {
						(void)st_pitch_click(&s_stem_pitch,
								      dir, pnow);
					}
					(void)st_pitch_tick(&s_stem_pitch, pnow);
					dir = 0;   /* the classic path sees no click */
				}

				if (dir != 0) {
					int64_t tnow = k_uptime_get();
					if (vcommit != vbefore) {            /* fresh click */
						if (dclick_t != 0 && dir == dclick_dir &&
						    tnow - dclick_t <= 350) {
							/* DOUBLE-CLICK -> next semitone */
							uint32_t ns = semitone_next(dclick_base, dir);
							int b = (int)(((uint64_t)ns * LOOP_BPM_BASE
								       + 32768u) / 65536u);
							if (b < BPM_MIN) {
								b = BPM_MIN;
								ns = (uint32_t)b * 65536u / LOOP_BPM_BASE;
							} else if (b > BPM_MAX) {
								b = BPM_MAX;
								ns = (uint32_t)b * 65536u / LOOP_BPM_BASE;
							}
							g_play_bpm = b;
							g_play_speed_q16 = ns;
							dclick_base = ns;   /* chain steps the grid */
							dclick_t = tnow;
							tempo_t = -1;       /* a double never hold-repeats */
						} else {
							dclick_base = g_play_speed_q16;
							dclick_dir  = dir;
							dclick_t    = tnow;
							step = 1; tempo_t = tnow; tempo_last = tnow;
						}
					} else if (tempo_t >= 0 && tnow - tempo_t >= 600 &&
						   tnow - tempo_last >= 80) {  /* slow hold-repeat */
						step = 1; tempo_last = tnow;
						dclick_t = 0;   /* a hold is not a click */
					}
				} else {
					tempo_t = -1;
				}
				if (step) {
					int b = g_play_bpm + dir;
					if (b < BPM_MIN) b = BPM_MIN;
					if (b > BPM_MAX) b = BPM_MAX;
					g_play_bpm = b;
					g_play_speed_q16 = (uint32_t)b * 65536u / LOOP_BPM_BASE;
				}
			}

			midi_service();         /* drain USB-MIDI queue, update held-note table */
			led_service();         /* one owner: song row + track row + standby */
			feed_wdt();
			k_msleep(8);
		}
	}

	return 0;
}
