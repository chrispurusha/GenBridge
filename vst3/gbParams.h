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

#ifndef GB_PARAMS_H
#define GB_PARAMS_H

#include <stdbool.h>
#include <stdint.h>

#include "device.h"
#include "gbMidi.h"

#ifdef __cplusplus
extern "C" {
#endif

// WHAT THE PARAMETERS ARE AND WHAT THEIR NUMBERS MEAN - in C, and in ONE place.
//
// This is the plug-in's own description of itself, not the VST3 API: an id, a range, a name and how
// a value reads as text. The wrapper turns it into ParameterInfo and String128, which is the only
// part of it that needs C++ at all.
//
// It lives here because three pieces of code have to agree about every scale on this page - the
// panel that draws a control, the wrapper that tells the host what the control is, and the
// processor that acts on the value. They did not: GB_CHANNEL_SLOTS, GB_MAX_FIRST_CHANNEL and the
// offset range were each defined twice, in gbDraw.c and in the wrapper, and a change to one was a
// silent disagreement with the other. gb_device_slot() already carries the note about what that
// costs - "the symptom was hearing a Kronos while the panel said Analog Keys".

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

// HOW MANY OF THEM THE EFFECT LEAVES OUT. The effect has no MIDI out, so no destination, no channel,
// no measurement, no correction to one and no test note - and no capture source either, since taking
// the host's input and handing it back is not a thing an effect can usefully do: SIX of the twelve.
//
// THE INSTRUMENT-ONLY ENTRIES MUST STAY LAST, because that is the whole of this arithmetic. A new
// parameter that both variants have goes BEFORE kParamMidiDest; one only the instrument has goes at
// the end, and this number goes up with it.
//
// IT SAID FOUR UNTIL 2026-09-09, so the effect advertised seven parameters and had six - and the
// seventh, kParamMidiDest, answered getParameterInfo() with kInvalidArgument. A host is entitled to
// walk 0..getParameterCount()-1 and expect every one of them to exist; what it does with the refusal
// is its own business, and none of the possibilities are good. Found by putting the count and the
// table in one file, which is the argument for having done that.
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

// WHERE THE AUDIO COMES FROM. Two values, and the second is the External-Instrument mode: instead of
// opening a CoreAudio device, take the host's own input - Live's interface, routed into the plug-in's
// side-chain - pass it through, and keep the MIDI and the latency measurement exactly as they are.
//
// In that mode the ring, the resampler and the drift loop are all switched off, because the host's
// input and its output are the same clock. Reconciling two clocks is the only reason they exist.
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

// ── Continuous controllers ──────────────────────────────────────────────────
//
// A DAMPER PEDAL IS NOT AN EVENT. VST3 delivers note on and note off as events, and everything else
// a keyboard produces - sustain, mod wheel, expression, pitch bend, aftertouch - as PARAMETER
// changes, routed through IMidiMapping. A plug-in that only walks the event list therefore passes
// notes to the hardware and silently drops the pedal, which is exactly what this one did.
//
// So a parameter is reserved for every controller on every channel, and the processor turns any
// change on one of them back into the MIDI message it came from. They are hidden: a host must know
// they exist to deliver values, but nobody wants two thousand entries in an automation menu.
//
// Per channel rather than flattened, because a bridge carries whatever the DAW sends and a
// multitimbral synth is the obvious use for one. Notes already carry their channel, so flattening
// controllers would make the pedal arrive on a different channel from the notes it belongs to.
//
// THE THREE NUMBERS BELOW ARE THE SDK'S, WRITTEN OUT. They are Vst::kAfterTouch, Vst::kPitchBend
// and Vst::kCountCtrlNumber from ivstmidicontrollers.h, which is a C++ header - so the wrapper
// asserts at compile time that these still match it rather than this file guessing. See the
// static_assert in gbVst3.cpp: if the SDK ever renumbers them, the build stops.
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
