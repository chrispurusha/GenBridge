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
// Notes: Docs/code-notes/gbMidi.h.md - "// notes §k" refers there.

#ifndef GB_MIDI_H
#define GB_MIDI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// notes §1

#define GB_MIDI_NAME_LEN    (128)
#define GB_MIDI_MAX_DEST    (64)

// Safe to call repeatedly; the client and port are made once.
bool     gb_midi_init(void);

int      gb_midi_destination_count(void);
void     gb_midi_destination_name(int index, char * out, unsigned long len);

// Sends to the destination at that index. False if the index names nothing.
bool     gb_midi_send(int index, const uint8_t * data, uint32_t length);

// notes §2
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
