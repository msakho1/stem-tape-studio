/* st_midi_queue.c -- see st_midi_queue.h. Pure, no Zephyr dependency. */

#include "st_midi_queue.h"

#include <string.h>

bool st_midi_decode_ump32(uint32_t word0, st_midi_event_t *out)
{
	uint32_t mt = word0 >> 28;

	if (mt != 0x02u) { /* UMP_MT_MIDI1_CHANNEL_VOICE */
		return false;
	}

	uint32_t status  = (word0 >> 16) & 0xFFu;
	uint32_t command = status >> 4;
	uint8_t  channel = (uint8_t)(status & 0x0Fu);
	uint8_t  p1      = (uint8_t)((word0 >> 8) & 0x7Fu);
	uint8_t  p2      = (uint8_t)(word0 & 0x7Fu);

	if (command == 0x9u) { /* UMP_MIDI_NOTE_ON */
		out->kind     = (p2 == 0u) ? ST_MIDI_EVT_NOTE_OFF : ST_MIDI_EVT_NOTE_ON;
		out->channel  = channel;
		out->note     = p1;
		out->velocity = p2;
		return true;
	}
	if (command == 0x8u) { /* UMP_MIDI_NOTE_OFF */
		out->kind     = ST_MIDI_EVT_NOTE_OFF;
		out->channel  = channel;
		out->note     = p1;
		out->velocity = p2;
		return true;
	}
	if (command == 0xBu && p1 == 123u) { /* Control Change 123 = All Notes Off */
		out->kind     = ST_MIDI_EVT_ALL_NOTES_OFF;
		out->channel  = channel;
		out->note     = 0;
		out->velocity = 0;
		return true;
	}
	return false; /* clock, aftertouch, program change, sysex, other CCs: dropped */
}

void st_midi_queue_init(st_midi_queue_t *q)
{
	memset(q, 0, sizeof(*q));
}

void st_midi_queue_push(st_midi_queue_t *q, const st_midi_event_t *ev)
{
	if (q->count == ST_MIDI_QUEUE_CAPACITY) {
		/* full: drop the oldest to make room, per the documented policy */
		q->head = (uint8_t)((q->head + 1u) % ST_MIDI_QUEUE_CAPACITY);
		q->count--;
		q->dropped++;
	}
	q->buf[q->tail] = *ev;
	q->tail = (uint8_t)((q->tail + 1u) % ST_MIDI_QUEUE_CAPACITY);
	q->count++;
}

bool st_midi_queue_pop(st_midi_queue_t *q, st_midi_event_t *out)
{
	if (q->count == 0u) {
		return false;
	}
	*out = q->buf[q->head];
	q->head = (uint8_t)((q->head + 1u) % ST_MIDI_QUEUE_CAPACITY);
	q->count--;
	return true;
}

void st_midi_queue_clear(st_midi_queue_t *q)
{
	q->head = 0;
	q->tail = 0;
	q->count = 0;
	/* `dropped` is a lifetime diagnostic counter -- not reset by clear() */
}
