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
// Notes: Docs/code-notes/gbParams.h.md - "// notes §k" refers there.

#ifndef GB_PARAMS_H
#define GB_PARAMS_H

#include <stdbool.h>
#include <stdint.h>

#include "device.h"
#include "gbMidi.h"

#ifdef __cplusplus
extern "C" {
#endif

// notes §1

// The parameters, in registration order. The last four exist only on the instrument.
enum {
    kParamDevice = 0,
    kParamTrim,
    kParamRate,
    kParamFrames,
    kParamMode,          // mono or stereo
    kParamFirstChannel,  // which device channel the capture starts at
    kParamMidiDest,      // instrument only: where note data is sent
    kParamMidiChannel,   // instrument only: which channel to send on
    kParamMeasure,       // instrument only: rising edge runs a latency measurement
    kParamOffsetMs,      // instrument only: manual correction to the measured figure
    kParamTestNote,      // instrument only: which note Measure plays
    kParamSource,        // instrument only: capture from a DEVICE, or from the host's own input
    kParamCount
};

// notes §2
#define GB_PARAMS_INSTRUMENT_ONLY    (6)

#define GB_CHANNELS           (2)

// How many devices the device parameter can address. A stepped parameter needs a fixed step count
// at registration time, and the host caches it, so this cannot follow the machine's actual device
// count as it changes.
#define GB_DEVICE_SLOTS       (DEVICE_MAX)
#define GB_MIDI_SLOTS         (GB_MIDI_MAX_DEST)

// 0 means "whatever channel the note arrived on"; 1..16 force it. Source is the default so that
// adding the control changed nothing for a session that already worked.
#define GB_CHANNEL_SLOTS      (17)

// A 32 input interface is common - the TD-50X here is one - so the first-channel list has to reach
// that far even though most devices are stereo. Slots past the device's real channel count simply
// fail to open, which the panel shows.
#define GB_MAX_FIRST_CHANNEL  (32)

// notes §3
#define GB_SOURCE_DEVICE      (0)
#define GB_SOURCE_HOST        (1)

// The manual correction, in milliseconds, mapped onto a normalised parameter. A measurement cannot
// separate the synth's response from its patch's attack, so the number always wants a human able to
// say "that pad is not really 90 ms late".
#define GB_OFFSET_MIN_MS      (-100.0)
#define GB_OFFSET_MAX_MS      (100.0)

// The default only. Which note Measure plays is settable - a drum machine may have nothing at all
// on middle C, and an Analog Rytm wants the lowest note there is.
#define GB_MEASURE_NOTE       (60)

// notes §4
#define GB_CC_AFTERTOUCH      (128)
#define GB_CC_PITCHBEND       (129)
#define GB_CC_PER_CHANNEL     (130)      // 128 controllers, plus aftertouch and bend
#define GB_CC_BASE            (1000)
#define GB_CC_CHANNELS        (16)
#define GB_CC_COUNT           (GB_CC_PER_CHANNEL * GB_CC_CHANNELS)

// The stepper lists, shared by the panel, the wrapper and the processor so the three cannot
// disagree about what index 2 means.
extern const double gGbRates[];
extern const int    gGbFrames[];
extern const int    gGbRateCount;
extern const int    gGbFrameCount;

// ONE PARAMETER'S DESCRIPTION, in terms that owe nothing to VST3. The wrapper copies these into a
// ParameterInfo and converts the two names to UTF-16; nothing else about a parameter lives there.
typedef struct {
    uint32_t id;
    char     title[64];
    char     shortTitle[32];
    char     units[16];          // empty when it has none
    int32_t  stepCount;          // 0 is continuous
    double   defaultNormalized;
    bool     automatable;        // false means hidden: somewhere for the host to deliver, not to draw
    bool     list;               // a host draws one of these as a drop-down
} tGbParamInfo;

// How many parameters this variant registers, controller pass-throughs included.
int32_t gb_param_count(bool instrument);

// Describe the parameter at a registration INDEX. False when the index does not exist on this
// variant - the instrument-only entries on an effect, or a controller past the last channel.
bool gb_param_info(int32_t index, bool instrument, tGbParamInfo * out);

// What a value READS as. Everything a host or a panel shows for a parameter comes from here, so a
// figure cannot be formatted one way in the generic panel and another on our own.
// False when the id is not one this knows how to render.
bool gb_param_text(uint32_t id, double normalized, char * out, unsigned long len);

#ifdef __cplusplus
}
#endif

#endif // GB_PARAMS_H
