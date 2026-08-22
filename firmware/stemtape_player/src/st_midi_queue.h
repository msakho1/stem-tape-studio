/*
 * st_midi_queue.h -- pure, host-testable USB-MIDI receive decode + bounded
 * event queue for Stem Tape cue control (KO II pad events via a standalone
 * MIDI bridge). No recording, no outgoing MIDI, no MIDI clock.
 *
 * st_midi_decode_ump32() decodes the first 32-bit word of a Universal MIDI
 * Packet (UMP) exactly the way Zephyr's real usbd_midi2 class / <zephyr/
 * audio/midi.h> macros do (UMP_MT/UMP_MIDI_COMMAND/UMP_MIDI_CHANNEL/
 * UMP_MIDI1_P1/UMP_MIDI1_P2 -- verified against Zephyr v4.3.0 source). This
 * is a faithful, independent reimplementation of that exact bit math so the
 * decode logic itself is host-testable without depending on Zephyr headers
 * (the firmware's real rx_packet_cb, in main.c, calls this SAME function on
 * ump.data[0] -- see main.c's USB-MIDI wiring). It is not a second,
 * divergent parser.
 *
 * Matches docs/FIRMWARE_CONTRACT_V1.md's MIDI contract exactly: accepted
 * events are noteOn, noteOff, allNotesOff (CC 123); noteOn velocity 0
 * normalizes to noteOff; clock, aftertouch, program change, sysex and every
 * other CC are dropped (not queued).
 */

#ifndef STEMTAPE_PLAYER_MIDI_QUEUE_H_
#define STEMTAPE_PLAYER_MIDI_QUEUE_H_

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	ST_MIDI_EVT_NOTE_ON = 0,
	ST_MIDI_EVT_NOTE_OFF,
	ST_MIDI_EVT_ALL_NOTES_OFF,
} st_midi_evt_kind_t;

typedef struct {
	st_midi_evt_kind_t kind;
	uint8_t channel;   /* 0..15 */
	uint8_t note;      /* 0..127; 0 for ST_MIDI_EVT_ALL_NOTES_OFF */
	uint8_t velocity;  /* 0..127; 0 for ST_MIDI_EVT_ALL_NOTES_OFF */
} st_midi_event_t;

/* Decodes one UMP MIDI1-Channel-Voice message's first word. Returns true and
 * fills *out for Note On, Note Off, or CC123 (All Notes Off); returns false
 * (does not touch *out) for every other message type or command -- the
 * correct "drop" outcome for clock/aftertouch/program-change/other CCs/etc,
 * per the documented MIDI contract. A Note On with velocity 0 is normalized
 * to ST_MIDI_EVT_NOTE_OFF here, once, in the one real decoder. */
bool st_midi_decode_ump32(uint32_t word0, st_midi_event_t *out);

/* Bounded SPSC-safe-by-construction ring (single producer: the USB stack's
 * rx_packet_cb; single consumer: the control-loop thread that drains it --
 * matches the existing g_arm_req/g_del_req single-writer/single-reader
 * convention elsewhere in this codebase, not a new concurrency primitive).
 * Overflow policy: PUSH NEVER BLOCKS AND NEVER GROWS. When full, the
 * OLDEST queued event is dropped to make room for the new one (favors
 * current gesture state over stale history -- correct for a live cue
 * controller) and `dropped` is incremented for diagnostics. */
#define ST_MIDI_QUEUE_CAPACITY 32u

typedef struct {
	st_midi_event_t buf[ST_MIDI_QUEUE_CAPACITY];
	uint8_t head;   /* next slot to pop */
	uint8_t tail;   /* next slot to push */
	uint8_t count;
	uint32_t dropped;
} st_midi_queue_t;

void st_midi_queue_init(st_midi_queue_t *q);
void st_midi_queue_push(st_midi_queue_t *q, const st_midi_event_t *ev);
bool st_midi_queue_pop(st_midi_queue_t *q, st_midi_event_t *out);
void st_midi_queue_clear(st_midi_queue_t *q);

#endif /* STEMTAPE_PLAYER_MIDI_QUEUE_H_ */
