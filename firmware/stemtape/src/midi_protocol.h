/*
 * Stem Tape firmware — Milestone M0 MIDI surface contract.
 *
 * M0 transmits RAW PHYSICAL STATE ONLY. No looper semantics, no chords, no
 * hold/tap discrimination, no LED feedback derived from musical state: the
 * host (the Stem Tape web app / Stem Instrument Mode) owns all interpretation.
 *
 * Everything is sent on MIDI channel 1 (channel index 0), UMP group 0,
 * MIDI 1.0 channel-voice messages inside USB MIDI 2.0 packets.
 *
 *   Button down -> Note On,  velocity 127
 *   Button up   -> Note Off, velocity 0
 *   Fader move  -> Control Change, 7-bit absolute value
 *
 * Note numbers were chosen to sit in one contiguous block starting at 36
 * (C2) so a generic MIDI monitor shows the whole surface on one row.
 */

#ifndef STEMTAPE_MIDI_PROTOCOL_H_
#define STEMTAPE_MIDI_PROTOCOL_H_

#define ST_MIDI_GROUP    0u
#define ST_MIDI_CHANNEL  0u   /* channel 1 */

/* ---- Notes: buttons (raw down/up) ---- */
#define ST_NOTE_TRACK1     36u
#define ST_NOTE_TRACK2     37u
#define ST_NOTE_TRACK3     38u
#define ST_NOTE_TRACK4     39u
#define ST_NOTE_PLAY       40u
#define ST_NOTE_FUNCTION   41u   /* the power / FUNCTION button, P0.27 */
#define ST_NOTE_VOL_UP     42u
#define ST_NOTE_VOL_DOWN   43u
#define ST_NOTE_ROCKER_FWD 44u   /* the FWD half of the tempo rocker */
#define ST_NOTE_ROCKER_RWD 45u   /* the RWD half of the tempo rocker */

#define ST_VEL_DOWN 127u
#define ST_VEL_UP     0u

/* ---- Control Change: continuous controls ---- */
#define ST_CC_FADER1   20u
#define ST_CC_FADER2   21u
#define ST_CC_FADER3   22u
#define ST_CC_FADER4   23u
#define ST_CC_BATTERY  24u   /* 0..127, sent on change, ~1 Hz maximum */

#define ST_CC_ALL_NOTES_OFF 123u

/* Firmware identity, printed on the CDC console at boot. v1.1.0 adds the
 * Stem Tape LED Feedback Protocol v1 (see led_protocol.h); the MIDI channel-1
 * contract above is unchanged. */
#define ST_FW_VERSION "Stem Tape M0 v1.1.0"

#endif /* STEMTAPE_MIDI_PROTOCOL_H_ */
