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

// THE PLUG-IN, WITH NO VST3 IN IT.
//
// Everything GenBridge actually does is here and in gbBridge.c / gbMeasure.c / gbState.c: opening a
// device, filling a ring from it, resampling onto the host's clock, tracking drift, playing the
// hardware, measuring its round trip, remembering the settings and reporting a latency. None of
// that needs C++ and none of it needs the SDK.
//
// What a PLUG-IN FORMAT needs is not here either. SynthLib's shared wrappers (SynthLib/plugin/) are
// the VST3 and the Audio Unit, and gbPlugin.c describes this bridge to them: a block becomes the
// four calls below, a parameter is what gbParams.c says, and the saved state is a block of bytes.
// When a question is "what does this plug-in do", the answer is in these files; when it is "what does
// a format require", it is in SynthLib.
//
// The struct is OPAQUE on purpose. It carries C11 _Atomic members, which C++ cannot parse, and
// keeping the definition in gbBridgePrivate.h is what lets the C side use the right tool without
// the wrapper's compiler ever seeing it.
typedef struct tGbBridge tGbBridge;

// ── What only the wrapper can do ────────────────────────────────────────────
//
// Two things the bridge needs and cannot reach: telling the host its latency changed, and telling
// the controller a value it did not choose. Both go through the CONTROLLER - restartComponent lives
// on IComponentHandler, which a processor never sees, and the messages travel over the
// IConnectionPoint the host wires between the two ends.
//
// So the bridge posts, and the wrapper delivers. The ids are the ones the controller's notify()
// switches on: "gbStatusSlot", "gbDeviceSlot", "gbMode", "gbFirstChannel", "gbOffset", "gbLatency".
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

// ── One block, in four calls ────────────────────────────────────────────────
//
// The order is the order process() must make them in. gb_bridge_block_begin() decides where this
// block sits in wall time - which is NOT "now" on a host that hands over four blocks per audio
// callback - and everything stamped afterwards uses the answer, so it comes first and once.
uint64_t gb_bridge_block_begin(tGbBridge * self, int32_t frames);

// A parameter change, already reduced to its last point. sampleOffset places a controller inside
// the block exactly as a note is placed.
void gb_bridge_parameter(tGbBridge * self, uint32_t id, double value, int32_t sampleOffset,
                         uint64_t blockHostTime);

// THE MEASURE BUTTON IS NOT AN ORDINARY PARAMETER, and it needs both halves of what the host
// delivered. sawPress is "a press appeared anywhere in this block", because the editor raises the
// control and drops it again immediately and a host may deliver both points at once; held is what
// it SETTLED at, which is what decides whether the button is still down. Arming from the first
// latched it true and the button then worked exactly once.
void gb_bridge_measure_trigger(tGbBridge * self, bool sawPress, bool held);

typedef enum { eGbNoteOn = 0, eGbNoteOff, eGbPolyPressure } tGbNoteKind;

// A note event on its way to the hardware. Velocity and pressure arrive as VST3 does them, 0..1,
// and are converted here so the one set of rules applies wherever they come from.
void gb_bridge_note(tGbBridge * self, tGbNoteKind kind, int16_t channel, int16_t pitch,
                    float value, int32_t sampleOffset, uint64_t blockHostTime);

// Fill the host's block. Silence is a perfectly good answer and is what comes back while a device
// change is in flight.
//
// `in` IS THE HOST'S OWN INPUT, or NULL when it gave none. It is used in one mode and ignored in the
// other: capturing from a DEVICE fills the block from the ring and never looks at it, while
// capturing from the HOST passes it straight through - see GB_SOURCE_HOST. Passing it on every
// block rather than latching it keeps the decision in one place and costs a pointer.
void gb_bridge_render(tGbBridge * self, float ** out, const float * const * in, int32_t inChannels,
                      int32_t frames, uint64_t blockHostTime);

// ── The saved state ─────────────────────────────────────────────────────────
//
// The device UID is stored in the project, NOT the audio. Reopening a session should pick up
// whatever the named device is now, not a frozen copy of what it was - the same reasoning as
// G2-Edit's plug-in storing a patch PATH.
bool gb_bridge_set_state(tGbBridge * self, const char * blob, size_t length);

// The blob to write, owned by the bridge and valid until the next call to this. The wrapper reads
// the bytes into the host's stream and nothing more.
const char * gb_bridge_state(tGbBridge * self, size_t * length);

// ── What the controller needs from a blob ───────────────────────────────────
//
// A VST3 host hands the component's saved state to the CONTROLLER as well, precisely so the two can
// agree on what was loaded - and a controller that ignores it comes up showing defaults. That is
// what made two tracks, saved with a Kronos and a Helix, both reopen as Analog Keys: the UID was in
// the file, but nothing told the panel about it.
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
