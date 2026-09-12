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
// Notes: Docs/code-notes/gbBridge.h.md - "// notes §k" refers there.

#ifndef GB_BRIDGE_H
#define GB_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device.h"     // DEVICE_UID_LEN, for the active device a saved blob names
#include "gbMidi.h"     // GB_MIDI_NAME_LEN, for the destination it names beside it

#ifdef __cplusplus
extern "C" {
#endif

// notes §1
typedef struct tGbBridge tGbBridge;

// notes §2
typedef struct {
    void * user;
    void (*send_message)(void * user, const char * id, int value);
} tGbHostOps;

tGbBridge * gb_bridge_create(bool instrument);
void gb_bridge_destroy(tGbBridge * self);

// Set once, when the host connects the two ends. Also publishes the status slot, which is how the
// panel knows WHICH instance's figures to read - without it a panel showing a microphone reported
// that it was capturing a Kronos, because it was reading the other instance's.
void gb_bridge_connect(tGbBridge * self, const tGbHostOps * ops);
void gb_bridge_disconnect(tGbBridge * self);

// Hot-plug: start and stop watching the CoreAudio device list. Nothing noticed a device appearing
// before this, so a plug-in waiting for a saved interface waited until the user touched a control.
void gb_bridge_watch_devices(tGbBridge * self);
void gb_bridge_unwatch_devices(tGbBridge * self);

// ACTIVATION OPENS THE DEVICE SYNCHRONOUSLY, and that is the whole reason the host sees a sensible
// latency figure - see the note in gbBridge.c. Deactivation stops the worker and closes.
void gb_bridge_set_active(tGbBridge * self, bool active);

// What the host declared, and whether it intends to render faster than realtime.
void gb_bridge_setup_processing(tGbBridge * self, double sampleRate, int32_t maxBlockFrames,
                                bool offline);

// The transport started: the block timeline starts again with it.
void gb_bridge_processing_started(tGbBridge * self);

// What the host is told, and what it caches until told to read it again.
uint32_t gb_bridge_latency(tGbBridge * self);

// notes §3
uint64_t gb_bridge_block_begin(tGbBridge * self, int32_t frames);

// A parameter change, already reduced to its last point. sampleOffset places a controller inside
// the block exactly as a note is placed.
void gb_bridge_parameter(tGbBridge * self, uint32_t id, double value, int32_t sampleOffset,
                         uint64_t blockHostTime);

// notes §4
void gb_bridge_measure_trigger(tGbBridge * self, bool sawPress, bool held);

typedef enum { eGbNoteOn = 0, eGbNoteOff, eGbPolyPressure } tGbNoteKind;

// A note event on its way to the hardware. Velocity and pressure arrive as VST3 does them, 0..1,
// and are converted here so the one set of rules applies wherever they come from.
void gb_bridge_note(tGbBridge * self, tGbNoteKind kind, int16_t channel, int16_t pitch,
                    float value, int32_t sampleOffset, uint64_t blockHostTime);

// notes §5
void gb_bridge_render(tGbBridge * self, float ** out, const float * const * in, int32_t inChannels,
                      int32_t frames, uint64_t blockHostTime);

// notes §6
bool gb_bridge_set_state(tGbBridge * self, const char * blob, size_t length);

// The blob to write, owned by the bridge and valid until the next call to this. The wrapper reads
// the bytes into the host's stream and nothing more.
const char * gb_bridge_state(tGbBridge * self, size_t * length);

// notes §7
typedef struct {
    char     uid[DEVICE_UID_LEN];
    char     midiName[GB_MIDI_NAME_LEN];
    int      midiChannel;
    int      testNote;
    unsigned frames;
    double   rate;
    unsigned firstChannel;
    unsigned channels;
    float    trim;
    double   offsetMs;
    bool     hostInput;      // capture from the host's own input rather than from a device
    bool     valid;
} tGbActive;

void gb_state_parse_active(const char * blob, size_t length, tGbActive * out);

#ifdef __cplusplus
}
#endif

#endif // GB_BRIDGE_H
