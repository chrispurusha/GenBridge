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

// WHAT GENBRIDGE IS, AS FAR AS A PLUG-IN FORMAT NEEDS TO KNOW - and nothing about any format.
//
// This is what gbVst3.cpp and gbEditor.mm used to be, less every line that was VST3. The bridge
// (gbBridge.h) was already the plug-in with no VST3 in it; this file describes it to SynthLib's
// wrappers, and `./do-plugin` builds GenBridge.vst3 and GenBridge.component from the same objects.
// The COM plumbing, the controller, the factory and the IPlugView all live in SynthLib/plugin/ now,
// shared with G2 Alike.
//
// TWO VARIANTS, ONE BINARY: the effect and the instrument, exactly as the VST3 registered them -
// same class ids, so a project saved against the old wrapper finds the same plug-in.
//
// EVERY INSTANCE IS ITS OWN. Nothing here is file-scope except the descriptors: two GenBridges on two
// tracks share no state, and whatever the plug-in tells the host names the instance it is about.

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synthlibPlugin.h"

#include "gbBridge.h"
#include "gbDraw.h"
#include "gbLog.h"
#include "gbMidi.h"
#include "gbParams.h"
#include "gbView.h"

// THE CONTROLLER PASS-THROUGHS ARE NUMBERED THE WAY SYNTHLIB NUMBERS MIDI CONTROLS, which is VST3's
// numbering - and SynthLib asserts that against the SDK itself, so these three keep all three agreeing.
_Static_assert(GB_CC_AFTERTOUCH == SYNTHLIB_MIDI_AFTERTOUCH, "aftertouch numbering drifted");
_Static_assert(GB_CC_PITCHBEND == SYNTHLIB_MIDI_PITCH_BEND, "pitch bend numbering drifted");
_Static_assert(GB_CC_PER_CHANNEL == SYNTHLIB_MIDI_CONTROLS, "controller count drifted");

// Set by do-plugin from $GENBRIDGE_VERSION, which do-release drives from the git tag. The fallback is
// for anyone compiling these sources by hand, and matches the one the plist defaults to.
#ifndef GB_VERSION_STRING
#define GB_VERSION_STRING    "0.1.0"
#endif

// 0xMMMMmmbb, from the same version by do-plugin, so the Audio Unit's number and the plist's cannot
// disagree.
#ifndef GB_AU_VERSION
#define GB_AU_VERSION        (0x00000100)
#endif

// ------------------------------------------------------------------------------------------------
// Identity
// ------------------------------------------------------------------------------------------------

// THE FOUR CLASS IDS gbVst3.cpp DECLARED AS FUIDs, written out as the bytes they always were. A VST3
// FUID built from four uint32s lays each one out big-endian on every platform but Windows, so
// FUID(0x4A1C8E52, ...) is the bytes 4A 1C 8E 52 ... - the identical plug-in to a host that already
// knows it. THESE MAY NEVER CHANGE: a project saved against them would reopen with an empty slot.
static const uint8_t gEffectProcessorUid[16] = {
    0x4A, 0x1C, 0x8E, 0x52, 0x9D, 0x3B, 0x4F, 0x07, 0xA6, 0xE2, 0x1B, 0x84, 0x53, 0xF0, 0xC9, 0x7D
};
static const uint8_t gEffectControllerUid[16] = {
    0x8B, 0x70, 0xD6, 0xA1, 0x2F, 0x59, 0x4C, 0x38, 0xE1, 0xA7, 0x60, 0x25, 0x9C, 0x4D, 0x3B, 0x8F
};
static const uint8_t gInstrumentProcessorUid[16] = {
    0x6E, 0x2D, 0x4B, 0x91, 0xA0, 0x7C, 0x3F, 0x58, 0x24, 0xB9, 0xE1, 0xD6, 0x8F, 0x53, 0x07, 0xCA
};
static const uint8_t gInstrumentControllerUid[16] = {
    0xC9, 0x4A, 0x1F, 0x63, 0x5B, 0x82, 0xD7, 0x0E, 0x3A, 0x6C, 0x48, 0xB1, 0xD2, 0x5E, 0x9F, 0x04
};

#define FOUR_CC(a, b, c, d)    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                                ((uint32_t)(c) << 8) | (uint32_t)(d))

// THE AUDIO UNIT'S CODES, new with this file and under the same rule as the class ids above. The
// manufacturer is the one G2 Alike registered, since it is the same vendor. do-plugin writes the same
// codes into the .component's Info.plist, which is where macOS actually finds them.
#define GB_AU_EFFECT_TYPE        FOUR_CC('a', 'u', 'f', 'x')     // kAudioUnitType_Effect
#define GB_AU_EFFECT_SUBTYPE     FOUR_CC('G', 'B', 'f', 'x')
#define GB_AU_INSTRUMENT_TYPE    FOUR_CC('a', 'u', 'm', 'u')     // kAudioUnitType_MusicDevice
#define GB_AU_INSTRUMENT_SUBTYPE FOUR_CC('G', 'B', 'i', 'n')
#define GB_AU_MANUFACTURER       FOUR_CC('C', 'P', 'u', 'r')

// MUST MATCH CFBundleIdentifier IN THE .component's Info.plist - the wrapper finds its own bundle by
// it to tell a host where the editor's view class is, and a mismatch is a plug-in with no editor.
#define GB_AU_BUNDLE_ID          "com.chrispurusha.genbridge.au"

// ------------------------------------------------------------------------------------------------
// The instance
// ------------------------------------------------------------------------------------------------

typedef struct {
    const tSynthLibPluginDesc * desc;
    tGbBridge *    bridge;
    bool           instrument;

    // WHICH STATUS SLOT THE BRIDGE CLAIMED, announced by it once at connect time. The editor reads the
    // meters and drift figures straight out of that slot, so this is how a panel knows whose figures
    // are its own - the question a microphone's panel once answered with a Kronos's.
    _Atomic int    statusSlot;

    // WHAT THE PLUG-IN IS SET TO, for getParam(). The pass-throughs are not kept: they are events.
    _Atomic double values[kParamCount];

    // A MEASURE PRESS THAT ARRIVED OFF THE AUDIO THREAD - an Audio Unit host sets parameters from
    // wherever it likes. The measurement's state belongs to the audio thread, so the press waits here
    // for the next block rather than reaching into a measurement in progress.
    _Atomic bool   measurePending;

    // Where this block sits in wall-clock time - see gb_bridge_block_begin(). Audio thread only.
    uint64_t       blockHostTime;

    // The blob gb_bridge_state() made for the SIZING call, handed back unchanged for the filling one:
    // made twice, it could come back a different length the second time. Owned by the bridge.
    const char *   stateCache;
    size_t         stateLength;

    // The side-chain probe's last report - see gb_probe_host_input(). Audio thread only.
    bool           probed;
    uint32_t       probeChannels;
    bool           probeAudible;
} tGbPlugin;

static double clamp01(double v) {
    return (!(v >= 0.0)) ? 0.0 : ((v > 1.0) ? 1.0 : v);
}

static bool is_pass_through(uint32_t id) {
    return (id >= (uint32_t)GB_CC_BASE) && (id < (uint32_t)(GB_CC_BASE + GB_CC_COUNT));
}

// ------------------------------------------------------------------------------------------------
// What the bridge has to say
// ------------------------------------------------------------------------------------------------

// THE BRIDGE'S MESSAGES, which used to cross IConnectionPoint to the controller and are now said to
// the wrapper directly. Almost all of them are the same thing - the plug-in has changed one of its own
// parameters (a device that could not give the channel asked for, a restored project naming a
// different slot) and the host must be told, or its copy of the value goes on disagreeing with ours.
//
// ANY THREAD - the worker sends most of these. synthlib_plugin_param_edited() and _latency_changed()
// post themselves to the main thread, which is where both formats want to hear about it.
static void gb_on_bridge_message(void * user, const char * id, int value) {
    tGbPlugin * g     = (tGbPlugin *)user;
    uint32_t    param = 0;
    double      v     = 0.0;

    if ((g == NULL) || (id == NULL)) {
        return;
    }

    if (strcmp(id, "gbStatusSlot") == 0) {
        atomic_store(&g->statusSlot, value);
        return;
    }

    // The whole point of the round trip: the host caches our latency until told to read it again.
    if (strcmp(id, "gbLatency") == 0) {
        synthlib_plugin_latency_changed(g);
        return;
    }

    if (value < 0) {
        return;
    }

    if (strcmp(id, "gbDeviceSlot") == 0) {
        param = kParamDevice;
        v     = gb_device_normalized(value);
    } else if (strcmp(id, "gbMode") == 0) {
        param = kParamMode;
        v     = (value > 0) ? 1.0 : 0.0;
    } else if (strcmp(id, "gbFirstChannel") == 0) {
        param = kParamFirstChannel;
        v     = (double)value / (double)(GB_MAX_FIRST_CHANNEL - 1);
    } else if (strcmp(id, "gbSource") == 0) {
        param = kParamSource;
        v     = (value > 0) ? 1.0 : 0.0;
    } else if (strcmp(id, "gbOffset") == 0) {
        // Thousandths of a millisecond, because the channel carries an integer.
        double ms = (double)value / 1000.0;

        ms    = (ms < GB_OFFSET_MIN_MS) ? GB_OFFSET_MIN_MS : ((ms > GB_OFFSET_MAX_MS) ? GB_OFFSET_MAX_MS : ms);
        param = kParamOffsetMs;
        v     = (ms - GB_OFFSET_MIN_MS) / (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS);
    } else {
        return;
    }
    atomic_store(&g->values[param], clamp01(v));
    synthlib_plugin_param_edited(g, param, clamp01(v));
}

// ------------------------------------------------------------------------------------------------
// Parameters
// ------------------------------------------------------------------------------------------------

static uint32_t gb_param_count_cb(const tSynthLibPluginDesc * desc, void * inst) {
    (void)inst;
    return (uint32_t)gb_param_count(desc->isInstrument);
}

// gbParams.c's description, in SynthLib's terms. NOTHING HERE IS SAVED BY THE WRAPPER: the bridge's
// own blob already holds every setting, keyed by device - a device picker's INDEX means nothing
// against a device list of a different shape, which is the whole of the microphone bug - and the
// pass-throughs are events. See gb_state_params() for how the panel gets them back.
static bool gb_param_info_cb(const tSynthLibPluginDesc * desc, void * inst, uint32_t index,
                             tSynthLibParamDesc * out) {
    tGbParamInfo info;

    (void)inst;

    if (gb_param_info((int32_t)index, desc->isInstrument, &info) == false) {
        return false;
    }
    out->id                = info.id;
    out->stepCount         = info.stepCount;
    out->defaultNormalized = info.defaultNormalized;
    out->midiControl       = SYNTHLIB_MIDI_NONE;        // midiMapping() decides, per channel
    out->flags             = SYNTHLIB_PARAM_NO_SAVE |
                             (info.automatable ? 0u : SYNTHLIB_PARAM_HIDDEN) |
                             (info.list ? SYNTHLIB_PARAM_LIST : 0u);
    snprintf(out->title, sizeof(out->title), "%s", info.title);
    snprintf(out->shortTitle, sizeof(out->shortTitle), "%s", info.shortTitle);

    // THE PLAIN RANGE, which only an Audio Unit host ever shows - VST3 hands normalized values
    // around. What each reads as is gb_param_text()'s, whichever format asks.
    if (info.id == (uint32_t)kParamOffsetMs) {
        out->unit     = eSynthLibUnitMilliseconds;
        out->plainMin = GB_OFFSET_MIN_MS;
        out->plainMax = GB_OFFSET_MAX_MS;
    } else if (info.id == (uint32_t)kParamTrim) {
        out->unit     = eSynthLibUnitGeneric;       // a gain multiplier, 0..2
        out->plainMin = 0.0;
        out->plainMax = 2.0;
    } else if (info.id == (uint32_t)kParamMeasure) {
        out->unit     = eSynthLibUnitBoolean;
        out->plainMin = 0.0;
        out->plainMax = 1.0;
    } else if (info.list) {
        out->unit     = eSynthLibUnitIndexed;
        out->plainMin = 0.0;
        out->plainMax = (double)info.stepCount;
    } else {
        out->unit     = eSynthLibUnitGeneric;
        out->plainMin = 0.0;
        out->plainMax = 1.0;
    }
    return true;
}

static bool gb_param_text_cb(const tSynthLibPluginDesc * desc, void * inst, uint32_t id,
                             double normalized, char * out, size_t len) {
    (void)inst;

    // THE DESTINATION LIST NEEDS A CLIENT, and a VST3 controller may be asked for the names before
    // any processor - which is what makes one - exists. Idempotent, so cheap to insist on.
    if (desc->isInstrument && (id == (uint32_t)kParamMidiDest)) {
        gb_midi_init();
    }
    return gb_param_text(id, normalized, out, (unsigned long)len);
}

// A CONTROLLER, ON A CHANNEL, BECOMES ITS OWN PARAMETER - see GB_CC_BASE. Per channel rather than
// flattened, because a bridge carries whatever the DAW sends and a pedal must arrive on the channel
// of the notes it belongs to. Only the instrument has anywhere to send them.
static bool gb_midi_mapping_cb(const tSynthLibPluginDesc * desc, void * inst, uint8_t channel,
                               int16_t control, uint32_t * idOut) {
    (void)inst;

    if ((desc->isInstrument == false) || (channel >= GB_CC_CHANNELS) || (control < 0) ||
        (control >= GB_CC_PER_CHANNEL)) {
        return false;
    }
    *idOut = (uint32_t)GB_CC_BASE + ((uint32_t)channel * (uint32_t)GB_CC_PER_CHANNEL) + (uint32_t)control;
    return true;
}

// FROM ANY THREAD, with no block to place it in: an Audio Unit host moving a control, or our own
// editor on that format. Stamped "now", which is what a change with no block behind it means.
static void gb_set_param(void * inst, uint32_t id, double normalized) {
    tGbPlugin * g = (tGbPlugin *)inst;

    if (id < (uint32_t)kParamCount) {
        atomic_store(&g->values[id], normalized);
    }

    if (id == (uint32_t)kParamMeasure) {
        if (normalized >= 0.5) {
            atomic_store(&g->measurePending, true);
        }
        return;
    }
    gb_bridge_parameter(g->bridge, id, normalized, 0, mach_absolute_time());
}

// ON THE AUDIO THREAD, every point the host delivered for one parameter this block.
static void gb_param_points(void * inst, uint32_t id, const tSynthLibParamPoint * points, uint32_t count) {
    tGbPlugin * g = (tGbPlugin *)inst;

    if (count == 0u) {
        return;
    }

    // THE MEASURE BUTTON IS A PRESS, NOT A LEVEL. The editor raises it and drops it again at once, a
    // host may deliver both in one block, and reading only the last point sees nothing but the
    // release - so the press is looked for among all of them, and whether it is still HELD is what
    // the last one says. See gb_bridge_measure_trigger().
    if (id == (uint32_t)kParamMeasure) {
        bool sawPress = false;

        for (uint32_t i = 0; i < count; i++) {
            if (points[i].value >= 0.5) {
                sawPress = true;
                break;
            }
        }
        atomic_store(&g->values[kParamMeasure], points[count - 1].value);
        gb_bridge_measure_trigger(g->bridge, sawPress, points[count - 1].value >= 0.5);
        return;
    }

    // A PASS-THROUGH IS A STREAM OF MIDI MESSAGES, one per point, each at its own offset: a pitch
    // bend sweep that arrives as three messages in a block leaves as three. gbVst3.cpp sent only the
    // last, which kept the level and lost the movement.
    if (is_pass_through(id)) {
        for (uint32_t i = 0; i < count; i++) {
            gb_bridge_parameter(g->bridge, id, points[i].value, (int32_t)points[i].sampleOffset,
                                g->blockHostTime);
        }
        return;
    }

    // Everything else is a level, and a level is where it settles.
    if (id < (uint32_t)kParamCount) {
        atomic_store(&g->values[id], points[count - 1].value);
    }
    gb_bridge_parameter(g->bridge, id, points[count - 1].value, (int32_t)points[count - 1].sampleOffset,
                        g->blockHostTime);
}

static double gb_get_param(void * inst, uint32_t id) {
    tGbPlugin * g = (tGbPlugin *)inst;

    if (id < (uint32_t)kParamCount) {
        return atomic_load(&g->values[id]);
    }

    // A pass-through: pitch bend rests centred, everything else at zero.
    if (is_pass_through(id) && (((id - (uint32_t)GB_CC_BASE) % GB_CC_PER_CHANNEL) == GB_CC_PITCHBEND)) {
        return 0.5;
    }
    return 0.0;
}

// ------------------------------------------------------------------------------------------------
// State
// ------------------------------------------------------------------------------------------------

static void put_value(tSynthLibParamValue * out, uint32_t capacity, uint32_t * count, uint32_t id,
                      double value) {
    if (*count < capacity) {
        out[*count].id    = id;
        out[*count].value = clamp01(value);
        (*count)++;
    }
}

// WHAT A SAVED BLOB SAYS THE PARAMETERS ARE, with no instance to load it into - what the old
// controller's setComponentState() did, moved where both formats can use it. A VST3 host hands the
// processor's state to the controller precisely so the panel agrees with the capture; ignoring it is
// how two tracks saved with a Kronos and a Helix both reopened showing Analog Keys.
//
// A DEVICE IS RESTORED BY ITS UID, NOT ITS SLOT, since a slot is a position in a list that changes
// shape whenever something is plugged in. gb_state_parse_active() resolves the UID; this turns it into
// the slot it occupies today.
static uint32_t gb_state_params(const tSynthLibPluginDesc * desc, const void * data, size_t len,
                                tSynthLibParamValue * out, uint32_t capacity) {
    tGbActive active;
    uint32_t  count = 0;

    if ((data == NULL) || (len < 9u) || (memcmp(data, "GENBRIDGE", 9) != 0)) {
        return 0;       // not one of ours - nothing to say about it
    }
    gb_state_parse_active((const char *)data, len, &active);

    // THE MIDI HALF even when no audio device was stored: an instrument may have had its destination
    // chosen and its capture not.
    if (desc->isInstrument) {
        if (active.midiName[0] != '\0') {
            gb_midi_init();
            gb_midi_invalidate();

            int slot = gb_midi_slot_for_name(active.midiName);

            if (slot >= 0) {
                put_value(out, capacity, &count, kParamMidiDest, (double)slot / (double)(GB_MIDI_SLOTS - 1));
            }
        }
        double ms = (active.offsetMs < GB_OFFSET_MIN_MS) ? GB_OFFSET_MIN_MS
                    : ((active.offsetMs > GB_OFFSET_MAX_MS) ? GB_OFFSET_MAX_MS : active.offsetMs);

        put_value(out, capacity, &count, kParamMidiChannel,
                  (double)active.midiChannel / (double)(GB_CHANNEL_SLOTS - 1));
        put_value(out, capacity, &count, kParamTestNote, (double)active.testNote / 127.0);
        put_value(out, capacity, &count, kParamSource, active.hostInput ? 1.0 : 0.0);

        // THE CORRECTION IS PER DEVICE AND DESTINATION, resolved by the parser from the pair; with no
        // device stored there is no pair, and zero is the honest answer.
        put_value(out, capacity, &count, kParamOffsetMs,
                  (ms - GB_OFFSET_MIN_MS) / (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS));
    }

    if (!active.valid) {
        return count;
    }
    gb_device_list_invalidate();

    int slot = gb_slot_for_uid(active.uid);

    if (slot >= 0) {
        put_value(out, capacity, &count, kParamDevice, gb_device_normalized(slot));
    }

    for (int i = 0; i < gGbRateCount; i++) {
        if (gGbRates[i] == active.rate) {
            put_value(out, capacity, &count, kParamRate, (double)i / (double)(gGbRateCount - 1));
        }
    }

    for (int i = 0; i < gGbFrameCount; i++) {
        if ((unsigned)gGbFrames[i] == active.frames) {
            put_value(out, capacity, &count, kParamFrames, (double)i / (double)(gGbFrameCount - 1));
        }
    }
    put_value(out, capacity, &count, kParamMode, (active.channels == 1u) ? 0.0 : 1.0);
    put_value(out, capacity, &count, kParamFirstChannel,
              (double)active.firstChannel / (double)(GB_MAX_FIRST_CHANNEL - 1));
    put_value(out, capacity, &count, kParamTrim, (double)active.trim / 2.0);
    return count;
}

// THE BYTES, AND THAT IS ALL. What they mean - the versioned line format, the per-device table, the
// measured pairs - is gbState.c's.
static size_t gb_get_state(void * inst, void * out, size_t len) {
    tGbPlugin * g = (tGbPlugin *)inst;

    if ((out == NULL) || (g->stateCache == NULL)) {
        g->stateCache = gb_bridge_state(g->bridge, &g->stateLength);
    }

    if (out == NULL) {
        return g->stateLength;
    }
    size_t take = (g->stateLength < len) ? g->stateLength : len;

    memcpy(out, g->stateCache, take);
    g->stateCache = NULL;
    return take;
}

static void gb_set_state(void * inst, const void * data, size_t len) {
    tGbPlugin *         g = (tGbPlugin *)inst;
    tSynthLibParamValue values[kParamCount];

    if ((data == NULL) || (len == 0u)) {
        return;
    }
    gb_bridge_set_state(g->bridge, (const char *)data, len);

    // And what the parameters are now, for getParam() - the wrapper reads them back after this.
    uint32_t count = gb_state_params(g->desc, data, len, values, kParamCount);

    for (uint32_t i = 0; i < count; i++) {
        if (values[i].id < (uint32_t)kParamCount) {
            atomic_store(&g->values[values[i].id], values[i].value);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------------------------------

static void * gb_create(const tSynthLibPluginDesc * desc) {
    tGbPlugin * g = (tGbPlugin *)calloc(1, sizeof(tGbPlugin));

    if (g == NULL) {
        return NULL;
    }
    g->desc       = desc;
    g->instrument = desc->isInstrument;
    atomic_init(&g->statusSlot, -1);
    atomic_init(&g->measurePending, false);

    for (int i = 0; i < kParamCount; i++) {
        tGbParamInfo info;

        atomic_init(&g->values[i], gb_param_info(i, g->instrument, &info) ? info.defaultNormalized : 0.0);
    }
    g->bridge = gb_bridge_create(g->instrument);

    if (g->bridge == NULL) {
        free(g);
        return NULL;
    }

    // CONNECTED AT ONCE. This used to wait for the host to join processor and controller, because the
    // bridge's messages had to cross between them; they are said to the wrapper now, which knows where
    // each one goes. Connecting also announces the status slot.
    tGbHostOps ops = { g, gb_on_bridge_message };

    gb_bridge_connect(g->bridge, &ops);
    return g;
}

static void gb_destroy(void * inst) {
    tGbPlugin * g = (tGbPlugin *)inst;

    gb_bridge_disconnect(g->bridge);
    gb_bridge_destroy(g->bridge);
    free(g);
}

// Hot-plug: nothing noticed a device appearing before this, so a plug-in waiting for a saved
// interface waited until the user touched a control.
static void gb_initialize(void * inst) {
    gb_bridge_watch_devices(((tGbPlugin *)inst)->bridge);
}

static void gb_terminate(void * inst) {
    gb_bridge_unwatch_devices(((tGbPlugin *)inst)->bridge);
}

static void gb_prepare(void * inst, const tSynthLibSetup * setup) {
    gb_bridge_setup_processing(((tGbPlugin *)inst)->bridge, setup->sampleRate,
                               (int32_t)setup->maxFrames, setup->offline);
}

// ACTIVATION OPENS THE DEVICE, synchronously - see gb_bridge_set_active() for why the host's first
// question about latency depends on it.
static void gb_set_active(void * inst, bool active) {
    gb_bridge_set_active(((tGbPlugin *)inst)->bridge, active);
}

static void gb_set_processing(void * inst, bool running) {
    if (running) {
        gb_bridge_processing_started(((tGbPlugin *)inst)->bridge);
    }
}

static uint32_t gb_latency_samples(void * inst) {
    return gb_bridge_latency(((tGbPlugin *)inst)->bridge);
}

// ------------------------------------------------------------------------------------------------
// Audio
// ------------------------------------------------------------------------------------------------

// ONCE, AND BEFORE ANYTHING THAT SENDS. Every MIDI byte this block produces is stamped from the
// instant this returns - see gb_bridge_block_begin(). A host flushing parameters with no audio hands
// over no frames at all, and there is no block to place them in but "now".
static void gb_block_begin(void * inst, uint32_t frames, const tSynthLibTransport * transport) {
    tGbPlugin * g = (tGbPlugin *)inst;

    (void)transport;
    g->blockHostTime = (frames > 0u) ? gb_bridge_block_begin(g->bridge, (int32_t)frames) : mach_absolute_time();

    if (atomic_exchange(&g->measurePending, false)) {
        gb_bridge_measure_trigger(g->bridge, true, false);
    }
}

// THE SIDE-CHAIN PROBE (2026-09-09). Does anything actually arrive on the instrument's aux input under
// a real host? The External-Instrument mode rests on the answer, and nothing in the code can give it -
// so the shape of what was handed over is logged the first time and again whenever it changes. Once,
// and on change: a line per block would be a file nobody can read and would perturb the timing this
// plug-in measures.
static void gb_probe_host_input(tGbPlugin * g, const float * const * in, uint32_t numIn, uint32_t frames) {
    bool buffers = (in != NULL) && (numIn > 0u) && (in[0] != NULL);
    bool audible = false;

    for (uint32_t i = 0; buffers && (i < frames); i++) {
        if ((in[0][i] > 1.0e-6f) || (in[0][i] < -1.0e-6f)) {
            audible = true;
            break;
        }
    }

    if (g->probed && (numIn == g->probeChannels) && (audible == g->probeAudible)) {
        return;
    }
    g->probed        = true;
    g->probeChannels = numIn;
    g->probeAudible  = audible;
    gb_log_line("HOST INPUT: channels %u, buffers %s, signal %s", numIn, buffers ? "yes" : "NO",
                audible ? "PRESENT" : "none");
}

// THE HOST'S OWN INPUT, HANDED STRAIGHT ON. Which of the two sources the bridge uses is its decision
// (Capture Source); this only makes sure both are available to it. A NULL input is the host having
// routed nothing, which the bridge treats as silence rather than as an error.
static void gb_process(void * inst, const float * const * in, uint32_t numIn, float ** out,
                       uint32_t numOut, uint32_t frames, const tSynthLibTransport * transport) {
    tGbPlugin * g = (tGbPlugin *)inst;

    (void)transport;

    if ((out == NULL) || (numOut < (uint32_t)GB_CHANNELS)) {
        return;
    }

    if (g->instrument) {
        gb_probe_host_input(g, in, numIn, frames);
    }
    gb_bridge_render(g->bridge, out, in, (int32_t)numIn, (int32_t)frames, g->blockHostTime);
}

// ------------------------------------------------------------------------------------------------
// Notes - the DAW playing the hardware
// ------------------------------------------------------------------------------------------------

// THE CHANNEL AND THE OFFSET BOTH MATTER HERE, which is why SynthLib's events carry them: a
// multitimbral synth is the obvious thing to put behind a bridge, and a note stamped at the block
// boundary rather than at its own offset is half a buffer early, on average, every time.
static void gb_note_on(void * inst, uint8_t channel, uint8_t note, float velocity, uint32_t sampleOffset) {
    tGbPlugin * g = (tGbPlugin *)inst;

    gb_bridge_note(g->bridge, eGbNoteOn, channel, note, velocity, (int32_t)sampleOffset, g->blockHostTime);
}

static void gb_note_off(void * inst, uint8_t channel, uint8_t note, float velocity, uint32_t sampleOffset) {
    tGbPlugin * g = (tGbPlugin *)inst;

    gb_bridge_note(g->bridge, eGbNoteOff, channel, note, velocity, (int32_t)sampleOffset, g->blockHostTime);
}

static void gb_poly_pressure(void * inst, uint8_t channel, uint8_t note, float pressure,
                             uint32_t sampleOffset) {
    tGbPlugin * g = (tGbPlugin *)inst;

    gb_bridge_note(g->bridge, eGbPolyPressure, channel, note, pressure, (int32_t)sampleOffset,
                   g->blockHostTime);
}

// ------------------------------------------------------------------------------------------------
// Editor
// ------------------------------------------------------------------------------------------------

// EVERY EDIT GOES THROUGH THE HOST, never straight into the bridge: that is what puts it into the
// host's automation and its saved state, and - on VST3 - how it reaches the processor at all.
static void gb_on_edit(void * user, const tGbEditRequest * request) {
    tGbPlugin * g  = (tGbPlugin *)user;
    uint32_t    id = 0;

    switch (request->which) {
        case eGbEditDevice:       id = kParamDevice;       break;
        case eGbEditRate:         id = kParamRate;         break;
        case eGbEditFrames:       id = kParamFrames;       break;
        case eGbEditTrim:         id = kParamTrim;         break;
        case eGbEditMode:         id = kParamMode;         break;
        case eGbEditFirstChannel: id = kParamFirstChannel; break;
        case eGbEditMidiDest:     id = kParamMidiDest;     break;
        case eGbEditMidiChannel:  id = kParamMidiChannel;  break;
        case eGbEditTestNote:     id = kParamTestNote;     break;
        case eGbEditMeasure:      id = kParamMeasure;      break;
        case eGbEditOffset:       id = kParamOffsetMs;     break;
        case eGbEditSource:       id = kParamSource;       break;
        default:                  return;
    }
    synthlib_plugin_param_edited(g, id, request->normalized);

    // THE MEASURE CONTROL IS MOMENTARY, so it must return to rest - otherwise the next press produces
    // no edge at all and the button works exactly once.
    if (request->which == eGbEditMeasure) {
        synthlib_plugin_param_edited(g, id, 0.0);
    }
}

// EVERY FRAME, BEFORE ANYTHING IS DRAWN OR HIT-TESTED: this editor's instance's status slot and values
// into the draw layer, which keeps both file-scope. Pushing them only on a change let whichever of two
// open editors pushed last speak for both.
//
// THE HOST'S VALUES, not the bridge's - synthlib_plugin_param_value() answers with what the host's own
// panel shows, which on VST3 is the controller's copy and moves the moment an edit is made.
static void gb_on_sync(void * user, void * view) {
    tGbPlugin * g = (tGbPlugin *)user;

    gb_view_set_status_slot(view, (g != NULL) ? atomic_load(&g->statusSlot) : -1);

    if (g == NULL) {
        return;
    }
    gb_view_set_values(view,
                       synthlib_plugin_param_value(g, kParamDevice),
                       synthlib_plugin_param_value(g, kParamRate),
                       synthlib_plugin_param_value(g, kParamFrames),
                       synthlib_plugin_param_value(g, kParamTrim),
                       synthlib_plugin_param_value(g, kParamMode),
                       synthlib_plugin_param_value(g, kParamFirstChannel),
                       synthlib_plugin_param_value(g, kParamMidiDest),
                       synthlib_plugin_param_value(g, kParamOffsetMs),
                       synthlib_plugin_param_value(g, kParamMidiChannel),
                       synthlib_plugin_param_value(g, kParamTestNote),
                       synthlib_plugin_param_value(g, kParamSource));
}

// `inst` IS NULL only on a VST3 host that never connected processor and controller while two copies
// were loaded; the panel then draws with no figures and its clicks go nowhere, which is honest.
static void * gb_create_view(const tSynthLibPluginDesc * desc, void * inst, double width, double height) {
    return gb_view_create(width, height, gb_on_edit, gb_on_sync, inst, desc->isInstrument);
}

// THE MACHINE'S REMEMBERED WIDTH, the starting point for an editor a project has never opened - a VST3
// project remembers its own on top of this. CFPreferences because it is the one store a plug-in can
// write from inside any host, sandboxed or not.
#define GB_PREFS_APP      CFSTR("com.chrispurusha.genbridge")
#define GB_PREFS_WIDTH    CFSTR("editorWidth")

static long gb_editor_width_load(void) {
    CFPropertyListRef value = CFPreferencesCopyAppValue(GB_PREFS_WIDTH, GB_PREFS_APP);
    long              width = 0;

    if (value != NULL) {
        if (CFGetTypeID(value) == CFNumberGetTypeID()) {
            CFNumberGetValue((CFNumberRef)value, kCFNumberLongType, &width);
        }
        CFRelease(value);
    }
    return width;
}

static void gb_editor_width_save(long width) {
    CFNumberRef value = CFNumberCreate(NULL, kCFNumberLongType, &width);

    if (value != NULL) {
        CFPreferencesSetAppValue(GB_PREFS_WIDTH, value, GB_PREFS_APP);
        CFRelease(value);
    }
}

// ------------------------------------------------------------------------------------------------
// The descriptors
// ------------------------------------------------------------------------------------------------

// THE EFFECT DECLARES AN INPUT AND IGNORES IT, which is what an effect must do - and what sidesteps
// the whole class of host rejection G2 Alike ran into for having none.
static const tSynthLibBus gEffectInputs[1] = {
    { "Unused In", GB_CHANNELS, false, true }
};

// THE INSTRUMENT'S INPUT IS A SIDE-CHAIN, AUX AND NOT ACTIVE BY DEFAULT: the host's own interface
// input, for the External-Instrument mode. A MAIN input on an instrument is the shape that makes a
// host go looking for a source and complain when there is none.
static const tSynthLibBus gInstrumentInputs[1] = {
    { "Host Input", GB_CHANNELS, true, false }
};

static const tSynthLibBus gOutputs[1] = {
    { "Device Out", GB_CHANNELS, false, true }
};

#define GB_CALLBACKS                            \
    {                                           \
        .create         = gb_create,            \
        .destroy        = gb_destroy,           \
        .initialize     = gb_initialize,        \
        .terminate      = gb_terminate,         \
        .prepare        = gb_prepare,           \
        .setActive      = gb_set_active,        \
        .setProcessing  = gb_set_processing,    \
        .latencySamples = gb_latency_samples,   \
        .blockBegin     = gb_block_begin,       \
        .process        = gb_process,           \
        .noteOn         = gb_note_on,           \
        .noteOff        = gb_note_off,          \
        .polyPressure   = gb_poly_pressure,     \
        .paramCount     = gb_param_count_cb,    \
        .paramInfo      = gb_param_info_cb,     \
        .midiMapping    = gb_midi_mapping_cb,   \
        .setParam       = gb_set_param,         \
        .getParam       = gb_get_param,         \
        .paramPoints    = gb_param_points,      \
        .paramText      = gb_param_text_cb,     \
        .getState       = gb_get_state,         \
        .setState       = gb_set_state,         \
        .stateParams    = gb_state_params,      \
        .createView     = gb_create_view        \
    }

// THE PANEL IS A FIXED CANVAS THAT SIMPLY SCALES, so any size works as long as the aspect is kept;
// three quarters to twice the canvas are the bounds the old editor enforced.
static const tSynthLibPluginDesc gVariants[2] = {
    {
        .name               = "GenBridge",
        .vendor             = "Chris Purusha",
        .url                = "https://github.com/chrispurusha/GenBridge",
        .email              = "",
        .version            = GB_VERSION_STRING,

        // NoOfflineProcess AND OnlyRT: a live capture comes off a wire at one second per second, so a
        // host must neither apply it offline nor bounce it faster than realtime.
        .isInstrument       = false,
        .vst3SubCategory    = "Fx|NoOfflineProcess|OnlyRT|Tools",
        .inputs             = gEffectInputs,
        .numInputs          = 1,
        .outputs            = gOutputs,
        .numOutputs         = 1,
        .wantsMidiIn        = false,
        .wantsTransport     = false,

        .vst3ProcessorUid   = gEffectProcessorUid,
        .vst3ControllerUid  = gEffectControllerUid,
        .auType             = GB_AU_EFFECT_TYPE,
        .auSubType          = GB_AU_EFFECT_SUBTYPE,
        .auManufacturer     = GB_AU_MANUFACTURER,
        .auVersion          = GB_AU_VERSION,
        .auBundleId         = GB_AU_BUNDLE_ID,

        .editorDefaultWidth = GB_CANVAS_W,
        .editorMinWidth     = GB_CANVAS_W * 0.75,
        .editorMaxWidth     = GB_CANVAS_W * 2.0,
        .editorAspect       = GB_CANVAS_W / GB_CANVAS_H,
        .editorWidthLoad    = gb_editor_width_load,
        .editorWidthSave    = gb_editor_width_save,

        .cb                 = GB_CALLBACKS
    },
    {
        .name               = "GenBridge Instrument",
        .vendor             = "Chris Purusha",
        .url                = "https://github.com/chrispurusha/GenBridge",
        .email              = "",
        .version            = GB_VERSION_STRING,

        // Instrument|Synth is kept rather than the more literal Instrument|External: hosts have been
        // known to refuse a class they cannot read as an instrument, and Synth is known to work.
        .isInstrument       = true,
        .vst3SubCategory    = "Instrument|Synth|OnlyRT",
        .inputs             = gInstrumentInputs,
        .numInputs          = 1,
        .outputs            = gOutputs,
        .numOutputs         = 1,
        .wantsMidiIn        = true,
        .wantsTransport     = false,

        .vst3ProcessorUid   = gInstrumentProcessorUid,
        .vst3ControllerUid  = gInstrumentControllerUid,
        .auType             = GB_AU_INSTRUMENT_TYPE,
        .auSubType          = GB_AU_INSTRUMENT_SUBTYPE,
        .auManufacturer     = GB_AU_MANUFACTURER,
        .auVersion          = GB_AU_VERSION,
        .auBundleId         = GB_AU_BUNDLE_ID,

        .editorDefaultWidth = GB_CANVAS_W,
        .editorMinWidth     = GB_CANVAS_W * 0.75,
        .editorMaxWidth     = GB_CANVAS_W * 2.0,
        .editorAspect       = GB_CANVAS_W / GB_CANVAS_H,
        .editorWidthLoad    = gb_editor_width_load,
        .editorWidthSave    = gb_editor_width_save,

        .cb                 = GB_CALLBACKS
    }
};

static const tSynthLibPluginSet gSet = {
    .variants = gVariants,
    .count    = 2
};

const tSynthLibPluginSet * synthlib_plugin_variants(void) {
    return &gSet;
}
