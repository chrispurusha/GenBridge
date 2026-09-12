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
// Notes: Docs/code-notes/gbParams.c.md - "// notes §k" refers there.

#include <stdio.h>
#include <string.h>

#include "gbDraw.h"
#include "gbMidi.h"
#include "gbParams.h"

// THE STEPPER LISTS. 44.1 through 96 covers what the hardware this is aimed at actually runs at;
// 16 through 1024 frames is what a CoreAudio device will normally accept.
const double gGbRates[]    = { 44100.0, 48000.0, 88200.0, 96000.0 };
const int    gGbFrames[]   = { 16, 32, 64, 128, 256, 512, 1024 };
const int    gGbRateCount  = (int)(sizeof(gGbRates) / sizeof(gGbRates[0]));
const int    gGbFrameCount = (int)(sizeof(gGbFrames) / sizeof(gGbFrames[0]));

int32_t gb_param_count(bool instrument) {
    // The pass-throughs exist only where there is a MIDI destination to pass them to.
    return instrument ? (int32_t)(kParamCount + GB_CC_COUNT)
                      : (int32_t)(kParamCount - GB_PARAMS_INSTRUMENT_ONLY);
}

// The names are COPIED IN rather than pointed at, so a filled-in tGbParamInfo owns everything it
// says and can be held, passed on or filled from a generated string with no lifetime to think about.
static void name_it(tGbParamInfo * out, const char * title, const char * shortTitle,
                    const char * units) {
    snprintf(out->title, sizeof(out->title), "%s", title);
    snprintf(out->shortTitle, sizeof(out->shortTitle), "%s", shortTitle);
    snprintf(out->units, sizeof(out->units), "%s", (units != NULL) ? units : "");
}

// The controller pass-throughs, which are described rather than tabulated: two thousand entries
// with one name apiece is a table nobody should be asked to read or maintain.
static bool controller_info(int32_t index, tGbParamInfo * out) {
    uint32_t offset     = (uint32_t)(index - kParamCount);
    uint32_t channel    = offset / GB_CC_PER_CHANNEL;
    uint32_t controller = offset % GB_CC_PER_CHANNEL;
    char     title[64];

    if (offset >= (uint32_t)GB_CC_COUNT) {
        return false;
    }

    snprintf(title, sizeof(title), "Ch%u CC%u", channel + 1, controller);

    out->id        = (uint32_t)GB_CC_BASE + offset;
    out->stepCount = 0;

    name_it(out, title, title, NULL);

    // Pitch bend rests in the middle; everything else rests at zero.
    out->defaultNormalized = (controller == (uint32_t)GB_CC_PITCHBEND) ? 0.5 : 0.0;
    out->automatable       = false;    // hidden: somewhere to deliver a pedal, not to draw one in
    out->list              = false;

    return true;
}

bool gb_param_info(int32_t index, bool instrument, tGbParamInfo * out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (index >= (int32_t)kParamCount) {
        return instrument ? controller_info(index, out) : false;
    }

    // notes §1
    out->automatable = true;

    switch (index) {
        case kParamDevice:
            out->id = kParamDevice;
            name_it(out, "Capture Device", "Device", NULL);
            out->stepCount = GB_DEVICE_SLOTS - 1;
            out->list      = true;
            return true;

        case kParamTrim:
            out->id = kParamTrim;
            name_it(out, "Output Trim", "Trim", "x");
            out->defaultNormalized = 0.5;      // unity, since the value maps to 0..2
            return true;

        case kParamRate:
            out->id = kParamRate;
            name_it(out, "Sample Rate", "Rate", NULL);
            out->stepCount         = gGbRateCount - 1;
            out->defaultNormalized = 1.0 / (double)(gGbRateCount - 1);   // 48000
            out->list              = true;
            return true;

        case kParamFrames:
            out->id = kParamFrames;
            name_it(out, "Device Buffer", "Buffer", NULL);
            out->stepCount         = gGbFrameCount - 1;
            out->defaultNormalized = 1.0 / (double)(gGbFrameCount - 1); // 128
            out->list              = true;
            return true;

        case kParamMode:
            out->id = kParamMode;
            name_it(out, "Channel Mode", "Mode", NULL);
            out->stepCount         = 1;
            out->defaultNormalized = 1.0;      // stereo
            out->list              = true;
            return true;

        case kParamFirstChannel:
            out->id = kParamFirstChannel;
            name_it(out, "First Channel", "Chan", NULL);
            out->stepCount = GB_MAX_FIRST_CHANNEL - 1;
            out->list      = true;
            return true;

        default:
            break;
    }

    // The rest exist only on the instrument, and asking for one on the effect is not an error the
    // wrapper has to know about - the count above already stops short of them.
    if (!instrument) {
        return false;
    }

    switch (index) {
        case kParamMidiDest:
            out->id = kParamMidiDest;
            name_it(out, "MIDI Destination", "MIDI", NULL);
            out->stepCount = GB_MIDI_SLOTS - 1;
            out->list      = true;
            return true;

        case kParamMidiChannel:
            out->id = kParamMidiChannel;
            name_it(out, "MIDI Channel", "Channel", NULL);
            out->stepCount = GB_CHANNEL_SLOTS - 1;   // default 0 is "Source"
            out->list      = true;
            return true;

        case kParamMeasure:
            out->id = kParamMeasure;
            name_it(out, "Measure Latency", "Measure", NULL);
            out->stepCount = 1;
            return true;

        case kParamOffsetMs:
            out->id = kParamOffsetMs;
            name_it(out, "Latency Offset", "Offset", "ms");
            out->defaultNormalized = -GB_OFFSET_MIN_MS / (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS);
            return true;

        case kParamTestNote:
            out->id = kParamTestNote;
            name_it(out, "Test Note", "Note", NULL);
            out->stepCount         = 127;
            out->defaultNormalized = (double)GB_MEASURE_NOTE / 127.0;
            out->list              = true;
            return true;

        case kParamSource:
            out->id = kParamSource;
            name_it(out, "Capture Source", "Source", NULL);
            out->stepCount         = 1;
            out->defaultNormalized = 0.0;      // a device, which is what every saved project means
            out->list              = true;
            return true;

        default:
            return false;
    }
}

// A normalised value onto its step, which is what every list parameter here means by its value.
static int slot_of(double normalized, int slots) {
    int slot = (int)((normalized * (double)(slots - 1)) + 0.5);

    return (slot < 0) ? 0 : ((slot >= slots) ? (slots - 1) : slot);
}

bool gb_param_text(uint32_t id, double normalized, char * out, unsigned long len) {
    if ((out == NULL) || (len == 0)) {
        return false;
    }

    switch (id) {
        case kParamDevice:
            gb_input_device_name(gb_device_slot(normalized), out, len);
            return true;

        case kParamTrim:
            snprintf(out, len, "%.2f", normalized * 2.0);
            return true;

        case kParamRate:
            snprintf(out, len, "%.0f Hz", gGbRates[slot_of(normalized, gGbRateCount)]);
            return true;

        case kParamFrames:
            snprintf(out, len, "%d", gGbFrames[slot_of(normalized, gGbFrameCount)]);
            return true;

        case kParamMode:
            snprintf(out, len, "%s", (normalized < 0.5) ? "Mono" : "Stereo");
            return true;

        case kParamFirstChannel:
            snprintf(out, len, "%d", slot_of(normalized, GB_MAX_FIRST_CHANNEL) + 1);
            return true;

        case kParamMidiDest:
            gb_midi_destination_name(slot_of(normalized, GB_MIDI_SLOTS), out, len);
            return true;

        case kParamMidiChannel: {
            int slot = slot_of(normalized, GB_CHANNEL_SLOTS);

            if (slot <= 0) {
                snprintf(out, len, "%s", "Source");
            } else {
                snprintf(out, len, "%d", slot);
            }

            return true;
        }

        case kParamMeasure:
            snprintf(out, len, "%s", (normalized < 0.5) ? "Ready" : "Measuring");
            return true;

        case kParamSource:
            snprintf(out, len, "%s", (normalized < 0.5) ? "Audio device" : "Host input");
            return true;

        case kParamOffsetMs:
            snprintf(out, len, "%+.1f",
                     GB_OFFSET_MIN_MS + (normalized * (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS)));
            return true;

        case kParamTestNote: {
            // C-2 IS NOTE 0, the convention where middle C is C3 - which is what the hardware this
            // is aimed at prints on its own screen. An Analog Rytm's lowest pad is C-2 and a user
            // reading "C-1" there would be a semitone-free octave out.
            static const char * const kName[12] = { "C",  "C#", "D",  "D#", "E",  "F",
                                                    "F#", "G",  "G#", "A",  "A#", "B" };
            int note = slot_of(normalized, 128);

            snprintf(out, len, "%s%d (%d)", kName[note % 12], (note / 12) - 2, note);
            return true;
        }

        default:
            break;
    }

    return false;
}
