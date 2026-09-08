/*
 * GenBridge - bridge any CoreAudio device into a DAW.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef GB_MIDI_H
#define GB_MIDI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// MIDI out, for the instrument variant: the DAW plays the hardware and GenBridge captures what
// comes back. That round trip is the whole point of an instrument version - without it the plug-in
// can only record something a person is playing by hand.
//
// Packet construction and MIDISend belong to SynthLib (synthlibMidi.c); what is here is the client,
// the output port and the destination list.

#define GB_MIDI_NAME_LEN    (128)
#define GB_MIDI_MAX_DEST    (64)

// Safe to call repeatedly; the client and port are made once.
bool     gb_midi_init(void);

int      gb_midi_destination_count(void);
void     gb_midi_destination_name(int index, char * out, unsigned long len);

// Sends to the destination at that index. False if the index names nothing.
bool     gb_midi_send(int index, const uint8_t * data, uint32_t length);

// THE SAME SEND, STAMPED - and the difference is a note's position INSIDE the host's block.
//
// A host hands over a block of events with a sample offset apiece, and a note at offset 900 of a
// 1024-frame block belongs 900 frames after that block begins. Firing everything the moment
// process() is entered puts every note up to a whole block EARLY, which at 128 frames is inaudible
// and at 2048 is 42 ms - so a part recorded back through the capture path drifts earlier the larger
// the host's buffer gets, and no latency figure can correct it because the error is per-note.
//
// CoreMIDI delivers a packet at its timestamp rather than on arrival, so the offset can simply be
// added. A hostTime of 0 means "now", as it does in CoreMIDI itself, and a time already past is
// delivered immediately - which is what makes this safe when a callback runs late.
bool     gb_midi_send_at(int index, const uint8_t * data, uint32_t length, uint64_t hostTime);

// Which slot currently holds that destination, or -1 if it is not present. Names rather than
// indices are what a saved project stores, because the list shifts as gear is powered on and off.
int      gb_midi_slot_for_name(const char * name);

// Destinations are cached, for the reason the audio device list is - enumerating on every frame
// takes locks in a system framework.
void     gb_midi_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif // GB_MIDI_H
