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

// The VST3 wrapper. This is where the proof of concept's output device is replaced by the DAW.
//
// The substitution is smaller than it looks: process() consumes blocks on a clock that is not the
// capture device's, which is exactly what the output IOProc did. Everything underneath - the ring,
// the drift loop, the resampler - is the same code, unchanged.
//
// Built against pluginterfaces/ ONLY, following G2-Edit's do-vst3: none of the SDK's public.sdk
// helper classes are used, so there is no CMake and no vstgui anywhere in this.
//
// AN EFFECT, NOT AN INSTRUMENT. AudioMovers' Inject registers as "Fx|NoOfflineProcess|Tools" and
// that is the right call for this too. An instrument would seem more natural - the plug-in
// generates audio and consumes none - but VST3 instruments live on instrument tracks and, more
// importantly, G2-Edit had to implement IPluginFactory2 with kInstrumentSynth purely to stop hosts
// rejecting it for having no audio input bus. Declaring an effect sidesteps that entire class of
// problem: an effect HAS an input bus, so nothing is missing. The input is simply ignored.
//
// NoOfflineProcess is not decoration either. A live capture has nothing to give a faster than
// realtime render, so a host bouncing offline must not call this at all; without the flag it would
// bounce silence or garbage and look like a plug-in bug.

#include <atomic>
#include <cstdarg>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>      // lround, for the offset pushed to the controller in thousandths

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/base/ipluginbase.h"

#include "gbDraw.h"
#include "gbEditor.h"

#include "device.h"
#include "drift.h"
#include "gbMidi.h"
#include "gbStatus.h"
#include "resampler.h"
#include "ring.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

#define GB_VENDOR         "Chris Purusha"
#define GB_PLUGIN_NAME    "GenBridge"

// Set by do-vst3 from $GENBRIDGE_VERSION, which do-release drives from the git tag. The fallback is
// for anyone compiling these sources by hand; it is deliberately the same string the plist defaults
// to, so the two cannot disagree in a developer build either.
#ifndef GB_VERSION_STRING
#define GB_VERSION_STRING "0.1.0"
#endif
#define GB_CHANNELS       (2)
// THE SETPOINT IS DERIVED, NOT CHOSEN. A fixed default in milliseconds is the wrong shape for this
// number: the floor below which the ring cannot go depends on the host's block size, the device's
// block size and the rate ratio, so any constant is either needlessly large on one rig or unsafe on
// another. A per-device targetMs of 0 means "work it out", which is the default; a non-zero value
// is an explicit override and is still clamped up to the floor.
#define GB_TARGET_AUTO    (0.0)

// Headroom above the theoretical floor. The floor already covers the worst phase alignment between
// the two callbacks; this covers scheduling jitter - a device callback that runs late - which is
// not bounded by anything we control. 25% of a few hundred frames is a millisecond or two, which is
// cheap next to an audible dropout.
#define GB_AUTO_MARGIN    (1.25)

// How long to watch the host's real block size before trusting it, and how much has to be on the
// table before disturbing the host's delay compensation to claim it.
#define GB_SETTLE_SECONDS    (2.0)
#define GB_RETUNE_MIN_GAIN   (64.0)      // frames

// TEMPORARY, until the editor exists. With no way to pick a device from inside a host, a fresh
// instance would sit silent and look broken, so it falls back to this. The device parameter and any
// saved state both take precedence, so choosing anything else immediately overrides it - and the
// whole block goes when the SynthLib chooser lands.
// A 32 input interface is common - the TD-50X here is one - so the first-channel list has to reach
// that far even though most devices are stereo. Slots past the device's real channel count simply
// fail to open, which the panel shows.
#define GB_MAX_FIRST_CHANNEL  (32)
#define GB_MIDI_SLOTS         (GB_MIDI_MAX_DEST)

// The manual correction, in milliseconds, mapped onto a normalised parameter. A measurement cannot
// separate the synth's response from its patch's attack, so the number always wants a human able to
// say "that pad is not really 90 ms late".
#define GB_OFFSET_MIN_MS      (-100.0)
#define GB_OFFSET_MAX_MS      (100.0)

// How long to listen for the note before giving up, and how far above the noise floor counts as an
// onset.
#define GB_MEASURE_TIMEOUT_S  (1.5)
#define GB_MEASURE_FLOOR_S    (0.15)

// Time to let a previous note decay before listening for silence. Without it a second measurement
// starts while the first one's note is still sounding: the floor is taken from a decaying tail, or
// the tail itself trips the threshold, and the answer comes back as zero. Measuring twice in a row
// is the normal thing to do, so it has to survive it.
#define GB_MEASURE_SETTLE_S   (0.35)
// The onset threshold is RELATIVE to whatever the input is already doing, with an absolute floor
// under it. A synth with a hissy output, a hum, or a pad still decaying would sit above any fixed
// level and trip the detector the instant the note went out. Measuring the quiet first and then
// demanding a multiple of it is what makes the answer mean something.
#define GB_MEASURE_MARGIN     (0.02f)    // absolute minimum rise, for a genuinely silent input
#define GB_MEASURE_RATIO      (8.0f)     // ...or this much above the noise, whichever is greater
#define GB_MEASURE_CEILING    (0.70f)    // never demand more than this; see the note in the code
#define GB_MEASURE_CONFIRM    (2)        // consecutive blocks required, so one glitch is not an onset
#define GB_MEASURE_NOTE       (60)

// Remembered per audio device AND per MIDI destination. The same synth answers differently over USB
// than over DIN, and two different synths on one interface are not comparable at all - so the pair
// is the key, not either half of it.
#define GB_MAX_MEASURED       (32)

// measureLatency sentinels. Negative means no usable figure; they are told apart so the panel and
// the log can say WHICH kind of nothing came back, which is the difference between "the synth is on
// the wrong channel" and "the onset arrived before our own buffering could have delivered it".
#define GB_MEASURE_TIMED_OUT  (-1)
#define GB_MEASURE_TOO_EARLY  (-2)

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
// 0 means "whatever channel the note arrived on"; 1..16 force it. Source is the default so that
// adding the control changes nothing for a session that already worked - a multitimbral part sending
// on channel 5 keeps arriving on channel 5 until someone says otherwise.
#define GB_CHANNEL_SLOTS      (17)

#define GB_CC_BASE            (1000)
#define GB_CC_PER_CHANNEL     (kCountCtrlNumber)      // 128 controllers, plus aftertouch and bend
#define GB_CC_CHANNELS        (16)
#define GB_CC_COUNT           (GB_CC_PER_CHANNEL * GB_CC_CHANNELS)

// ONE ENTRY PER (AUDIO DEVICE, MIDI DESTINATION) PAIR, because that pair is what a round trip is a
// property of. Two figures, and the difference matters:
//
//   hardwareSamples  what the last measurement actually returned. A record, never edited.
//   offsetMs         the correction IN FORCE, which is what report_latency() adds. A measurement
//                    seeds it; the panel's +/- moves it from there.
//
// Splitting them is what lets the panel show "measured 4.6, using 4.8" - and it means re-measuring
// replaces the reading and the value together, while a nudge moves only the value.
typedef struct {
    char     audioUid[DEVICE_UID_LEN];
    char     midiDest[GB_MIDI_NAME_LEN];
    uint32_t hardwareSamples;    // the round trip MINUS whatever the plug-in was contributing
    double   offsetMs;           // seeded from the measurement, then adjusted by hand
} tMeasured;

#define GB_FALLBACK_DEVICE    "KRONOS"
#define GB_DEFAULT_FRAMES     (128)
#define GB_DEFAULT_RATE       (48000.0)

// Stable identity. A host remembers a plug-in by this, so it must never change once a project has
// been saved against it.
static const FUID kGenBridgeProcessorUID(0x4A1C8E52, 0x9D3B4F07, 0xA6E21B84, 0x53F0C97D);
static const FUID kGenBridgeControllerUID(0x8B70D6A1, 0x2F594C38, 0xE1A76025, 0x9C4D3B8F);

// The instrument variant. SAME CODE, registered a second time under its own identity and category -
// the audio path is identical and only the MIDI half and the bus layout differ, so two sets of
// classes would be two places to fix everything.
//
// One bundle, four classes, rather than the two bundles AudioMovers ship as Inject and Inject-MIDI.
// VST3 supports it and it halves the build, the install and the quarantine dance.
static const FUID kGenBridgeInstProcessorUID(0x6E2D4B91, 0xA07C3F58, 0x24B9E1D6, 0x8F5307CA);
static const FUID kGenBridgeInstControllerUID(0xC94A1F63, 0x5B82D70E, 0x3A6C48B1, 0xD25E9F04);

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
    kParamCount
};

// How many devices the device parameter can address. A stepped parameter needs a fixed step count
// at registration time, and the host caches it, so this cannot follow the machine's actual device
// count as it changes.
#define GB_DEVICE_SLOTS     (DEVICE_MAX)
#define GB_MAX_REMEMBERED   (32)

// Per device settings, remembered across sessions and across device changes within a session.
//
// Switching away from a device and back should not lose how it was set up - a 32 channel drum
// module and a stereo synth want completely different buffer sizes and channel pairs, and having
// to redial them every time is the sort of friction that makes a plug-in annoying rather than
// broken. So the state carries a small table keyed by device UID, not just the active device.
typedef struct {
    char     uid[DEVICE_UID_LEN];
    uint32_t frames;          // device buffer frames; 0 means "leave the device as it is"
    double   rate;            // nominal sample rate to request; 0 means "leave the device as it is"
    double   targetMs;        // ring setpoint
    uint32_t firstChannel;    // first device channel to take
    uint32_t captureChannels; // 1 for mono, 2 for a stereo pair
    float    trim;
} tDeviceSettings;

// Diagnostics, gated on a file rather than an environment variable.
//
// The obvious gate would be getenv, and it does not work: a host launched from the Dock inherits no
// shell environment, so the variable is never seen in the one situation that matters. Testing for a
// file the user can touch works from anywhere, and is the same trick the sibling projects use for
// their backdoor channels.
//
//     touch /tmp/genbridge-log        # then reload the plug-in
//     cat /tmp/genbridge.log
// EVERY LINE SAYS WHO WROTE IT. One log file is shared by every instance in every process on the
// machine - a DAW with two plug-ins in it, and a command line harness running alongside, all append
// here. Reading it without attribution means diagnosing one process's symptom from another's
// output, which is exactly what happened: a run of "measured: 0" lines was read as a harness fault
// when it came from a DAW that also had the device open.
static void log_line(const char * format, ...) {
    if (access("/tmp/genbridge-log", F_OK) != 0) {
        return;
    }

    FILE * file = fopen("/tmp/genbridge.log", "a");

    if (file == nullptr) {
        return;
    }

    static const char * name = nullptr;

    if (name == nullptr) {
        // The executable's own name, so a line from Live is distinguishable from one from the
        // checker at a glance rather than by pid alone.
        const char * path = getprogname();

        name = (path != nullptr) ? path : "?";
    }

    fprintf(file, "[%s %d] ", name, (int)getpid());

    va_list args;

    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);

    fputc('\n', file);
    fclose(file);
}

// Read a saved blob far enough to recover the ACTIVE device and its settings.
//
// The controller needs this as much as the processor does. A VST3 host saves the component's state
// and hands the same bytes to the controller through setComponentState, precisely so the two can
// agree on what was loaded - and a controller that ignores it comes up showing defaults. That is
// what made two tracks, saved with a Kronos and a Helix, both reopen as Analog Keys: the UID was in
// the file, but nothing told the panel about it.
struct tGbActive {
    std::string uid;
    std::string midiName;
    int         midiChannel{0};
    unsigned    frames{GB_DEFAULT_FRAMES};
    double      rate{GB_DEFAULT_RATE};
    unsigned    firstChannel{0};
    unsigned    channels{GB_CHANNELS};
    float       trim{1.0f};
    double      offsetMs{0.0};
    bool        valid{false};
};

static tGbActive gb_parse_active(const std::string & blob) {
    tGbActive out;
    int       version = 0;

    if (blob.compare(0, 10, "GENBRIDGE3") == 0) {
        version = 3;
    } else if (blob.compare(0, 10, "GENBRIDGE2") == 0) {
        version = 2;
    } else if (blob.compare(0, 10, "GENBRIDGE1") == 0) {
        version = 1;
    } else {
        return out;
    }

    size_t                   pos = 0;
    std::vector<std::string> hwLines;

    while (pos < blob.size()) {
        size_t      end  = blob.find('\n', pos);
        std::string line = blob.substr(pos, (end == std::string::npos) ? std::string::npos : end - pos);

        pos = (end == std::string::npos) ? blob.size() : end + 1;

        if (line.compare(0, 5, "midi=") == 0) {
            out.midiName = line.substr(5);
        } else if (line.compare(0, 7, "midich=") == 0) {
            out.midiChannel = atoi(line.substr(7).c_str());
        } else if (line.compare(0, 3, "hw=") == 0) {
            // Held, not applied. A hw= line names its own pair, and which pair is the ACTIVE one is
            // not known until active= and midi= have both been seen - and getState writes them
            // first only by convention, which is a thin thing to parse by.
            hwLines.push_back(line.substr(3));
        } else if (line.compare(0, 7, "active=") == 0) {
            out.uid   = line.substr(7);
            out.valid = !out.uid.empty();
        } else if ((line.compare(0, 4, "dev=") == 0) && !out.uid.empty()) {
            std::string body = line.substr(4);

            if (body.size() > out.uid.size()
                && body.compare(body.size() - out.uid.size(), out.uid.size(), out.uid) == 0) {
                double   values[6] = { GB_DEFAULT_FRAMES, GB_DEFAULT_RATE, 0.0, 0.0, GB_CHANNELS, 1.0 };
                int      numeric   = (version >= 3) ? 6 : ((version == 2) ? 5 : 4);
                size_t   at        = 0;

                for (int field = 0; field < numeric; field++) {
                    size_t comma = body.find(',', at);

                    if (comma == std::string::npos) {
                        break;
                    }

                    double v = strtod(body.substr(at, comma - at).c_str(), nullptr);

                    // Same slot mapping as the processor's own parser - v1 has no rate, v2 no
                    // channel count.
                    int slot = field;

                    if (version == 1) {
                        static const int kV1[] = { 0, 2, 3, 5 };
                        slot = kV1[field];
                    } else if (version == 2) {
                        static const int kV2[] = { 0, 1, 2, 3, 5 };
                        slot = kV2[field];
                    }

                    values[slot] = v;
                    at           = comma + 1;
                }

                out.frames       = (unsigned)values[0];
                out.rate         = values[1];
                out.firstChannel = (unsigned)values[3];
                out.channels     = ((values[4] == 1.0) || (values[4] == 2.0)) ? (unsigned)values[4] : GB_CHANNELS;
                out.trim         = (float)values[5];
            }
        }
    }

    // The correction for the pair this instance is actually on. Same length-prefixed layout the
    // processor writes - see parse_measured_line() for why the destination is counted, not split.
    for (const std::string & body : hwLines) {
        size_t at = 0;
        double fields[3];
        bool   ok = true;

        for (int i = 0; (i < 3) && ok; i++) {
            size_t comma = body.find(',', at);

            if (comma == std::string::npos) {
                ok = false;
                break;
            }
            fields[i] = strtod(body.substr(at, comma - at).c_str(), nullptr);
            at        = comma + 1;
        }

        if (!ok) {
            continue;
        }

        size_t      destLen = (size_t)fields[2];
        std::string tail    = body.substr(at);

        if (destLen > tail.size()) {
            continue;
        }

        if ((tail.substr(0, destLen) == out.midiName) && (tail.substr(destLen) == out.uid)) {
            out.offsetMs = fields[1];
            break;
        }
    }

    return out;
}

// ------------------------------------------------------------------------------------------------
// The processor.
// ------------------------------------------------------------------------------------------------

class GenBridgePlugin : public IComponent, public IAudioProcessor, public IConnectionPoint {
public:
    explicit GenBridgePlugin(bool instrumentIn) : refCount(1), instrument(instrumentIn) {
        statusSlot = gb_status_claim();

        if (instrument) {
            gb_midi_init();
        }
        memset(&ring, 0, sizeof(ring));
        memset(&resampler, 0, sizeof(resampler));
        memset(&drift, 0, sizeof(drift));
        memset(&capture, 0, sizeof(capture));

        pthread_mutex_init(&configLock, nullptr);
        pthread_mutex_init(&wakeMutex, nullptr);
        pthread_cond_init(&wakeCond, nullptr);
    }

    virtual ~GenBridgePlugin(void) {
        stop_worker();
        close_capture();
        gb_status_release(statusSlot);

        pthread_cond_destroy(&wakeCond);
        pthread_mutex_destroy(&wakeMutex);
        pthread_mutex_destroy(&configLock);
    }

    // ---- FUnknown ----

    tresult PLUGIN_API queryInterface(const TUID iid, void ** obj) SMTG_OVERRIDE {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IComponent)
        QUERY_INTERFACE(iid, obj, IPluginBase::iid, IComponent)
        QUERY_INTERFACE(iid, obj, IComponent::iid, IComponent)
        QUERY_INTERFACE(iid, obj, IAudioProcessor::iid, IAudioProcessor)
        QUERY_INTERFACE(iid, obj, IConnectionPoint::iid, IConnectionPoint)
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef(void) SMTG_OVERRIDE { return (uint32)++refCount; }

    uint32 PLUGIN_API release(void) SMTG_OVERRIDE {
        int32 c = --refCount;

        if (c == 0) {
            delete this;
            return 0;
        }
        return (uint32)c;
    }

    // ---- IPluginBase ----

    tresult PLUGIN_API initialize(FUnknown * context) SMTG_OVERRIDE {
        // The host application is the only thing that can make an IMessage, so it has to be kept.
        if (context != nullptr) {
            context->queryInterface(IHostApplication::iid, (void **)&host);
        }

        return kResultOk;
    }

    tresult PLUGIN_API terminate(void) SMTG_OVERRIDE {
        close_capture();

        if (host != nullptr) {
            host->release();
            host = nullptr;
        }

        return kResultOk;
    }

    // ---- IConnectionPoint ----
    //
    // The host connects processor and controller to each other and this is the only channel between
    // them. All that travels over it is the status slot number, sent once: the controller then reads
    // the meters and drift figures straight out of that slot, rather than a message per frame.

    tresult PLUGIN_API connect(IConnectionPoint * other) SMTG_OVERRIDE {
        peer = other;
        send_slot();
        log_line("connected to controller, published status slot %d", statusSlot);
        return kResultOk;
    }

    tresult PLUGIN_API disconnect(IConnectionPoint * other) SMTG_OVERRIDE {
        (void)other;
        peer = nullptr;
        return kResultOk;
    }

    tresult PLUGIN_API notify(IMessage * message) SMTG_OVERRIDE {
        (void)message;
        return kResultOk;
    }

    // ---- IComponent ----

    tresult PLUGIN_API getControllerClassId(TUID classId) SMTG_OVERRIDE {
        memcpy(classId,
               instrument ? kGenBridgeInstControllerUID.toTUID() : kGenBridgeControllerUID.toTUID(),
               sizeof(TUID));
        return kResultOk;
    }

    tresult PLUGIN_API setIoMode(IoMode mode) SMTG_OVERRIDE {
        (void)mode;
        return kResultOk;
    }

    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) SMTG_OVERRIDE {
        if (type == kAudio) {
            // An INSTRUMENT has no audio input, and that is allowed here only because
            // IPluginFactory2 declares the subcategory - the base interface reports a bare "Audio
            // Module Class", a host assumes effect, looks for the input an effect must have, and
            // refuses to load. That is the trap G2-Edit fell into.
            //
            // The effect variant declares one and ignores it, which is what an effect must do.
            if ((dir == kInput) && instrument) {
                return 0;
            }

            return 1;
        }

        if ((type == kEvent) && (dir == kInput)) {
            return instrument ? 1 : 0;      // the notes the host plays the hardware with
        }

        return 0;
    }

    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo & info) SMTG_OVERRIDE {
        if (index != 0) {
            return kInvalidArgument;
        }

        if ((type == kEvent) && (dir == kInput) && instrument) {
            info.mediaType    = kEvent;
            info.direction    = kInput;
            info.channelCount = 16;         // the MIDI channels
            info.busType      = kMain;
            info.flags        = BusInfo::kDefaultActive;

            name_to_utf16("MIDI In", info.name, 128);

            return kResultOk;
        }

        if (type != kAudio) {
            return kInvalidArgument;
        }

        if ((dir == kInput) && instrument) {
            return kInvalidArgument;
        }

        info.mediaType    = kAudio;
        info.direction    = dir;
        info.channelCount = GB_CHANNELS;
        info.busType      = kMain;
        info.flags        = BusInfo::kDefaultActive;

        name_to_utf16((dir == kInput) ? "Unused In" : "Device Out", info.name, 128);

        return kResultOk;
    }

    tresult PLUGIN_API getRoutingInfo(RoutingInfo & inInfo, RoutingInfo & outInfo) SMTG_OVERRIDE {
        (void)inInfo; (void)outInfo;
        return kNotImplemented;
    }

    tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index, TBool state) SMTG_OVERRIDE {
        (void)type; (void)dir; (void)index; (void)state;
        return kResultOk;
    }

    // ACTIVATION OPENS THE DEVICE SYNCHRONOUSLY, and that is the whole reason the host sees a
    // sensible latency figure.
    //
    // A host asks getLatencySamples() shortly after activating a plug-in and then caches the
    // answer; it only asks again if told to, via IComponentHandler::restartComponent. Opening the
    // device on the worker meant latency was still 0 when Ableton asked, and it reported zero
    // latency for ever after - while a test harness that polls until it settles saw the real 2228
    // and looked perfectly healthy. Both were right, which is what made it worth writing down.
    //
    // setActive is not the audio thread, and it is where a plug-in is expected to do its expensive
    // set-up, so a blocking device open belongs here. The worker stays for CHANGES made while
    // running, which is where doing it asynchronously actually matters.
    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE {
        if (state) {
            start_worker();
            reconfigure();          // synchronous: latency must be known before the host asks
        } else {
            stop_worker();
            close_capture();
        }

        return kResultOk;
    }

    // The device UID is stored in the project, NOT the audio. That is deliberate, and it is the
    // same reasoning as G2-Edit's plug-in storing a patch PATH: reopening a session should pick up
    // whatever the named device is now, not a frozen copy of what it was.
    //
    // THE FORMAT IS VERSIONED AND LINE BASED, and it is that way now rather than later because a
    // state format becomes expensive to change the moment anyone saves a session against it. Text
    // costs nothing at this size, survives being looked at in a hex editor, and lets an older
    // build skip keys it does not recognise instead of rejecting the whole blob.
    //
    // The UID is written LAST on each line and read as "everything after the fifth comma", because
    // real UIDs contain commas - "AppleUSBAudioEngine:CalDigit, Inc.:..." - and splitting on them
    // would truncate it.
    tresult PLUGIN_API setState(IBStream * state) SMTG_OVERRIDE {
        if (state == nullptr) {
            return kResultFalse;
        }

        std::string blob;
        char        chunk[1024];
        int32       read = 0;

        while ((state->read(chunk, (int32)sizeof(chunk), &read) == kResultOk) && (read > 0)) {
            blob.append(chunk, (size_t)read);

            if (read < (int32)sizeof(chunk)) {
                break;
            }
        }

        return parse_state(blob) ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API getState(IBStream * state) SMTG_OVERRIDE {
        if (state == nullptr) {
            return kResultFalse;
        }

        capture_live_settings();

        std::string blob = "GENBRIDGE3\n";

        blob += "active=" + deviceSelector + "\n";

        // BY NAME, not by index. The MIDI list shifts whenever a device is powered on or off, so an
        // index saved on Monday names something else on Tuesday - the same reasoning that keeps the
        // audio device stored as a UID. Ableton was not forgetting the destination; nothing was ever
        // writing it down.
        if (instrument) {
            blob += "midi=" + std::string(midiName) + "\n";
            blob += "midich=" + std::to_string(midiChannel.load()) + "\n";

            // THE MANUAL TRIM, AND THE MEASUREMENTS IT TRIMS. Neither belongs on a dev= line: the
            // offset is one value for the whole plug-in, and a measurement is keyed by the audio
            // device and the MIDI destination TOGETHER - a pair no single dev= line names.
            //
            // Without both of these the feature came apart on reload, and quietly. report_latency()
            // adds the measured hardware share and then the offset on top of it, so a session
            // reopened with the pair missing reported a latency short by the entire round trip,
            // with the trim someone had dialled in by ear silently back at zero. Restoring the
            // offset alone would be worse than neither: it would trim a base that was not there.
            // The live value belongs to a pair like every other, so fold it in before writing -
            // otherwise a nudge made since the last device change would not be in the table yet.
            sync_offset_to_pair();

            char line[64 + DEVICE_UID_LEN + GB_MIDI_NAME_LEN];

            for (uint32_t i = 0; i < measuredCount; i++) {
                const tMeasured * m = &measured[i];

                snprintf(line, sizeof(line), "hw=%u,%.3f,%u,%s%s\n",
                         m->hardwareSamples, m->offsetMs,
                         (unsigned)strlen(m->midiDest), m->midiDest, m->audioUid);
                blob += line;
            }
        }

        for (uint32_t i = 0; i < rememberedCount; i++) {
            const tDeviceSettings * d = &remembered[i];
            char                    line[512];

            snprintf(line, sizeof(line), "dev=%u,%.1f,%.3f,%u,%u,%.4f,%s\n",
                     d->frames, d->rate, d->targetMs, d->firstChannel, d->captureChannels,
                     (double)d->trim, d->uid);
            blob += line;
        }

        int32 written = 0;

        return state->write((void *)blob.data(), (int32)blob.size(), &written);
    }

    // ---- IAudioProcessor ----

    tresult PLUGIN_API setBusArrangements(SpeakerArrangement * inputs, int32 numIns,
                                          SpeakerArrangement * outputs, int32 numOuts) SMTG_OVERRIDE {
        (void)inputs;

        int32 wantIns = instrument ? 0 : 1;

        if ((numIns == wantIns) && (numOuts == 1) && (outputs[0] == SpeakerArr::kStereo)) {
            return kResultOk;
        }
        return kResultFalse;
    }

    tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement & arr) SMTG_OVERRIDE {
        (void)dir;

        if (index == 0) {
            arr = SpeakerArr::kStereo;
            return kResultOk;
        }
        return kInvalidArgument;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSize) SMTG_OVERRIDE {
        return (symbolicSize == kSample32) ? kResultTrue : kResultFalse;
    }

    uint32 PLUGIN_API getTailSamples(void) SMTG_OVERRIDE {
        return 0;
    }

    // Everything between the device's converters and this plug-in's output, so the host can line
    // the track up against the rest of the session. Getting this wrong is the kind of bug people
    // live with for months without noticing: the audio is simply, quietly, in the wrong place.
    uint32 PLUGIN_API getLatencySamples(void) SMTG_OVERRIDE {
        return running ? report_latency() : 0;
    }

    // WHAT THE PLUG-IN ITSELF ADDS, with no hardware correction in it. Split out of
    // report_latency() because the measurement has to subtract our share from the onset it sees,
    // and subtracting the REPORTED figure meant subtracting the previous measurement along with it:
    // every re-measure came back short by whatever correction was already in force, so the value
    // walked towards zero the more times it was run. Only ever grows out of the ring and the
    // converters, so it is the honest thing to net off.
    double internal_latency_frames(void) const {
        double inputFrames = setpointFrames + resampler_latency_frames() + (double)deviceLatency;

        // Reported in the HOST's frames, and the ring is measured in the device's.
        return inputFrames / nominalRatio;
    }

    uint32 internal_latency(void) const {
        double total = internal_latency_frames();

        return (total > 0.0) ? (uint32)total : 0;
    }

    uint32 report_latency(void) const {
        double total = internal_latency_frames();

        // THE HARDWARE'S SHARE IS ADDED FOR THE INSTRUMENT, because that is what makes a recorded
        // part land on the beat. The host delays everything else to match, which is precisely the
        // job an External Instrument device does in Live with its Hardware Latency field.
        //
        // Not for the effect: nothing is being played through it, so there is no round trip to
        // compensate and inflating its latency would only push a live input further out of place.
        //
        // ONE TERM, NOT TWO. This used to add hardwareSamples and then offsetMs on top of it, which
        // made the panel incoherent: Measure wrote a figure you could not touch, beside a trim that
        // started at zero and existed only to correct it. The measurement now lands IN offsetMs, so
        // what is added is simply the correction in force - and adding hardwareSamples as well here
        // would count the round trip twice.
        if (instrument) {
            total += (offsetMs.load() / 1000.0) * hostRate;
        }

        return (total > 0.0) ? (uint32)total : 0;
    }

    // WHETHER THE HOST INTENDS TO RUN US FASTER THAN REALTIME, which it tells us here and nowhere
    // else. There is no reciprocal call - a plug-in cannot demand realtime, it can only declare
    // OnlyRT in its class subcategories (see the factory) and find out here whether that was
    // honoured. Logged for exactly that reason: it is the only evidence of what a host decided.
    //
    // A bounce in kOffline cannot work. The ring is filled by a device running at one second per
    // second, so a host consuming it faster simply drains it, and the render comes out silent or in
    // pieces. Nothing in here can fix that; the flag exists so the panel can say so afterwards
    // rather than leaving a silent bounce to be puzzled over.
    tresult PLUGIN_API setupProcessing(ProcessSetup & setup) SMTG_OVERRIDE {
        hostRate      = setup.sampleRate;
        hostMaxFrames = (uint32)setup.maxSamplesPerBlock;

        bool offline  = (setup.processMode == kOffline);

        if (offline != offlineRender.load()) {
            log_line("host set process mode %d (%s)%s", (int)setup.processMode,
                     (setup.processMode == kRealtime) ? "realtime"
                     : ((setup.processMode == kPrefetch) ? "prefetch" : "OFFLINE"),
                     offline ? " - a bounce in this mode captures nothing, the device runs in real time"
                             : "");
        }

        offlineRender.store(offline);

        tGbStatus * status = gb_status(statusSlot);

        if (status != nullptr) {
            atomic_store(&status->offlineRender, offline ? 1 : 0);
        }

        return kResultOk;
    }

    tresult PLUGIN_API setProcessing(TBool state) SMTG_OVERRIDE {
        (void)state;
        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData & data) SMTG_OVERRIDE {
        if ((data.numOutputs < 1) || (data.outputs[0].numChannels < GB_CHANNELS)) {
            return kResultOk;
        }

        float ** out    = data.outputs[0].channelBuffers32;
        int32    frames = data.numSamples;

        if (frames <= 0) {
            return kResultOk;
        }

        apply_parameter_changes(data);

        if (instrument) {
            forward_events(data);
        }

        // TRYLOCK, NEVER LOCK. The worker holds this while it tears down and rebuilds the ring,
        // the resampler and the device - during which none of them may be touched. Blocking here
        // would stall the host's audio thread on a CoreAudio device open, which is exactly the
        // kind of thing that makes a DAW drop out. Failing to acquire it means a device change is
        // in flight, and a block of silence is the right answer.
        if (pthread_mutex_trylock(&configLock) != 0) {
            silence(out, frames);
            return kResultOk;
        }

        if (!running) {
            silence(out, frames);
            pthread_mutex_unlock(&configLock);
            return kResultOk;
        }

        // Same start-up and recovery rule as the command line bridge: hold silence until there is
        // a setpoint's worth to snap to, then resync so the loop opens with zero error.
        if (needResync.load()) {
            if (ring_fill(&ring) < (uint64_t)setpointFrames) {
                silence(out, frames);
                pthread_mutex_unlock(&configLock);
                return kResultOk;
            }

            ring_resync(&ring, (uint32_t)setpointFrames);
            resampler_reset(&resampler);
            drift_reset(&drift);
            needResync.store(false);

            // The first one is the prime, not a fault. Anything after it means the loop lost the
            // buffer and had to be rescued, which is exactly what should never happen.
            if (primed) {
                resyncs.fetch_add(1);
            }

            primed = true;
        }

        observe_block(frames);

        double fill       = (double)ring_fill(&ring);
        double interval   = (double)frames / hostRate;
        double correction = drift_update(&drift, fill, interval);
        double ratio      = nominalRatio * (1.0 + correction);

        uint32_t needed = resampler_needed(&resampler, (uint32_t)frames, ratio);

        if (needed > pullCapacity) {
            needed = pullCapacity;
        }

        if (needed > 0) {
            if (!ring_read(&ring, pullBuffer, needed)) {
                needResync.store(true);
            }

            resampler_push(&resampler, pullBuffer, needed);
        }

        resampler_process(&resampler, interleaved, (uint32_t)frames, ratio);

        float trim = trimGain.load();

        for (int32 i = 0; i < frames; i++) {
            for (int c = 0; c < GB_CHANNELS; c++) {
                out[c][i] = interleaved[(i * GB_CHANNELS) + c] * trim;
            }
        }

        run_measurement(out, frames);
        publish_status(out, frames, fill);

        pthread_mutex_unlock(&configLock);

        return kResultOk;
    }

    // ---- latency measurement -------------------------------------------------------------------
    //
    // Play a note, time how long until anything comes back, and remember it. The whole point is that
    // a DAW cannot compensate for a delay it does not know about: without this, a part played
    // through the instrument records roughly ninety milliseconds behind the beat on the rig this
    // was built against, and no amount of buffer tuning touches it because most of it is the
    // hardware.
    //
    // WHAT IS STORED IS THE HARDWARE'S SHARE, NOT THE TOTAL, and that distinction is what makes it
    // correct under a host. The measured onset includes the plug-in's own path, which the host is
    // ALREADY compensating for because getLatencySamples() reported it. Storing the total and then
    // reporting it would count our part twice - and worse, the stored figure would silently go
    // wrong the moment the ring was retuned. Subtracting our contribution at the moment of
    // measurement leaves a number that is purely the synth and the wire, which stays true whatever
    // the buffer does afterwards.
    //
    // IT CANNOT SEPARATE THE SYNTH FROM ITS PATCH. A slow pad crosses the threshold later than a
    // piano, and nothing measuring from outside can tell the difference. Hence the manual offset:
    // the measurement gets you within a few milliseconds and a person settles the rest.
    void start_measurement(void) {
        if (!instrument || !running) {
            return;
        }

        // ALL NOTES OFF FIRST, on every channel we might have used. Belt and braces: our own test
        // note is released explicitly, and anything left hanging by a previous attempt - or by the
        // user playing - goes with it.
        for (uint8_t channel = 0; channel < 16; channel++) {
            uint8_t note[3]  = { (uint8_t)(0x80 | channel), GB_MEASURE_NOTE, 0 };
            uint8_t panic[3] = { (uint8_t)(0xB0 | channel), 123, 0 };   // All Notes Off

            gb_midi_send(midiDestination.load(), note, 3);
            gb_midi_send(midiDestination.load(), panic, 3);
        }

        measureUnderrunsAtStart = atomic_load(&ring.underflows);
        measureResyncsAtStart    = resyncs.load();

        measureState       = eMeasureSettle;
        measureFrames      = 0;
        measurePeak        = 0.0f;
        measureFloor       = 0.0f;
        measureConfirm     = 0;
        measureOnsetFrames = 0;
        measureLatency.store(0);
    }

    void run_measurement(float ** out, int32 frames) {
        if (measureState == eMeasureIdle) {
            return;
        }

        float peak = 0.0f;

        for (int32 i = 0; i < frames; i++) {
            for (int c = 0; c < GB_CHANNELS; c++) {
                float magnitude = (out[c][i] < 0.0f) ? -out[c][i] : out[c][i];

                if (magnitude > peak) {
                    peak = magnitude;
                }
            }
        }

        measureFrames += (uint32_t)frames;

        if (measureState == eMeasureSettle) {
            if ((double)measureFrames >= (GB_MEASURE_SETTLE_S * hostRate)) {
                measureState  = eMeasureFloor;
                measureFrames = 0;
                measureFloor  = 0.0f;
            }

            return;
        }

        if (measureState == eMeasureFloor) {
            // Establish what silence looks like on this input before deciding what a note looks
            // like. A noisy preamp would otherwise register an onset immediately.
            if (peak > measureFloor) {
                measureFloor = peak;
            }

            if ((double)measureFrames >= (GB_MEASURE_FLOOR_S * hostRate)) {
                uint8_t note[3] = { 0x90, GB_MEASURE_NOTE, 100 };

                gb_midi_send(midiDestination.load(), note, 3);

                measureState  = eMeasureListening;
                measureFrames = 0;
            }

            return;
        }

        // Listening.
        float threshold = measureFloor * GB_MEASURE_RATIO;

        if (threshold < GB_MEASURE_MARGIN) {
            threshold = GB_MEASURE_MARGIN;
        }

        // A very loud input would otherwise set a threshold no note could reach, and the
        // measurement would time out reporting "nothing came back" when the truth is "this input is
        // too noisy to measure". The ceiling turns that into a detection that at least tries, and
        // the logged floor tells the story afterwards.
        if (threshold > GB_MEASURE_CEILING) {
            threshold = GB_MEASURE_CEILING;
        }

        if (peak > threshold) {
            measureConfirm++;

            // The FIRST block that crossed is the onset; the confirmation only decides whether to
            // believe it. Counting from the confirming block instead would add its duration to
            // every measurement.
            if (measureConfirm == 1) {
                measureOnsetFrames = measureFrames;
            }
        } else {
            measureConfirm = 0;
        }

        if (measureConfirm >= GB_MEASURE_CONFIRM) {
            uint8_t off[3] = { 0x80, GB_MEASURE_NOTE, 0 };

            gb_midi_send(midiDestination.load(), off, 3);

            // Our own share is subtracted here, while it is unambiguously the share that was in
            // force during the measurement.
            uint32_t total = measureOnsetFrames;
            // internal_latency(), NOT report_latency() - see the comment on that pair. Netting off
            // the reported figure would subtract the correction already in force as well as our own
            // buffering, so each run would return less than the last.
            uint32_t ours   = internal_latency();

            // A ROUND TRIP CANNOT BE FASTER THAN OUR OWN PIPELINE. If it looks like it was, the
            // onset is not the note - a threshold crossing on something else, or a ring that was
            // not at its setpoint when the arithmetic assumed it was. Reported rather than clamped
            // to zero: a silent 0 looked exactly like a device that had never been measured.
            int      theirs = (total > ours) ? (int)(total - ours) : GB_MEASURE_TOO_EARLY;

            measureOnset.store((int)total);
            measureOurs.store((int)ours);
            measureTriggerPeak.store(peak);
            measureFloorSeen.store(measureFloor);

            measureLatency.store(theirs);
            measureState = eMeasureIdle;
            measureStore.store(true);
            wake_worker();
            return;
        }

        if ((double)measureFrames >= (GB_MEASURE_TIMEOUT_S * hostRate)) {
            uint8_t off[3] = { 0x80, GB_MEASURE_NOTE, 0 };

            gb_midi_send(midiDestination.load(), off, 3);

            measureLatency.store(GB_MEASURE_TIMED_OUT);        // nothing came back
            measureState = eMeasureIdle;
            measureStore.store(true);
            wake_worker();
        }
    }

    // WATCH THE BLOCK SIZE THE HOST ACTUALLY USES, which need not be the one it declared.
    //
    // The ring's floor is computed from maxSamplesPerBlock, because that is all a plug-in is told
    // before it has to size anything. Ableton declares 512 and then calls with 128 - so the floor
    // came out at 768 frames where 384 would do, and the plug-in reported 26 ms of latency for a
    // job that needs 16.
    //
    // So: watch for a couple of seconds, take the largest block actually seen, and if that leaves a
    // worthwhile amount on the table, ask the worker to retune. This runs on the audio thread and
    // therefore only ever raises a flag - the setpoint change and the message to the host both
    // allocate, and neither belongs here.
    //
    // ONCE ONLY, and never upward. A latency change makes the host redo its delay compensation, so
    // doing it repeatedly would be worse than the latency it saves. If the gamble is wrong - a
    // larger block arrives later and underruns - the safety net below puts the conservative floor
    // back for the rest of the session and stops trying.
    // THE DAW PLAYS THE HARDWARE. Note data arrives as VST3 events and leaves as MIDI bytes on a
    // CoreMIDI destination; the audio comes back through the same capture path the effect uses.
    // That round trip is what makes this an instrument rather than a recorder.
    //
    // Sent straight from process(), not queued. MIDISend is not strictly real-time safe, but the
    // alternative - handing the bytes to another thread - adds exactly the jitter that makes a
    // hardware synth feel loose, and every plug-in that drives external gear makes the same trade.
    //
    // Continuous controllers, pitch bend and aftertouch do NOT arrive here: VST3 delivers those as
    // parameter changes via IMidiMapping, which is a separate piece of work and is why G2-Edit's
    // controller implements it. Notes first.
    // 0 keeps whatever the host sent; anything else forces the channel.
    uint8_t channel_for(int16 incoming) const {
        int forced = midiChannel.load();

        return (forced <= 0) ? (uint8_t)(incoming & 0x0F) : (uint8_t)((forced - 1) & 0x0F);
    }

    void forward_events(ProcessData & data) {
        if (data.inputEvents == nullptr) {
            return;
        }

        int32 destination = midiDestination.load();
        int32 count       = data.inputEvents->getEventCount();

        for (int32 i = 0; i < count; i++) {
            Event   event;
            uint8_t message[3];

            if (data.inputEvents->getEvent(i, event) != kResultOk) {
                continue;
            }

            if (event.type == Event::kNoteOnEvent) {
                int velocity = (int)((event.noteOn.velocity * 127.0f) + 0.5f);

                // A note-on at zero velocity is a note-off on the wire, and some devices treat the
                // two differently. Sending a real note-off is the safer of the two.
                if (velocity <= 0) {
                    message[0] = (uint8_t)(0x80 | channel_for(event.noteOn.channel));
                    message[1] = (uint8_t)(event.noteOn.pitch & 0x7F);
                    message[2] = 64;
                } else {
                    message[0] = (uint8_t)(0x90 | channel_for(event.noteOn.channel));
                    message[1] = (uint8_t)(event.noteOn.pitch & 0x7F);
                    message[2] = (uint8_t)((velocity > 127) ? 127 : velocity);
                }
            } else if (event.type == Event::kNoteOffEvent) {
                int velocity = (int)((event.noteOff.velocity * 127.0f) + 0.5f);

                message[0] = (uint8_t)(0x80 | channel_for(event.noteOff.channel));
                message[1] = (uint8_t)(event.noteOff.pitch & 0x7F);
                message[2] = (uint8_t)((velocity < 0) ? 0 : ((velocity > 127) ? 127 : velocity));
            } else if (event.type == Event::kPolyPressureEvent) {
                message[0] = (uint8_t)(0xA0 | channel_for(event.polyPressure.channel));
                message[1] = (uint8_t)(event.polyPressure.pitch & 0x7F);
                message[2] = (uint8_t)((event.polyPressure.pressure * 127.0f) + 0.5f);
            } else {
                continue;
            }

            gb_midi_send(destination, message, 3);
        }
    }

    // Back from a normalised parameter to the wire.
    void send_controller(ParamID id, ParamValue value) {
        uint32_t index      = (uint32_t)(id - GB_CC_BASE);
        uint8_t  channel    = channel_for((int16)((index / GB_CC_PER_CHANNEL) & 0x0F));
        uint32_t controller = index % GB_CC_PER_CHANNEL;
        int      destination = midiDestination.load();
        uint8_t  message[3];

        if (value < 0.0) {
            value = 0.0;
        } else if (value > 1.0) {
            value = 1.0;
        }

        if (controller == kAfterTouch) {
            // Channel pressure is two bytes, not three.
            message[0] = (uint8_t)(0xD0 | channel);
            message[1] = (uint8_t)((value * 127.0) + 0.5);

            gb_midi_send(destination, message, 2);
            return;
        }

        if (controller == kPitchBend) {
            // FOURTEEN BITS, centred at 8192 - the one controller that is not a 0..127 byte, and
            // the one a keyboard player notices immediately if it is wrong.
            int bend = (int)((value * 16383.0) + 0.5);

            message[0] = (uint8_t)(0xE0 | channel);
            message[1] = (uint8_t)(bend & 0x7F);
            message[2] = (uint8_t)((bend >> 7) & 0x7F);

            gb_midi_send(destination, message, 3);
            return;
        }

        if (controller > 127) {
            return;
        }

        message[0] = (uint8_t)(0xB0 | channel);
        message[1] = (uint8_t)controller;
        message[2] = (uint8_t)((value * 127.0) + 0.5);

        gb_midi_send(destination, message, 3);
    }

    void observe_block(int32 frames) {
        if (retuneState != eRetuneWatching) {
            return;
        }

        if ((uint32_t)frames > observedMaxFrames) {
            observedMaxFrames = (uint32_t)frames;
        }

        observedFrames += (uint64_t)frames;

        if ((double)observedFrames < (GB_SETTLE_SECONDS * hostRate)) {
            return;
        }

        double candidate = minimum_setpoint_for(observedMaxFrames) * GB_AUTO_MARGIN;

        // Nothing to gain if the host is using what it declared.
        if (observedMaxFrames >= hostMaxFrames) {
            retuneState = eRetuneSettled;
            return;
        }

        if ((setpointFrames - candidate) >= GB_RETUNE_MIN_GAIN) {
            retuneState = eRetuneRequested;
            wake_worker();
        } else {
            retuneState = eRetuneSettled;
        }
    }

    // Peak-hold with a slow decay, which is what makes a meter readable: a true instantaneous peak
    // on a 30 Hz repaint shows almost nothing of a transient.
    void publish_status(float ** out, int32 frames, double fill) {
        tGbStatus * status = gb_status(statusSlot);

        if (status == nullptr) {
            return;
        }
        float       peak[GB_CHANNELS] = { 0.0f, 0.0f };

        for (int c = 0; c < GB_CHANNELS; c++) {
            for (int32 i = 0; i < frames; i++) {
                float magnitude = (out[c][i] < 0.0f) ? -out[c][i] : out[c][i];

                if (magnitude > peak[c]) {
                    peak[c] = magnitude;
                }
            }
        }

        float heldLeft  = atomic_load(&status->peakLeft) * 0.85f;
        float heldRight = atomic_load(&status->peakRight) * 0.85f;

        atomic_store(&status->peakLeft, (peak[0] > heldLeft) ? peak[0] : heldLeft);
        atomic_store(&status->peakRight, (peak[1] > heldRight) ? peak[1] : heldRight);

        atomic_store(&status->fillFrames, fill);
        atomic_store(&status->driftPpm, drift_measured_ppm(&drift));
        atomic_store(&status->underruns, (int)atomic_load(&ring.underflows));
        atomic_store(&status->resyncs, (int)resyncs.load());
    }

private:
    // TELLING THE HOST ITS LATENCY CHANGED, which it will not ask about on its own.
    //
    // A host reads getLatencySamples() once, shortly after activation, and caches it until told
    // otherwise. Now that a fresh instance opens nothing, that reading is always zero - so
    // selecting a device later left Ableton compensating for nothing at all, and every track fed by
    // the plug-in sat late by the whole buffer.
    //
    // Only the CONTROLLER can say so: restartComponent lives on IComponentHandler, which the
    // processor never sees. So it goes over the same connection the status slot does.
    void send_latency_changed(void) {
        send_message("gbLatency", 0);
    }

    void send_slot(void) {
        send_message("gbStatusSlot", statusSlot);
    }

    void send_message(const char * id, int value) {
        if ((peer == nullptr) || (host == nullptr)) {
            return;
        }

        IMessage * message = nullptr;
        TUID       messageIid;

        // createInstance takes TUIDs (raw 16-byte arrays) while the interface exposes an FUID, so
        // it has to be copied out rather than passed straight through.
        memcpy(messageIid, IMessage::iid.toTUID(), sizeof(TUID));

        if ((host->createInstance(messageIid, messageIid, (void **)&message) != kResultOk)
            || (message == nullptr)) {
            return;
        }

        message->setMessageID(id);
        message->getAttributes()->setInt("value", value);
        peer->notify(message);
        message->release();
    }

    static void silence(float ** out, int32 frames) {
        for (int c = 0; c < GB_CHANNELS; c++) {
            memset(out[c], 0, (size_t)frames * sizeof(float));
        }
    }

    void apply_parameter_changes(ProcessData & data) {
        if (data.inputParameterChanges == nullptr) {
            return;
        }

        int32 count = data.inputParameterChanges->getParameterCount();

        for (int32 i = 0; i < count; i++) {
            IParamValueQueue * q = data.inputParameterChanges->getParameterData(i);

            if (q == nullptr) {
                continue;
            }

            int32       points = q->getPointCount();
            int32       offset = 0;
            ParamValue  value  = 0.0;

            // The last point is what a continuous parameter settles at, and that is all any of
            // these need - except the measure trigger.
            if ((points <= 0) || (q->getPoint(points - 1, offset, value) != kResultOk)) {
                continue;
            }

            // A TRIGGER HAS TO BE FOUND AMONG ALL THE POINTS, because the editor raises it and drops
            // it again immediately - a momentary control has to return to rest or its next press
            // produces no edge at all. A host is free to deliver both of those in one block, and
            // reading only the last point then sees nothing but the release. The button did nothing,
            // every time.
            if (q->getParameterId() == kParamMeasure) {
                bool sawPress = false;

                for (int32 point = 0; point < points; point++) {
                    int32      at   = 0;
                    ParamValue held = 0.0;

                    if ((q->getPoint(point, at, held) == kResultOk) && (held >= 0.5)) {
                        sawPress = true;
                        break;
                    }
                }

                // TRIGGER FROM ANY POINT, BUT ARM FROM THE LAST ONE. Both halves matter and they
                // are not the same question: the press has to be found wherever it lands in the
                // block, while whether the button is still DOWN is whatever it settled at. Arming
                // from "was there a press" latched the flag true - the editor's own release arrives
                // in the same block - so the button worked exactly once and then never again.
                if (sawPress && !measureArmed && (measureState == eMeasureIdle)) {
                    log_line("measure requested");
                    start_measurement();
                }

                measureArmed = (value >= 0.5);
                continue;
            }

            ParamID id = q->getParameterId();

            if ((id >= GB_CC_BASE) && (id < (GB_CC_BASE + GB_CC_COUNT))) {
                if (instrument) {
                    send_controller(id, value);
                }

                continue;
            }

            if (q->getParameterId() == kParamTrim) {
                trimGain.store((float)(value * 2.0));
            } else if (q->getParameterId() == kParamDevice) {
                int wanted = gb_device_slot(value);

                if (wanted != wantedDevice.load()) {
                    wantedDevice.store(wanted);
                    request_device();       // signals the worker; opens nothing on this thread
                }
            } else if (q->getParameterId() == kParamMidiDest) {
                int wanted = (int)(value * (double)(GB_MIDI_SLOTS - 1) + 0.5);

                midiDestination.store(wanted);
                gb_midi_destination_name(wanted, midiName, sizeof(midiName));
            } else if (q->getParameterId() == kParamMidiChannel) {
                midiChannel.store((int)(value * (double)(GB_CHANNEL_SLOTS - 1) + 0.5));
            } else if (q->getParameterId() == kParamOffsetMs) {
                offsetMs.store(GB_OFFSET_MIN_MS + (value * (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS)));

                // The correction is part of the reported figure, so the host has to be told.
                uint32 nowLatency = running ? report_latency() : 0;

                if (nowLatency != reportedLatency) {
                    reportedLatency = nowLatency;
                    latencyDirty.store(true);
                    wake_worker();
                }
            } else if (q->getParameterId() == kParamRate) {
                int index = (int)(value * (double)(gGbRateCount - 1) + 0.5);

                if (gGbRates[index] != wantedRate.load()) {
                    wantedRate.store(gGbRates[index]);
                    request_device();
                }
            } else if (q->getParameterId() == kParamMode) {
                int wanted = (value < 0.5) ? 1 : 2;

                if ((uint32_t)wanted != captureChannels) {
                    wantedChannels.store(wanted);
                    request_device();
                }
            } else if (q->getParameterId() == kParamFirstChannel) {
                int wanted = (int)(value * (double)(GB_MAX_FIRST_CHANNEL - 1) + 0.5);

                wantedFirstChannel.store(wanted);
                request_device();
            } else if (q->getParameterId() == kParamFrames) {
                int index = (int)(value * (double)(gGbFrameCount - 1) + 0.5);

                if (gGbFrames[index] != wantedFrames.load()) {
                    wantedFrames.store(gGbFrames[index]);
                    request_device();
                }
            }
        }
    }

    // MONO IS WIDENED HERE, on the way into the ring, rather than on the way out.
    //
    // The ring, the resampler and the drift loop are all stereo, and keeping them that way means
    // one code path downstream instead of a channel count threaded through every one of them. The
    // cost is resampling a duplicated channel, which is a few hundred thousand multiplies a second
    // - nothing beside the clarity of not having a mono variant of the whole chain.
    static void capture_callback(void * user, const float * input, float * output, uint32_t frames) {
        (void)output;

        GenBridgePlugin * self = (GenBridgePlugin *)user;
        const float *     source = input;

        if (self->captureChannels == 1) {
            for (uint32_t i = 0; i < frames; i++) {
                self->widen[(i * 2) + 0] = input[i];
                self->widen[(i * 2) + 1] = input[i];
            }

            source = self->widen;
        }

        if (!ring_write(&self->ring, source, frames)) {
            self->needResync.store(true);
        }
    }

    // ---- device life cycle, worker thread only -------------------------------------------------
    //
    // The device is opened IN PROCESS for now. The feeder process this eventually wants - spawned
    // with posix_spawn so it inherits the host's microphone consent - is a robustness and sharing
    // move, not a correctness one, and splitting it out before there is a working plug-in to feed
    // means debugging IPC and DAW hosting at the same time with no baseline.
    //
    // Everything here runs on the worker, never on the audio thread and never on the host's UI
    // thread. Opening a Core Audio device allocates, talks to a driver and can block for tens of
    // milliseconds; process() only ever asks for a change and carries on.

    void start_worker(void) {
        if (workerActive) {
            return;
        }

        workerQuit.store(false);
        workerActive = (pthread_create(&worker, nullptr, worker_entry, this) == 0);
    }

    void stop_worker(void) {
        if (!workerActive) {
            return;
        }

        workerQuit.store(true);
        wake_worker();
        pthread_join(worker, nullptr);
        workerActive = false;
    }

    void wake_worker(void) {
        pthread_mutex_lock(&wakeMutex);
        wakeFlag = true;
        pthread_cond_signal(&wakeCond);
        pthread_mutex_unlock(&wakeMutex);
    }

    void request_device(void) {
        deviceDirty.store(true);
        wake_worker();
    }

    static void * worker_entry(void * arg) {
        ((GenBridgePlugin *)arg)->worker_loop();
        return nullptr;
    }

    void worker_loop(void) {
        while (!workerQuit.load()) {
            pthread_mutex_lock(&wakeMutex);

            while (!wakeFlag && !workerQuit.load()) {
                pthread_cond_wait(&wakeCond, &wakeMutex);
            }

            wakeFlag = false;
            pthread_mutex_unlock(&wakeMutex);

            if (workerQuit.load()) {
                break;
            }

            if (deviceDirty.exchange(false)) {
                reconfigure();
            }

            if (retuneState.load() == eRetuneRequested) {
                retune();
            }

            if (revertWanted.exchange(false)) {
                revert_retune();
            }

            if (measureStore.exchange(false)) {
                store_measurement();
            }

            if (latencyDirty.exchange(false)) {
                // BEFORE send_latency_changed(), deliberately. That call is what makes the host
                // reactivate us and reopen the device, and reconfigure() decides on reopen whether
                // to re-seed the offset. Writing the new value into the table first means the pair
                // is already up to date by the time anything looks at it.
                sync_offset_to_pair();
                send_latency_changed();
                publish_measurement();
            }
        }
    }

    // Resolve what the plug-in should be listening to, then swap to it under configLock so a
    // process() call in flight sees either the old arrangement or the new one, never a half
    // dismantled one.
    void reconfigure(void) {
        tDeviceInfo list[DEVICE_MAX];
        uint32_t    count = device_enumerate(list, DEVICE_MAX);
        tDeviceInfo chosen;
        bool        found = false;

        int index = wantedDevice.load();

        log_line("reconfigure: slot %d, saved uid '%s'", index, deviceSelector.c_str());

        // THE PARAMETER IS THE ONLY SELECTOR. It used to be one of two, with a saved UID as the
        // other, and they disagreed: the panel drew the parameter while the processor had opened
        // whatever the UID named, so the header said it was capturing a QU-24 while the device row
        // said Analog Keys and the audio was a Kronos. Three answers, all sincerely held.
        //
        // The UID is still stored, and still keys the per-device settings - it is simply no longer
        // allowed to decide WHICH device. One selector, one answer, and the panel cannot be wrong
        // about it.
        if (index >= 0) {
            found = resolve_slot(list, count, index, &chosen);
        }

        // Before any parameter has arrived, the saved UID is what a reopened project has to go on.
        // It is no longer a competing selector: the controller resolves the same UID to the same
        // slot and sets the parameter to match, so what arrives next agrees with what opened here.
        if (!found && (index < 0) && !deviceSelector.empty()) {
            found = device_find(deviceSelector.c_str(), true, &chosen);
            log_line("no parameter yet - restoring saved uid: %s", found ? chosen.name : "not present");
        }

        // NOTHING IS OPENED UNTIL SOMETHING HAS ACTUALLY BEEN CHOSEN.
        //
        // Every previous version of this opened SOMETHING on a fresh instance - first a hard-coded
        // Kronos, then whatever sat at slot 0 - on the reasoning that a silent plug-in looks broken.
        // That reasoning was wrong, and expensively so. Slot 0 on this machine is an iPhone
        // Continuity microphone, so loading a set woke it once per instance, synchronously, on the
        // host's main thread during load. Ableton stopped starting.
        //
        // A plug-in has no business seizing capture hardware nobody asked it to. Idle until chosen
        // is both safer and more honest, and the panel says "no device selected" rather than naming
        // something the user never picked.
        if (!found && (index < 0)) {
            log_line("nothing selected yet - staying idle");
        }

        if (!found) {
            log_line("slot %d resolved to nothing - staying closed", index);
        } else {
            log_line("slot %d -> '%s'", index, chosen.name);
        }

        pthread_mutex_lock(&configLock);
        close_capture_locked();

        if (found) {
            // Recorded only on SUCCESS. Writing it before the attempt meant a device that failed to
            // open still became the thing the project saved and reopened with.
            if (open_capture_locked(chosen)) {
                deviceSelector = chosen.uid;
                appliedDevice  = index;
            } else {
                log_line("open failed - selection unchanged, staying closed");
            }
        }

        uint32 nowLatency = running ? report_latency() : 0;

        pthread_mutex_unlock(&configLock);

        // Told OUTSIDE the lock: restartComponent re-enters the plug-in, and a host is entitled to
        // call straight back into it - including into process(), which trylocks this same mutex.
        if (nowLatency != reportedLatency) {
            reportedLatency = nowLatency;
            send_latency_changed();
            log_line("latency now %u samples - asked the host to re-read it", nowLatency);
        }
    }

    // Slot to device, skipping anything with no inputs - the same filter, in the same order, that
    // the editor and the controller use. One function so the three cannot drift apart again.
    static bool resolve_slot(const tDeviceInfo * list, uint32_t count, int slot, tDeviceInfo * out) {
        uint32_t seen = 0;

        for (uint32_t i = 0; i < count; i++) {
            if (list[i].inputChannels == 0) {
                continue;
            }

            if ((int)seen == slot) {
                *out = list[i];
                return true;
            }

            seen++;
        }

        return false;
    }

    // Apply the smaller setpoint the observed block size allows, then tell the host its latency
    // moved. Worker thread only.
    void retune(void) {
        pthread_mutex_lock(&configLock);

        if (!running) {
            pthread_mutex_unlock(&configLock);
            retuneState.store(eRetuneSettled);
            return;
        }

        double before = setpointFrames;

        setpointFrames = minimum_setpoint_for(observedMaxFrames) * GB_AUTO_MARGIN;

        drift_set_setpoint(&drift, setpointFrames);
        publish_latency_breakdown();

        // The ring holds more than the new setpoint wants, so snap it down rather than waiting for
        // the loop to drain it at a few hundred ppm - which would take minutes.
        needResync.store(true);

        tGbStatus * status = gb_status(statusSlot);

        if (status != nullptr) {
            atomic_store(&status->setpointFrames, setpointFrames);
        }

        uint32 nowLatency = report_latency();

        pthread_mutex_unlock(&configLock);

        retuneState.store(eRetuneSettled);

        log_line("retuned: host declared %u but uses %u - setpoint %.0f -> %.0f, latency %u -> %u",
                 hostMaxFrames, observedMaxFrames, before, setpointFrames, reportedLatency, nowLatency);

        if (nowLatency != reportedLatency) {
            reportedLatency = nowLatency;

            if (status != nullptr) {
                atomic_store(&status->latencySamples, (int)nowLatency);
            }

            send_latency_changed();
        }
    }

    // The pair a measurement belongs to. Both halves matter: the same synth reached over USB and
    // over DIN answers at different speeds, and two synths on one interface are not comparable.
    tMeasured * measured_for(const char * audioUid, const char * midiDest, bool create) {
        for (uint32_t i = 0; i < measuredCount; i++) {
            if ((strncmp(measured[i].audioUid, audioUid, DEVICE_UID_LEN) == 0)
                && (strncmp(measured[i].midiDest, midiDest, GB_MIDI_NAME_LEN) == 0)) {
                return &measured[i];
            }
        }

        if (!create) {
            return nullptr;
        }

        uint32_t slot = measuredCount;

        if (measuredCount < GB_MAX_MEASURED) {
            measuredCount++;
        } else {
            memmove(&measured[0], &measured[1], sizeof(tMeasured) * (GB_MAX_MEASURED - 1));
            slot = GB_MAX_MEASURED - 1;
        }

        memset(&measured[slot], 0, sizeof(measured[slot]));
        strncpy(measured[slot].audioUid, audioUid, DEVICE_UID_LEN - 1);
        strncpy(measured[slot].midiDest, midiDest, GB_MIDI_NAME_LEN - 1);

        return &measured[slot];
    }

    void current_midi_name(char * out, unsigned long len) {
        gb_midi_destination_name(midiDestination.load(), out, len);
    }

    void remember_offset_pair(const char * destination) {
        snprintf(offsetUid, sizeof(offsetUid), "%s", deviceSelector.c_str());
        snprintf(offsetDest, sizeof(offsetDest), "%s", destination);
    }

    // WORKER THREAD ONLY. The +/- arrives on the audio thread, which stores the atomic and asks for
    // a latency update; the table write happens here instead, because measured_for() can allocate a
    // slot and memmove the array and that has no business running under a process() call.
    void sync_offset_to_pair(void) {
        if (!instrument || deviceSelector.empty()) {
            return;
        }

        char destination[GB_MIDI_NAME_LEN];

        current_midi_name(destination, sizeof(destination));

        tMeasured * entry = measured_for(deviceSelector.c_str(), destination, true);

        if (entry != nullptr) {
            entry->offsetMs = offsetMs.load();
        }

        remember_offset_pair(destination);
    }

    void store_measurement(void) {
        int result = measureLatency.load();

        // A RESYNC OR AN UNDERRUN DURING THE MEASUREMENT INVALIDATES IT. Either one means the ring
        // was snapped or starved while we were counting, so the onset moved by however long the
        // disturbance lasted - and the resulting figure is a measurement of the glitch, not of the
        // hardware. Better to say so and let it be repeated than to store a number that looks
        // authoritative and is not.
        unsigned underruns = (unsigned)(atomic_load(&ring.underflows) - measureUnderrunsAtStart);
        int      resynced  = resyncs.load() - measureResyncsAtStart;

        if ((result >= 0) && ((underruns > 0) || (resynced > 0))) {
            log_line("measurement discarded: the capture was not clean during it "
                     "(%u underruns, %d resyncs) - try again", underruns, resynced);

            tGbStatus * status = gb_status(statusSlot);

            if (status != nullptr) {
                atomic_store(&status->measureFailed, 1);
            }

            return;
        }

        if (result < 0) {
            if (result == GB_MEASURE_TOO_EARLY) {
                log_line("measurement: onset at %d frames but our own share is %u - the note cannot "
                         "have arrived before our buffering delivered it. Either the threshold was "
                         "crossed by something other than the test note, or the ring was not at its "
                         "setpoint. floor %.4f, triggered at %.4f",
                         measureOnset.load(), internal_latency(),
                         (double)measureFloorSeen.load(), (double)measureTriggerPeak.load());
            } else {
                log_line("measurement: nothing came back within %.1f s - is the synth on the channel "
                         "and audible?", GB_MEASURE_TIMEOUT_S);
            }

            tGbStatus * status = gb_status(statusSlot);

            if (status != nullptr) {
                atomic_store(&status->measureFailed, (result == GB_MEASURE_TOO_EARLY) ? 0 : 1);
                atomic_store(&status->measureRanEmpty, (result == GB_MEASURE_TOO_EARLY) ? 1 : 0);
            }

            return;
        }

        char destination[GB_MIDI_NAME_LEN];

        current_midi_name(destination, sizeof(destination));

        {
            tGbStatus * status = gb_status(statusSlot);

            if (status != nullptr) {
                atomic_store(&status->measureRanEmpty, 0);
                atomic_store(&status->measureFailed, 0);
            }
        }

        // THE MEASUREMENT LANDS IN THE CORRECTION, which is the whole point of the arrangement: the
        // figure Measure produces is the one the panel then lets you nudge, rather than a number
        // sitting next to a separate trim that starts at zero.
        //
        // Clamped, because offsetMs is a normalised VST3 parameter with a fixed range and a reading
        // outside it cannot be represented. Logged when that bites - a silently clamped round trip
        // would put every take in the wrong place with nothing on screen to say why.
        double measuredMs = (double)result / (hostRate / 1000.0);
        double seeded     = (measuredMs < GB_OFFSET_MIN_MS) ? GB_OFFSET_MIN_MS
                            : ((measuredMs > GB_OFFSET_MAX_MS) ? GB_OFFSET_MAX_MS : measuredMs);

        if (seeded != measuredMs) {
            log_line("measured %.1f ms is outside the offset range %.0f..%.0f - clamped to %.1f",
                     measuredMs, GB_OFFSET_MIN_MS, GB_OFFSET_MAX_MS, seeded);
        }

        tMeasured * entry = measured_for(deviceSelector.c_str(), destination, true);

        if (entry != nullptr) {
            entry->hardwareSamples = (uint32_t)result;
            entry->offsetMs        = seeded;
        }

        offsetMs.store(seeded);
        remember_offset_pair(destination);

        // The panel reads the offset from the CONTROLLER, and the controller has no idea a
        // measurement just happened. Without this the readout stays at its old value until the next
        // click, and worse, the next +/- steps from the stale number and throws the reading away.
        send_message("gbOffset", (int)lround(seeded * 1000.0));

        pthread_mutex_lock(&configLock);
        hardwareSamples = (uint32_t)result;

        uint32 nowLatency = running ? report_latency() : 0;

        pthread_mutex_unlock(&configLock);

        // Underruns during the measurement matter: a gap in the capture delays the onset by
        // however long the gap was, so a figure taken while the ring was starving is not a
        // measurement of the hardware at all.
        log_line("measured: onset at %d frames, our share %d, hardware %d (%.1f ms). "
                 "floor %.4f, triggered at %.4f, underruns %u resyncs %d during. '%s' -> '%s'",
                 measureOnset.load(), measureOurs.load(), result,
                 (double)result / (hostRate / 1000.0),
                 (double)measureFloorSeen.load(), (double)measureTriggerPeak.load(),
                 (unsigned)(atomic_load(&ring.underflows) - measureUnderrunsAtStart),
                 resyncs.load() - measureResyncsAtStart,
                 deviceSelector.c_str(), destination);

        publish_measurement();

        if (nowLatency != reportedLatency) {
            reportedLatency = nowLatency;
            send_latency_changed();
        }
    }

    void publish_measurement(void) {
        publish_latency_breakdown();
    }

    // Every component of the reported figure, so the panel can show where the time actually goes
    // rather than one number nobody can argue with.
    void publish_latency_breakdown(void) {
        tGbStatus * status = gb_status(statusSlot);

        if (status == nullptr) {
            return;
        }

        double ratio = (nominalRatio > 0.0) ? nominalRatio : 1.0;

        atomic_store(&status->ringSamples, (int)(setpointFrames / ratio));
        atomic_store(&status->deviceSamples, (int)((double)deviceLatency / ratio));
        atomic_store(&status->filterSamples, (int)(resampler_latency_frames() / ratio));
        atomic_store(&status->measuredSamples, (int)hardwareSamples);
        atomic_store(&status->offsetSamples, (int)((offsetMs.load() / 1000.0) * hostRate));
        atomic_store(&status->latencySamples, (int)(running ? report_latency() : 0));
    }

    // Put the conservative floor back after a retune turned out to be too tight.
    void revert_retune(void) {
        pthread_mutex_lock(&configLock);

        if (!running) {
            pthread_mutex_unlock(&configLock);
            return;
        }

        double before = setpointFrames;

        setpointFrames = minimum_setpoint(openDeviceFrames) * GB_AUTO_MARGIN;

        drift_set_setpoint(&drift, setpointFrames);
        needResync.store(true);

        tGbStatus * status = gb_status(statusSlot);

        if (status != nullptr) {
            atomic_store(&status->setpointFrames, setpointFrames);
        }

        uint32 nowLatency = report_latency();

        pthread_mutex_unlock(&configLock);

        log_line("retune reverted after an underrun: setpoint %.0f -> %.0f", before, setpointFrames);

        if (nowLatency != reportedLatency) {
            reportedLatency = nowLatency;

            if (status != nullptr) {
                atomic_store(&status->latencySamples, (int)nowLatency);
            }

            send_latency_changed();
        }
    }

    bool open_capture_locked(const tDeviceInfo & info) {
        tDeviceSettings * settings = ensure_settings(info.uid);

        // A rate or buffer size chosen since the last open takes precedence over what was stored,
        // and is then stored itself - so the choice survives the next reopen of the project.
        if (wantedRate.load() > 0.0) {
            settings->rate = wantedRate.exchange(0.0);
        }

        if (wantedFrames.load() > 0) {
            settings->frames = (uint32_t)wantedFrames.exchange(0);
        }

        if (wantedChannels.load() > 0) {
            settings->captureChannels = (uint32_t)wantedChannels.exchange(0);
        }

        if (wantedFirstChannel.load() >= 0) {
            settings->firstChannel = (uint32_t)wantedFirstChannel.exchange(-1);
        }

        // CLAMP THE CHANNEL REQUEST TO WHAT THE DEVICE ACTUALLY HAS, rather than letting the open
        // fail.
        //
        // The channel settings persist per device, but the PARAMETER is global to the instance - so
        // selecting a 32 input desk, choosing channels 17/18, then switching to a two-channel synth
        // asks for channels that do not exist. device_open() refused, nothing opened, and the plug-in
        // appeared stuck on the last device big enough to satisfy the request. Silently refusing to
        // change device is a far worse answer than capturing the nearest thing that exists and
        // showing what happened.
        uint32_t available = info.inputChannels;
        uint32_t first     = settings->firstChannel;
        uint32_t wantCount = settings->captureChannels;

        if (available == 0) {
            log_line("device '%s' reports no input channels", info.name);
            return false;
        }

        if (first >= available) {
            log_line("first channel %u past the device's %u - using channel 1", first + 1, available);
            first = 0;
        }

        if ((first + wantCount) > available) {
            wantCount = available - first;      // at least 1, since first < available
            log_line("only %u channel(s) available from %u - capturing %u",
                     available - first, first + 1, wantCount);
        }

        captureChannels = wantCount;

        // Rate before buffer size: changing the nominal rate can reset the buffer size on some
        // devices, so doing it the other way round silently loses the buffer setting.
        if (settings->rate > 0.0) {
            device_set_sample_rate_and_wait(info.id, settings->rate);
        }

        if (settings->frames > 0) {
            uint32_t lowest  = 0;
            uint32_t highest = 0;
            uint32_t wanted  = settings->frames;

            // CLAMPED TO WHAT THE DEVICE ALLOWS. Asking a USB interface for 16 frames is simply
            // refused, and a refusal looks exactly like a device that changed its mind on its own -
            // so ask what it can do and say what happened.
            if (device_buffer_frame_range(info.id, &lowest, &highest)) {
                if (wanted < lowest) {
                    log_line("device '%s' will not go below %u frames; %u requested",
                             info.name, lowest, settings->frames);
                    wanted = lowest;
                } else if ((highest > 0) && (wanted > highest)) {
                    wanted = highest;
                }
            }

            device_set_buffer_frames(info.id, wanted);
        }

        double deviceRate = device_sample_rate(info.id);

        if ((deviceRate <= 0.0) || (hostRate <= 0.0)) {
            return false;
        }

        uint32_t deviceFrames = device_buffer_frames(info.id);

        nominalRatio   = deviceRate / hostRate;
        deviceLatency  = device_latency_frames(info.id, true);
        setpointFrames = (settings->targetMs > 0.0)
                         ? ((settings->targetMs / 1000.0) * deviceRate)
                         : 0.0;
        trimGain.store(settings->trim);

        uint32_t effectiveHostFrames = (observedMaxFrames > 0) ? observedMaxFrames : hostMaxFrames;
        double   minimum              = minimum_setpoint_for(effectiveHostFrames, deviceFrames);

        if (setpointFrames <= 0.0) {
            setpointFrames = minimum * GB_AUTO_MARGIN;      // auto
        } else if (setpointFrames < minimum) {
            setpointFrames = minimum;                       // an override, clamped to what is safe
        }

        log_line("open %s rate %.0f frames %u ratio %.6f floor %.0f setpoint %.0f devlat %u"
                 " -> reported latency %u",
                 info.name, deviceRate, deviceFrames, nominalRatio, minimum, setpointFrames,
                 deviceLatency,
                 (unsigned)((setpointFrames + resampler_latency_frames() + (double)deviceLatency)
                            / nominalRatio));

        uint32_t ringFrames = (uint32_t)(setpointFrames * 8.0)
                              + (4 * (deviceFrames + hostMaxFrames));

        if (!ring_init(&ring, ringFrames, GB_CHANNELS)) {
            return false;
        }

        if (!resampler_init(&resampler, GB_CHANNELS, nominalRatio, hostMaxFrames)) {
            return false;
        }

        pullCapacity = (uint32_t)((double)hostMaxFrames * nominalRatio * 1.5) + (4 * RESAMPLER_TAPS);
        pullBuffer   = (float *)calloc((size_t)pullCapacity * GB_CHANNELS, sizeof(float));
        interleaved  = (float *)calloc((size_t)hostMaxFrames * GB_CHANNELS, sizeof(float));
        widen        = (float *)calloc((size_t)deviceFrames * 4 * GB_CHANNELS, sizeof(float));

        if ((pullBuffer == nullptr) || (interleaved == nullptr) || (widen == nullptr)) {
            return false;
        }

        tDriftConfig config = drift_default_config();

        drift_init(&drift, &config, deviceRate, setpointFrames);

        if (!device_open(&capture, info.id, true, first, captureChannels,
                         deviceFrames * 4, capture_callback, this)) {
            log_line("device_open failed for '%s' (%u ch from %u)", info.name, captureChannels, first + 1);
            return false;
        }

        if (!device_start(&capture)) {
            return false;
        }

        // COUNTERS BELONG TO THIS OPEN, NOT TO THE INSTANCE. A buffer or rate change tears the
        // device down and rebuilds it, and the rebuild resyncs by design - so carrying the old
        // totals over reports a fault that was actually a setting being changed. Worse, it makes a
        // latency measurement refuse itself, because it checks those same counters to decide
        // whether the capture was clean.
        atomic_store(&ring.underflows, 0);
        atomic_store(&ring.overflows, 0);
        resyncs.store(0);
        primed = false;

        needResync.store(true);
        running = true;

        // What is now genuinely in force. The parameter handlers compare against these, so a
        // re-sent value that changes nothing costs nothing.
        appliedRate         = deviceRate;
        appliedFrames       = deviceFrames;
        appliedChannels     = captureChannels;
        appliedFirstChannel = settings->firstChannel;
        openDeviceFrames    = deviceFrames;

        // Whatever was measured for this device and destination before.
        {
            char destination[GB_MIDI_NAME_LEN];

            current_midi_name(destination, sizeof(destination));

            const tMeasured * previous = measured_for(info.uid, destination, false);

            hardwareSamples = (previous != nullptr) ? previous->hardwareSamples : 0;

            // ONLY WHEN THE PAIR HAS ACTUALLY CHANGED - see offsetUid/offsetDest for why. A reopen
            // of the same device must leave the live correction alone, or applying it would undo
            // it. A genuinely different device gets that device's own stored value, or zero if it
            // has never been measured: a correction belongs to the rig it was taken from, and
            // carrying it to another one is a whole round trip of error, not a small trim.
            if ((strncmp(offsetUid, info.uid, DEVICE_UID_LEN) != 0)
                || (strncmp(offsetDest, destination, GB_MIDI_NAME_LEN) != 0)) {
                offsetMs.store((previous != nullptr) ? previous->offsetMs : 0.0);
                remember_offset_pair(destination);
                send_message("gbOffset", (int)lround(offsetMs.load() * 1000.0));
            }
        }

        // THE OBSERVATION SURVIVES A REOPEN, and this is what stops the retune eating itself.
        //
        // Telling the host its latency changed makes it deactivate and reactivate the plug-in, which
        // reopens the device. Resetting the observation there meant: open wide, watch two seconds,
        // retune, tell the host, get reactivated, open wide again - for ever, with the device torn
        // down and rebuilt every few seconds and the audio in pieces throughout.
        //
        // The real block size is a property of the HOST, not of the device, so once known it stays
        // known and the correct setpoint is used from the first frame. Nothing then changes after
        // activation, so nothing asks the host to restart anything.
        observedFrames = 0;
        retuneState.store((observedMaxFrames > 0) ? eRetuneSettled : eRetuneWatching);

        tGbStatus * status = gb_status(statusSlot);

        snprintf(status->deviceName, sizeof(status->deviceName), "%s", info.name);
        atomic_store(&status->deviceRate, (int)deviceRate);
        atomic_store(&status->deviceFrames, (int)deviceFrames);
        atomic_store(&status->setpointFrames, setpointFrames);
        atomic_store(&status->active, true);

        publish_latency_breakdown();

        return true;
    }

    void close_capture_locked(void) {
        tGbStatus * status = gb_status(statusSlot);

        if (status != nullptr) {
            // Cleared, not just marked inactive: a stale device name outlives the device and the
            // panel goes on naming something it is no longer connected to.
            atomic_store(&status->active, false);
            atomic_store(&status->deviceRate, 0);
            atomic_store(&status->latencySamples, 0);
            status->deviceName[0] = '\0';
        }

        if (running) {
            device_close(&capture);
            ring_free(&ring);
            resampler_free(&resampler);
            running = false;
        }

        free(pullBuffer);
        free(interleaved);
        free(widen);
        pullBuffer  = nullptr;
        interleaved = nullptr;
        widen       = nullptr;
    }

    // THE RING CANNOT GO TO ZERO, and it is worth being precise about why rather than treating it
    // as a tuning knob that happens to bottom out.
    //
    // The device's callback and the host's process() are driven by different clocks and fire at
    // unrelated moments. In the worst phase alignment, process() is called immediately BEFORE the
    // device callback that would have supplied its samples - so the ring must already hold a whole
    // host block's worth of input, or that call underruns. It must also hold a device block, since
    // input arrives in whole blocks and nothing can be consumed from a block that is still being
    // filled. The filter needs its taps either side on top.
    //
    // That floor is a property of block-based audio, not of this design: passing samples straight
    // through would mean a host block landing in a gap between device callbacks and getting
    // silence. It is also why hostMaxFrames is the host's MAXIMUM block size rather than its usual
    // one - the floor has to cover the largest block the host may ever ask for, even if it
    // normally asks for far less.
    double minimum_setpoint(uint32_t deviceFrames) const {
        return minimum_setpoint_for(hostMaxFrames, deviceFrames);
    }

    double minimum_setpoint_for(uint32_t hostFrames) const {
        return minimum_setpoint_for(hostFrames, openDeviceFrames);
    }

    double minimum_setpoint_for(uint32_t hostFrames, uint32_t deviceFrames) const {
        return ((double)hostFrames * nominalRatio)
               + (double)deviceFrames
               + (2.0 * RESAMPLER_TAPS);
    }

    void close_capture(void) {
        pthread_mutex_lock(&configLock);
        close_capture_locked();
        pthread_mutex_unlock(&configLock);
    }

    // ---- per device settings -------------------------------------------------------------------

    tDeviceSettings * find_settings(const char * uid) {
        for (uint32_t i = 0; i < rememberedCount; i++) {
            if (strncmp(remembered[i].uid, uid, DEVICE_UID_LEN) == 0) {
                return &remembered[i];
            }
        }

        return nullptr;
    }

    tDeviceSettings * ensure_settings(const char * uid) {
        tDeviceSettings * found = find_settings(uid);

        if (found != nullptr) {
            return found;
        }

        // Full table: overwrite the oldest entry rather than refusing. Losing the least recently
        // added device's settings is a far better failure than silently ignoring the new one.
        uint32_t slot = rememberedCount;

        if (rememberedCount < GB_MAX_REMEMBERED) {
            rememberedCount++;
        } else {
            slot = 0;
            memmove(&remembered[0], &remembered[1],
                    sizeof(tDeviceSettings) * (GB_MAX_REMEMBERED - 1));
            slot = GB_MAX_REMEMBERED - 1;
        }

        tDeviceSettings * entry = &remembered[slot];

        memset(entry, 0, sizeof(*entry));
        strncpy(entry->uid, uid, DEVICE_UID_LEN - 1);
        // ZERO MEANS "LEAVE THE DEVICE ALONE", and that is the default for both.
        //
        // Setting a device's nominal rate or buffer size is a GLOBAL operation affecting every
        // client of that device - including the host itself, if it happens to be the same
        // interface. Doing it uninvited during load is how a plug-in wedges a DAW. The device's own
        // settings are now simply adopted, and these are written only when the user changes them.
        entry->frames       = 0;
        entry->rate         = 0.0;
        entry->targetMs     = GB_TARGET_AUTO;
        entry->firstChannel    = 0;
        entry->captureChannels = GB_CHANNELS;
        entry->trim            = 1.0f;

        return entry;
    }

    // Fold whatever the user has changed live back into the active device's entry, so that saving
    // the project records what is actually on screen rather than what was last loaded.
    //
    // ONLY WHEN A DEVICE IS ACTUALLY RUNNING. If nothing is open, the live values are construction
    // defaults rather than anything the user chose, and writing them back destroys the settings
    // that were just loaded - a project saved without ever starting playback came back with every
    // trim reset to 1.0.
    void capture_live_settings(void) {
        if (deviceSelector.empty() || !running) {
            return;
        }

        tDeviceSettings * entry = ensure_settings(deviceSelector.c_str());

        entry->trim = trimGain.load();

        // DELIBERATELY NOT WRITING BACK targetMs WHEN IT IS AUTO. It used to record the computed
        // setpoint, which turned "work it out" into a fixed number the moment a project was saved -
        // so a session saved at one buffer size reopened with that size's setpoint baked in, and the
        // floor calculation was quietly bypassed for ever after. Only an explicit choice is stored.
        if (running && (entry->targetMs > 0.0)) {
            entry->targetMs = (setpointFrames / (nominalRatio * hostRate)) * 1000.0;
        }
    }

    bool parse_state(const std::string & blob) {
        // Version 2 added the per-device sample rate as a numeric field, which changes how a dev=
        // line is split - so it needed a version bump rather than a new key. Reading version 1 is
        // still supported: this is exactly the situation the format was versioned for, and refusing
        // to open a session saved yesterday would be a poor advertisement for it.
        int version = 0;

        if (blob.compare(0, 10, "GENBRIDGE3") == 0) {
            version = 3;
        } else if (blob.compare(0, 10, "GENBRIDGE2") == 0) {
            version = 2;
        } else if (blob.compare(0, 10, "GENBRIDGE1") == 0) {
            version = 1;
        } else {
            return false;
        }

        rememberedCount = 0;
        measuredCount   = 0;
        deviceSelector.clear();

        // Defaults for a blob that predates these keys, so loading an older session zeroes the
        // correction rather than leaving whatever the previous project in this instance had. The
        // pair record is cleared with it, so the device open that follows treats this as a new pair
        // and seeds the offset from whatever the restored table holds for it.
        offsetMs.store(0.0);
        offsetUid[0]  = '\0';
        offsetDest[0] = '\0';

        size_t pos = 0;

        while (pos < blob.size()) {
            size_t      end  = blob.find('\n', pos);
            std::string line = blob.substr(pos, (end == std::string::npos) ? std::string::npos : end - pos);

            pos = (end == std::string::npos) ? blob.size() : end + 1;

            if (line.compare(0, 5, "midi=") == 0) {
                std::string wanted = line.substr(5);

                snprintf(midiName, sizeof(midiName), "%s", wanted.c_str());

                int slot = gb_midi_slot_for_name(midiName);

                if (slot >= 0) {
                    midiDestination.store(slot);
                }
            } else if (line.compare(0, 7, "midich=") == 0) {
                midiChannel.store(atoi(line.substr(7).c_str()));
            } else if (line.compare(0, 7, "active=") == 0) {
                deviceSelector = line.substr(7);
            } else if (line.compare(0, 4, "dev=") == 0) {
                parse_device_line(line.substr(4), version);
            } else if (line.compare(0, 3, "hw=") == 0) {
                parse_measured_line(line.substr(3));
            }
            // Anything else is from a newer build; skipping it is the point of the format. That is
            // also why hw= arrives without a version bump: it is a new KEY, and only a change to how
            // a dev= line splits has ever needed the version.
            //
            // It also means the short-lived offset=/meas= pair from earlier today is simply ignored
            // rather than misread. meas= carried SAMPLES where hw= carries milliseconds, so reusing
            // the name would have loaded a 221-sample reading as 221 ms.
        }

        return true;
    }

    // hw=<samples>,<offset ms>,<destination name length>,<destination name><audio uid>
    //
    // THE LENGTH IS THERE BECAUSE BOTH TAILS ARE FREE TEXT. A dev= line gets away with putting its
    // uid last and taking the rest of the line, but this one carries two names, and a MIDI
    // destination is quite entitled to contain a comma - "Scarlett 2i2, Port 1" is an ordinary
    // thing for a driver to call itself. Counting the first name off by length leaves nothing to
    // guess at, where a third comma would have been a guess that fails on somebody's interface.
    void parse_measured_line(const std::string & body) {
        size_t at     = 0;
        double fields[3];

        for (int i = 0; i < 3; i++) {
            size_t comma = body.find(',', at);

            if (comma == std::string::npos) {
                return;
            }
            fields[i] = strtod(body.substr(at, comma - at).c_str(), nullptr);
            at        = comma + 1;
        }

        size_t      destLen = (size_t)fields[2];
        std::string tail    = body.substr(at);

        if (destLen > tail.size()) {
            return;
        }

        std::string dest = tail.substr(0, destLen);
        std::string uid  = tail.substr(destLen);

        if (uid.empty()) {
            return;
        }

        tMeasured * entry = measured_for(uid.c_str(), dest.c_str(), true);

        if (entry != nullptr) {
            entry->hardwareSamples = (uint32_t)fields[0];
            entry->offsetMs        = (fields[1] < GB_OFFSET_MIN_MS) ? GB_OFFSET_MIN_MS
                                     : ((fields[1] > GB_OFFSET_MAX_MS) ? GB_OFFSET_MAX_MS : fields[1]);
        }
    }

    // The canonical order of the numeric fields, and which of them each version actually wrote.
    // A table rather than arithmetic: every time a field is added the shifting gets harder to do in
    // the head, and one wrong offset silently loads a sample rate as a channel number.
    enum { kSlotFrames = 0, kSlotRate, kSlotTarget, kSlotFirst, kSlotChannels, kSlotTrim, kSlotCount };

    void parse_device_line(const std::string & body, int version) {
        static const int kV1[] = { kSlotFrames, kSlotTarget, kSlotFirst, kSlotTrim };
        static const int kV2[] = { kSlotFrames, kSlotRate, kSlotTarget, kSlotFirst, kSlotTrim };
        static const int kV3[] = { kSlotFrames, kSlotRate, kSlotTarget, kSlotFirst, kSlotChannels, kSlotTrim };

        const int * map     = (version >= 3) ? kV3 : ((version == 2) ? kV2 : kV1);
        int         numeric = (version >= 3) ? 6 : ((version == 2) ? 5 : 4);

        unsigned frames = 0, firstChannel = 0, captureChannels = GB_CHANNELS;
        double   rate = 0.0, targetMs = GB_TARGET_AUTO, trim = 1.0;
        size_t   at = 0;
        int      field = 0;

        for (; field < numeric; field++) {
            size_t comma = body.find(',', at);

            if (comma == std::string::npos) {
                return;
            }

            std::string value = body.substr(at, comma - at);

            switch (map[field]) {
                case kSlotFrames:   frames          = (unsigned)strtoul(value.c_str(), nullptr, 10); break;
                case kSlotRate:     rate            = strtod(value.c_str(), nullptr);                break;
                case kSlotTarget:   targetMs        = strtod(value.c_str(), nullptr);                break;
                case kSlotFirst:    firstChannel    = (unsigned)strtoul(value.c_str(), nullptr, 10); break;
                case kSlotChannels: captureChannels = (unsigned)strtoul(value.c_str(), nullptr, 10); break;
                default:            trim            = strtod(value.c_str(), nullptr);                break;
            }

            at = comma + 1;
        }

        std::string uid = body.substr(at);

        if (uid.empty()) {
            return;
        }

        tDeviceSettings * entry = ensure_settings(uid.c_str());

        entry->frames       = frames;
        entry->rate         = rate;
        entry->targetMs     = targetMs;    // 0 is legitimate: it means "auto"
        entry->firstChannel    = firstChannel;
        entry->captureChannels = ((captureChannels == 1) || (captureChannels == 2))
                                 ? captureChannels : GB_CHANNELS;
        entry->trim         = (float)trim;
    }

    static void name_to_utf16(const char * src, char16 * dst, int max) {
        int i = 0;

        for (; (src[i] != '\0') && (i < (max - 1)); i++) {
            dst[i] = (char16)src[i];
        }
        dst[i] = 0;
    }

    std::atomic<int32>  refCount;
    IHostApplication *  host{nullptr};
    IConnectionPoint *  peer{nullptr};
    int                 statusSlot{-1};
    uint32              reportedLatency{0};

    // The block-size observation, and where it has got to.
    enum tRetuneState { eRetuneWatching = 0, eRetuneRequested, eRetuneSettled, eRetuneBlocked };

    std::atomic<tRetuneState> retuneState{eRetuneSettled};
    std::atomic<bool>   revertWanted{false};
    std::atomic<int>    midiDestination{0};
    std::atomic<bool>   offlineRender{false};
    std::atomic<double> offsetMs{0.0};
    std::atomic<int>    midiChannel{0};        // 0 = whatever the note arrived on
    char                midiName[GB_MIDI_NAME_LEN]{};

    enum tMeasureState { eMeasureIdle = 0, eMeasureSettle, eMeasureFloor, eMeasureListening };

    tMeasureState       measureState{eMeasureIdle};
    uint32_t            measureFrames{0};
    uint32_t            measureOnsetFrames{0};
    int                 measureConfirm{0};
    float               measurePeak{0.0f};
    float               measureFloor{0.0f};
    bool                measureArmed{false};
    std::atomic<int>    measureLatency{0};
    std::atomic<bool>   measureStore{false};
    std::atomic<bool>   latencyDirty{false};
    std::atomic<int>    measureOnset{0};
    std::atomic<int>    measureOurs{0};
    std::atomic<float>  measureTriggerPeak{0.0f};
    std::atomic<float>  measureFloorSeen{0.0f};
    uint32_t            measureUnderrunsAtStart{0};
    int                 measureResyncsAtStart{0};

    tMeasured           measured[GB_MAX_MEASURED];
    uint32_t            measuredCount{0};

    // WHICH PAIR THE LIVE offsetMs IS FOR. Without this the correction could not survive its own
    // application: changing the reported latency makes the host reactivate the plug-in, which
    // reopens the device, and a reopen that re-seeded the offset from the table would undo every
    // nudge the moment it took effect. Re-seeding is now gated on the pair actually having changed,
    // so reopening the same device leaves the value exactly where the user put it.
    char                offsetUid[DEVICE_UID_LEN]{0};
    char                offsetDest[GB_MIDI_NAME_LEN]{0};
    uint32_t            hardwareSamples{0};    // in force for the current device/destination pair
    uint32_t            observedMaxFrames{0};
    uint64_t            observedFrames{0};
    uint32_t            openDeviceFrames{0};
    std::atomic<bool>  needResync{true};
    std::atomic<float> trimGain{1.0f};
    std::atomic<bool>  deviceDirty{false};
    std::atomic<int>   resyncs{0};       // how often the ring had to be snapped back; 0 is healthy
    std::atomic<bool>  workerQuit{false};
    std::atomic<int>   wantedDevice{-1};
    std::atomic<double> wantedRate{0.0};
    std::atomic<int>   wantedFrames{0};
    std::atomic<int>   wantedChannels{0};
    std::atomic<int>   wantedFirstChannel{-1};
    uint32_t           captureChannels{GB_CHANNELS};

    // Set by the last successful open; read by the parameter handlers. Plain members rather than
    // atomics: only the worker writes them, and only under configLock, which process() holds
    // whenever it reads them.
    int                appliedDevice{-1};
    double             appliedRate{0.0};
    uint32_t           appliedFrames{0};
    uint32_t           appliedChannels{0};
    uint32_t           appliedFirstChannel{0};
    float *            widen{nullptr};

    pthread_mutex_t    configLock;      // held by the worker while swapping devices; trylocked in process()
    pthread_mutex_t    wakeMutex;
    pthread_cond_t     wakeCond;
    pthread_t          worker{};
    bool               wakeFlag{false};
    bool               workerActive{false};

    tDeviceSettings    remembered[GB_MAX_REMEMBERED];
    uint32_t           rememberedCount{0};

    tRing         ring;
    tResampler    resampler;
    tDrift        drift;
    tDeviceStream capture;

    std::string   deviceSelector;
    double        hostRate{0.0};
    uint32_t      hostMaxFrames{1024};
    double        nominalRatio{1.0};
    double        setpointFrames{0.0};
    uint32_t      deviceLatency{0};
    uint32_t      pullCapacity{0};
    float *       pullBuffer{nullptr};
    float *       interleaved{nullptr};
    bool          running{false};
    bool          primed{false};
    const bool    instrument;
};

// ------------------------------------------------------------------------------------------------
// The controller. A SEPARATE registered class from the processor, and it must stay that way.
//
// VST3 permits one object to implement IComponent, IAudioProcessor and IEditController together,
// which is simpler and which a hand written test host accepts happily. G2-Edit shipped that
// arrangement and Ableton would not have it: the host obtains the controller by instantiating the
// class named by IComponent::getControllerClassId() and does not fall back to asking the component
// for IEditController. With a single object it reported "parameter count is 0" and its wrench icon
// opened nothing.
//
// Exposing at least one automatable parameter matters for the same reason. A plug-in with none
// leaves the host's generic panel empty and looks broken even when it is working perfectly.
// ------------------------------------------------------------------------------------------------

class GenBridgeController : public IEditController, public IConnectionPoint, public IMidiMapping {
public:
    explicit GenBridgeController(bool instrumentIn) : refCount(1), instrument(instrumentIn) {
        if (instrument) {
            gb_midi_init();
        }
    }
    virtual ~GenBridgeController(void) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void ** obj) SMTG_OVERRIDE {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IEditController)
        QUERY_INTERFACE(iid, obj, IPluginBase::iid, IEditController)
        QUERY_INTERFACE(iid, obj, IEditController::iid, IEditController)
        QUERY_INTERFACE(iid, obj, IConnectionPoint::iid, IConnectionPoint)
        QUERY_INTERFACE(iid, obj, IMidiMapping::iid, IMidiMapping)
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef(void) SMTG_OVERRIDE { return (uint32)++refCount; }

    uint32 PLUGIN_API release(void) SMTG_OVERRIDE {
        int32 c = --refCount;

        if (c == 0) {
            delete this;
            return 0;
        }
        return (uint32)c;
    }

    tresult PLUGIN_API initialize(FUnknown * context) SMTG_OVERRIDE {
        (void)context;
        return kResultOk;
    }

    tresult PLUGIN_API terminate(void) SMTG_OVERRIDE { return kResultOk; }

    // ---- IConnectionPoint ----
    //
    // The processor announces which status slot it owns. Without this the editor has no way to know
    // WHICH processor it belongs to, which is how a panel showing a microphone came to report that
    // it was capturing a Kronos - it was reading the other instance's figures.

    tresult PLUGIN_API connect(IConnectionPoint * other) SMTG_OVERRIDE {
        peer = other;
        return kResultOk;
    }

    tresult PLUGIN_API disconnect(IConnectionPoint * other) SMTG_OVERRIDE {
        (void)other;
        peer = nullptr;
        return kResultOk;
    }

    tresult PLUGIN_API notify(IMessage * message) SMTG_OVERRIDE {
        if (message == nullptr) {
            return kInvalidArgument;
        }

        const char * id = message->getMessageID();

        if (strcmp(id, "gbStatusSlot") == 0) {
            int64 slot = -1;

            if (message->getAttributes()->getInt("value", slot) == kResultOk) {
                statusSlot = (int)slot;

                if (editorView != nullptr) {
                    gb_editor_set_status_slot(editorView, statusSlot);
                }
            }
        } else if (strcmp(id, "gbOffset") == 0) {
            // A measurement or a device change moved the correction inside the processor. The panel
            // reads this parameter, and the next +/- steps from it, so a stale value here would be
            // written straight back over the new reading on the first click.
            int64 thousandths = 0;

            if (message->getAttributes()->getInt("value", thousandths) == kResultOk) {
                double ms      = (double)thousandths / 1000.0;
                double clamped = (ms < GB_OFFSET_MIN_MS) ? GB_OFFSET_MIN_MS
                                 : ((ms > GB_OFFSET_MAX_MS) ? GB_OFFSET_MAX_MS : ms);

                offset = (clamped - GB_OFFSET_MIN_MS) / (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS);

                // Through the handler, not just into the member: this is a parameter the host
                // automates and saves, and a value it was never told about is one it will overwrite
                // from its own copy at the next opportunity.
                if (componentHandler != nullptr) {
                    componentHandler->beginEdit(kParamOffsetMs);
                    componentHandler->performEdit(kParamOffsetMs, offset);
                    componentHandler->endEdit(kParamOffsetMs);
                }

                if (editorView != nullptr) {
                    gb_editor_refresh_values(editorView);
                }
            }
        } else if (strcmp(id, "gbLatency") == 0) {
            // The whole point of the round trip: only the controller holds the handler that can
            // tell the host to read the latency again.
            if (componentHandler != nullptr) {
                componentHandler->restartComponent(kLatencyChanged);
            }
        }

        return kResultOk;
    }

    // THE HOST HANDS THE CONTROLLER THE COMPONENT'S OWN STATE, and ignoring it is why a saved set
    // reopened with every track showing the first device in the list. The parameters are restored
    // from the same bytes the processor restores from, so panel and capture agree on load.
    tresult PLUGIN_API setComponentState(IBStream * state) SMTG_OVERRIDE {
        if (state == nullptr) {
            return kResultFalse;
        }

        std::string blob;
        char        chunk[1024];
        int32       read = 0;

        while ((state->read(chunk, (int32)sizeof(chunk), &read) == kResultOk) && (read > 0)) {
            blob.append(chunk, (size_t)read);

            if (read < (int32)sizeof(chunk)) {
                break;
            }
        }

        tGbActive active = gb_parse_active(blob);

        // The MIDI half is restored even when no audio device was stored: an instrument may have had
        // its destination chosen and its capture not.
        if (!active.midiName.empty()) {
            gb_midi_invalidate();

            int slot = gb_midi_slot_for_name(active.midiName.c_str());

            if (slot >= 0) {
                midiDest = (double)slot / (double)(GB_MIDI_SLOTS - 1);
            }
        }

        midiChannel = (double)active.midiChannel / (double)(GB_CHANNEL_SLOTS - 1);

        // THE CORRECTION IS PER DEVICE, so unlike the MIDI half above it has nothing to restore
        // until a device is known - gb_parse_active() resolves it by matching the saved pair. With
        // no device stored there is no pair, and the default of zero is the honest answer.
        {
            double clamped = (active.offsetMs < GB_OFFSET_MIN_MS) ? GB_OFFSET_MIN_MS
                             : ((active.offsetMs > GB_OFFSET_MAX_MS) ? GB_OFFSET_MAX_MS : active.offsetMs);

            offset = (clamped - GB_OFFSET_MIN_MS) / (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS);
        }

        if (!active.valid) {
            return kResultOk;
        }

        gb_device_list_invalidate();

        int slot = gb_slot_for_uid(active.uid.c_str());

        if (slot >= 0) {
            device = gb_device_normalized(slot);
        }

        for (int i = 0; i < gGbRateCount; i++) {
            if (gGbRates[i] == active.rate) {
                rate = (double)i / (double)(gGbRateCount - 1);
            }
        }

        for (int i = 0; i < gGbFrameCount; i++) {
            if ((unsigned)gGbFrames[i] == active.frames) {
                frames = (double)i / (double)(gGbFrameCount - 1);
            }
        }

        mode         = (active.channels == 1) ? 0.0 : 1.0;
        firstChannel = (double)active.firstChannel / (double)(GB_MAX_FIRST_CHANNEL - 1);
        trim         = (double)active.trim / 2.0;

        return kResultOk;
    }

    tresult PLUGIN_API setState(IBStream * state) SMTG_OVERRIDE {
        (void)state;
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream * state) SMTG_OVERRIDE {
        (void)state;
        return kResultOk;
    }

    int32 PLUGIN_API getParameterCount(void) SMTG_OVERRIDE {
        // The effect has no MIDI out, so it does not pretend to a parameter for one.
        // The effect has no MIDI out, so no destination, no measurement, no correction to one - and
        // no controllers to pass on either.
        return instrument ? (kParamCount + GB_CC_COUNT) : (kParamCount - 4);
    }

    // The rate and buffer lists are the SAME arrays the editor steps through and the processor
    // applies, so an index cannot mean one thing here and another there.

    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo & info) SMTG_OVERRIDE {
        memset(&info, 0, sizeof(info));
        info.unitId = 0;           // kRootUnitId, without pulling in ivstunits.h

        // The controller pass-throughs. Hidden and not automatable: they exist so the host has
        // somewhere to deliver a pedal or a wheel, not so a person can draw one in.
        if (index >= kParamCount) {
            uint32_t offsetIndex = (uint32_t)(index - kParamCount);

            if (!instrument || (offsetIndex >= GB_CC_COUNT)) {
                return kInvalidArgument;
            }

            uint32_t channel    = offsetIndex / GB_CC_PER_CHANNEL;
            uint32_t controller = offsetIndex % GB_CC_PER_CHANNEL;
            char     title[64];

            info.id         = GB_CC_BASE + offsetIndex;
            info.stepCount  = 0;
            info.flags      = ParameterInfo::kIsHidden;

            // Pitch bend rests in the middle; everything else rests at zero.
            info.defaultNormalizedValue = (controller == (uint32_t)kPitchBend) ? 0.5 : 0.0;

            snprintf(title, sizeof(title), "Ch%u CC%u", channel + 1, controller);
            to_utf16(title, info.title, 128);
            to_utf16(title, info.shortTitle, 128);

            return kResultOk;
        }

        // A STEPPED LIST PARAMETER IS THE DEVICE CHOOSER, until the SynthLib editor exists. A host
        // renders one as a drop-down in its generic panel, which makes the plug-in usable with no
        // editor at all - and it keeps working afterwards, because it is automatable and the host
        // saves it. The step count is fixed at registration and cached by the host, so it cannot
        // track how many devices the machine happens to have; unused slots simply read "-".
        if (index == kParamDevice) {
            info.id                     = kParamDevice;
            info.stepCount              = GB_DEVICE_SLOTS - 1;
            info.defaultNormalizedValue = 0.0;
            info.flags                  = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;

            to_utf16("Capture Device", info.title, 128);
            to_utf16("Device", info.shortTitle, 128);

            return kResultOk;
        }

        if (index == kParamTrim) {
            info.id                     = kParamTrim;
            info.stepCount              = 0;
            info.defaultNormalizedValue = 0.5;      // unity, since the value maps to 0..2
            info.flags                  = ParameterInfo::kCanAutomate;

            to_utf16("Output Trim", info.title, 128);
            to_utf16("Trim", info.shortTitle, 128);
            to_utf16("x", info.units, 128);

            return kResultOk;
        }

        if (index == kParamRate) {
            info.id                     = kParamRate;
            info.stepCount              = gGbRateCount - 1;
            info.defaultNormalizedValue = 1.0 / (double)(gGbRateCount - 1);   // 48000
            info.flags                  = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;

            to_utf16("Sample Rate", info.title, 128);
            to_utf16("Rate", info.shortTitle, 128);

            return kResultOk;
        }

        if ((index == kParamMidiDest) && instrument) {
            info.id                     = kParamMidiDest;
            info.stepCount              = GB_MIDI_SLOTS - 1;
            info.defaultNormalizedValue = 0.0;
            info.flags                  = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;

            to_utf16("MIDI Destination", info.title, 128);
            to_utf16("MIDI", info.shortTitle, 128);

            return kResultOk;
        }

        if ((index == kParamMidiChannel) && instrument) {
            info.id                     = kParamMidiChannel;
            info.stepCount              = GB_CHANNEL_SLOTS - 1;
            info.defaultNormalizedValue = 0.0;      // "Source"
            info.flags                  = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;

            to_utf16("MIDI Channel", info.title, 128);
            to_utf16("Channel", info.shortTitle, 128);

            return kResultOk;
        }

        if ((index == kParamMeasure) && instrument) {
            info.id                     = kParamMeasure;
            info.stepCount              = 1;
            info.defaultNormalizedValue = 0.0;
            info.flags                  = ParameterInfo::kCanAutomate;

            to_utf16("Measure Latency", info.title, 128);
            to_utf16("Measure", info.shortTitle, 128);

            return kResultOk;
        }

        if ((index == kParamOffsetMs) && instrument) {
            info.id                     = kParamOffsetMs;
            info.stepCount              = 0;
            info.defaultNormalizedValue = -GB_OFFSET_MIN_MS / (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS);
            info.flags                  = ParameterInfo::kCanAutomate;

            to_utf16("Latency Offset", info.title, 128);
            to_utf16("Offset", info.shortTitle, 128);
            to_utf16("ms", info.units, 128);

            return kResultOk;
        }

        if (index == kParamMode) {
            info.id                     = kParamMode;
            info.stepCount              = 1;
            info.defaultNormalizedValue = 1.0;      // stereo
            info.flags                  = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;

            to_utf16("Channel Mode", info.title, 128);
            to_utf16("Mode", info.shortTitle, 128);

            return kResultOk;
        }

        if (index == kParamFirstChannel) {
            info.id                     = kParamFirstChannel;
            info.stepCount              = GB_MAX_FIRST_CHANNEL - 1;
            info.defaultNormalizedValue = 0.0;      // channel 1
            info.flags                  = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;

            to_utf16("First Channel", info.title, 128);
            to_utf16("Chan", info.shortTitle, 128);

            return kResultOk;
        }

        if (index == kParamFrames) {
            info.id                     = kParamFrames;
            info.stepCount              = gGbFrameCount - 1;
            info.defaultNormalizedValue = 1.0 / (double)(gGbFrameCount - 1);  // 128
            info.flags                  = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;

            to_utf16("Device Buffer", info.title, 128);
            to_utf16("Buffer", info.shortTitle, 128);

            return kResultOk;
        }

        return kInvalidArgument;
    }

    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue valueNormalized, String128 string) SMTG_OVERRIDE {
        char buffer[DEVICE_NAME_LEN + 8];

        if (id == kParamDevice) {
            gb_input_device_name(gb_device_slot(valueNormalized), buffer, sizeof(buffer));
            to_utf16(buffer, string, 128);

            return kResultOk;
        }

        if (id == kParamTrim) {
            snprintf(buffer, sizeof(buffer), "%.2f", valueNormalized * 2.0);
            to_utf16(buffer, string, 128);

            return kResultOk;
        }

        if (id == kParamRate) {
            snprintf(buffer, sizeof(buffer), "%.0f Hz",
                     gGbRates[(int)((valueNormalized * (double)(gGbRateCount - 1)) + 0.5)]);
            to_utf16(buffer, string, 128);

            return kResultOk;
        }

        if (id == kParamFrames) {
            snprintf(buffer, sizeof(buffer), "%d",
                     gGbFrames[(int)((valueNormalized * (double)(gGbFrameCount - 1)) + 0.5)]);
            to_utf16(buffer, string, 128);

            return kResultOk;
        }

        if (id == kParamMode) {
            to_utf16((valueNormalized < 0.5) ? "Mono" : "Stereo", string, 128);
            return kResultOk;
        }

        if (id == kParamMidiChannel) {
            int slot = (int)((valueNormalized * (double)(GB_CHANNEL_SLOTS - 1)) + 0.5);

            if (slot <= 0) {
                to_utf16("Source", string, 128);
            } else {
                snprintf(buffer, sizeof(buffer), "%d", slot);
                to_utf16(buffer, string, 128);
            }

            return kResultOk;
        }

        if (id == kParamMeasure) {
            to_utf16((valueNormalized < 0.5) ? "Ready" : "Measuring", string, 128);
            return kResultOk;
        }

        if (id == kParamOffsetMs) {
            snprintf(buffer, sizeof(buffer), "%+.1f",
                     GB_OFFSET_MIN_MS + (valueNormalized * (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS)));
            to_utf16(buffer, string, 128);
            return kResultOk;
        }

        if (id == kParamMidiDest) {
            gb_midi_destination_name((int)((valueNormalized * (double)(GB_MIDI_SLOTS - 1)) + 0.5),
                                     buffer, sizeof(buffer));
            to_utf16(buffer, string, 128);

            return kResultOk;
        }

        if (id == kParamFirstChannel) {
            int first = (int)((valueNormalized * (double)(GB_MAX_FIRST_CHANNEL - 1)) + 0.5);

            snprintf(buffer, sizeof(buffer), "%d", first + 1);
            to_utf16(buffer, string, 128);

            return kResultOk;
        }

        return kInvalidArgument;
    }

    tresult PLUGIN_API getParamValueByString(ParamID id, TChar * string, ParamValue & valueNormalized) SMTG_OVERRIDE {
        (void)id; (void)string; (void)valueNormalized;
        return kNotImplemented;
    }

    ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue v) SMTG_OVERRIDE {
        (void)id;
        return v * 2.0;
    }

    ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue v) SMTG_OVERRIDE {
        (void)id;
        return v / 2.0;
    }

    ParamValue PLUGIN_API getParamNormalized(ParamID id) SMTG_OVERRIDE {
        switch (id) {
            case kParamTrim:   return trim;
            case kParamDevice: return device;
            case kParamRate:   return rate;
            case kParamFrames: return frames;
            case kParamMode:   return mode;
            case kParamFirstChannel: return firstChannel;
            case kParamMidiDest: return midiDest;
            case kParamMidiChannel: return midiChannel;
            case kParamMeasure:  return measure;
            case kParamOffsetMs: return offset;
            default:
                // A controller pass-through: the host owns its value, and pitch bend rests centred.
                if ((id >= GB_CC_BASE) && (id < (GB_CC_BASE + GB_CC_COUNT))) {
                    return (((id - GB_CC_BASE) % GB_CC_PER_CHANNEL) == (uint32_t)kPitchBend)
                           ? 0.5 : 0.0;
                }

                return 0.0;
        }
    }

    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) SMTG_OVERRIDE {
        switch (id) {
            case kParamTrim:   trim   = value; return kResultOk;
            case kParamDevice: device = value; return kResultOk;
            case kParamRate:   rate   = value; return kResultOk;
            case kParamFrames: frames = value; return kResultOk;
            case kParamMode:   mode   = value; return kResultOk;
            case kParamFirstChannel: firstChannel = value; return kResultOk;
            case kParamMidiDest: midiDest = value; return kResultOk;
            case kParamMidiChannel: midiChannel = value; return kResultOk;
            case kParamMeasure:  measure  = value; return kResultOk;
            case kParamOffsetMs: offset   = value; return kResultOk;
            default:
                if ((id >= GB_CC_BASE) && (id < (GB_CC_BASE + GB_CC_COUNT))) {
                    return kResultOk;      // passed straight to the hardware, nothing to keep here
                }

                return kInvalidArgument;
        }
    }

    // WHERE THE PEDAL COMES FROM. A host asks, once, which parameter each MIDI controller should
    // arrive on; without this it has nowhere to put them and simply discards everything that is not
    // a note.
    tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16 channel,
                                                   CtrlNumber midiControllerNumber,
                                                   ParamID & id) SMTG_OVERRIDE {
        if (!instrument || (busIndex != 0)) {
            return kResultFalse;
        }

        if ((channel < 0) || (channel >= GB_CC_CHANNELS)
            || (midiControllerNumber < 0) || (midiControllerNumber >= GB_CC_PER_CHANNEL)) {
            return kResultFalse;
        }

        id = GB_CC_BASE + ((uint32_t)channel * GB_CC_PER_CHANNEL) + (uint32_t)midiControllerNumber;

        return kResultTrue;
    }

    tresult PLUGIN_API setComponentHandler(IComponentHandler * handler) SMTG_OVERRIDE {
        componentHandler = handler;
        return kResultOk;
    }

    IPlugView * PLUGIN_API createView(FIDString name) SMTG_OVERRIDE {
        if ((name == nullptr) || (strcmp(name, ViewType::kEditor) != 0)) {
            return nullptr;
        }

        // ONE EDITOR AT A TIME is all this pointer can describe, and a host that opens a second
        // without closing the first would leave the older one unreachable. No host does, but the
        // assignment is worth reading as deliberate rather than accidental.
        editorView = gb_create_editor_view(this, componentHandler, statusSlot, instrument,
                                           editor_gone, this);

        return editorView;
    }

private:
    // The host, not this, owns the reference createView() returned: it releases it when the user
    // closes the editor and the view deletes itself there and then. Nothing here held a reference or
    // was told, so editorView went on pointing at freed memory and the status and parameter-notify
    // paths below wrote through it - reached routinely, since they run precisely when the editor is
    // NOT open. Taking a reference of our own instead would be worse: the view already addRefs the
    // controller, so the two would keep each other alive for ever.
    static void editor_gone(void * user) {
        ((GenBridgeController *)user)->editorView = nullptr;
    }

    static void to_utf16(const char * src, char16 * dst, int max) {
        int i = 0;

        for (; (src[i] != '\0') && (i < (max - 1)); i++) {
            dst[i] = (char16)src[i];
        }
        dst[i] = 0;
    }

    std::atomic<int32>  refCount;
    const bool          instrument;
    IComponentHandler * componentHandler{nullptr};
    IConnectionPoint *  peer{nullptr};
    IPlugView *         editorView{nullptr};
    int                 statusSlot{-1};
    ParamValue          trim{0.5};
    ParamValue          device{0.0};
    ParamValue          rate{1.0 / 3.0};      // 48000, index 1 of 4
    ParamValue          frames{0.25};         // 128, index 1 of 5
    ParamValue          mode{1.0};            // stereo
    ParamValue          firstChannel{0.0};    // channel 1
    ParamValue          midiDest{0.0};
    ParamValue          midiChannel{0.0};
    ParamValue          measure{0.0};
    ParamValue          offset{0.5};       // zero correction sits in the middle of the range
};

// ------------------------------------------------------------------------------------------------
// Factory.
// ------------------------------------------------------------------------------------------------

class GenBridgeFactory : public IPluginFactory3 {
public:
    GenBridgeFactory(void) : refCount(1) {}
    virtual ~GenBridgeFactory(void) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void ** obj) SMTG_OVERRIDE {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IPluginFactory)
        QUERY_INTERFACE(iid, obj, IPluginFactory::iid, IPluginFactory)
        QUERY_INTERFACE(iid, obj, IPluginFactory2::iid, IPluginFactory2)
        QUERY_INTERFACE(iid, obj, IPluginFactory3::iid, IPluginFactory3)
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef(void) SMTG_OVERRIDE { return (uint32)++refCount; }

    uint32 PLUGIN_API release(void) SMTG_OVERRIDE {
        int32 c = --refCount;

        if (c == 0) {
            delete this;
            return 0;
        }
        return (uint32)c;
    }

    tresult PLUGIN_API getFactoryInfo(PFactoryInfo * info) SMTG_OVERRIDE {
        if (info == nullptr) {
            return kInvalidArgument;
        }

        memset(info, 0, sizeof(PFactoryInfo));
        strncpy(info->vendor, GB_VENDOR, PFactoryInfo::kNameSize - 1);
        strncpy(info->url, "https://github.com/chrispurusha/GenBridge", PFactoryInfo::kURLSize - 1);
        info->flags = PFactoryInfo::kUnicode;

        return kResultOk;
    }

    int32 PLUGIN_API countClasses(void) SMTG_OVERRIDE { return 4; }

    // 0/1 are the effect's processor and controller, 2/3 the instrument's. The controllers go under
    // kVstComponentControllerClass and NOT kVstAudioEffectClass, or a host enumerating plug-ins
    // finds four audio modules instead of two.
    struct tClassEntry {
        const FUID * cid;
        const char * category;
        const char * name;
        const char * subCategory;
    };

    static const tClassEntry * entry(int32 index) {
        static const tClassEntry kEntries[4] = {
            // OnlyRT ON BOTH, because neither of these can be rendered faster than realtime: the
            // audio does not come from arithmetic, it comes off a wire at whatever speed the
            // hardware runs, and that speed is one second per second. Steinberg's own words for the
            // flag are "supports only realtime process call, no processing faster than realtime".
            //
            // NoOfflineProcess, which the effect already carried, is NOT the same thing - it opts
            // out of a host's offline-processing FEATURE (apply a plug-in destructively to a clip),
            // and says nothing about how a mixdown is rendered.
            //
            // Instrument|Synth is kept rather than swapped for Instrument|External, which is the
            // literal description ("External Instrument (wrapped Hardware)"). Hosts have been known
            // to refuse to load a plug-in whose class they cannot read as an instrument, and Synth
            // is the string already known to work here - not worth trading a plug-in that loads for
            // one that is better described.
            { &kGenBridgeProcessorUID,      kVstAudioEffectClass,        GB_PLUGIN_NAME,
              "Fx|NoOfflineProcess|OnlyRT|Tools" },
            { &kGenBridgeControllerUID,     kVstComponentControllerClass, GB_PLUGIN_NAME " Controller",
              "" },
            { &kGenBridgeInstProcessorUID,  kVstAudioEffectClass,        GB_PLUGIN_NAME " Instrument",
              "Instrument|Synth|OnlyRT" },
            { &kGenBridgeInstControllerUID, kVstComponentControllerClass, GB_PLUGIN_NAME " Instrument Controller",
              "" },
        };

        return ((index < 0) || (index > 3)) ? nullptr : &kEntries[index];
    }

    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo * info) SMTG_OVERRIDE {
        if ((info == nullptr) || (index < 0) || (index > 3)) {
            return kInvalidArgument;
        }

        const tClassEntry * e = entry(index);

        if (e == nullptr) {
            return kInvalidArgument;
        }

        memset(info, 0, sizeof(PClassInfo));
        info->cardinality = PClassInfo::kManyInstances;
        memcpy(info->cid, e->cid->toTUID(), sizeof(TUID));
        strncpy(info->category, e->category, PClassInfo::kCategorySize - 1);
        strncpy(info->name, e->name, PClassInfo::kNameSize - 1);

        return kResultOk;
    }

    // The subcategory lives only on PClassInfo2, which is why IPluginFactory2 is implemented at
    // all. "Fx|NoOfflineProcess|Tools" is what Inject declares, and it is what stops a host trying
    // to bounce a live capture faster than realtime.
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2 * info) SMTG_OVERRIDE {
        if ((info == nullptr) || (index < 0) || (index > 3)) {
            return kInvalidArgument;
        }

        const tClassEntry * e = entry(index);

        if (e == nullptr) {
            return kInvalidArgument;
        }

        memset(info, 0, sizeof(PClassInfo2));
        info->cardinality = PClassInfo::kManyInstances;
        strncpy(info->vendor, GB_VENDOR, PClassInfo2::kVendorSize - 1);
        strncpy(info->version, GB_VERSION_STRING, PClassInfo2::kVersionSize - 1);
        strncpy(info->sdkVersion, kVstVersionString, PClassInfo2::kVersionSize - 1);
        memcpy(info->cid, e->cid->toTUID(), sizeof(TUID));
        strncpy(info->category, e->category, PClassInfo::kCategorySize - 1);
        strncpy(info->name, e->name, PClassInfo::kNameSize - 1);
        strncpy(info->subCategories, e->subCategory, PClassInfo2::kSubCategoriesSize - 1);

        return kResultOk;
    }

    tresult PLUGIN_API getClassInfoUnicode(int32 index, PClassInfoW * info) SMTG_OVERRIDE {
        PClassInfo2 wide;

        if (getClassInfo2(index, &wide) != kResultOk) {
            return kInvalidArgument;
        }

        memset(info, 0, sizeof(PClassInfoW));
        memcpy(info->cid, wide.cid, sizeof(TUID));
        info->cardinality = wide.cardinality;
        strncpy(info->category, wide.category, PClassInfo::kCategorySize - 1);
        info->classFlags = wide.classFlags;
        strncpy(info->subCategories, wide.subCategories, PClassInfo2::kSubCategoriesSize - 1);

        ascii_to_utf16(wide.name, info->name, PClassInfo::kNameSize);
        ascii_to_utf16(wide.vendor, info->vendor, PClassInfo2::kVendorSize);
        ascii_to_utf16(wide.version, info->version, PClassInfo2::kVersionSize);
        ascii_to_utf16(wide.sdkVersion, info->sdkVersion, PClassInfo2::kVersionSize);

        return kResultOk;
    }

    tresult PLUGIN_API createInstance(FIDString cid, FIDString _iid, void ** obj) SMTG_OVERRIDE {
        FUnknown * instance = nullptr;

        if (memcmp(cid, kGenBridgeProcessorUID.toTUID(), sizeof(TUID)) == 0) {
            instance = (IComponent *)new GenBridgePlugin(false);
        } else if (memcmp(cid, kGenBridgeControllerUID.toTUID(), sizeof(TUID)) == 0) {
            instance = (IEditController *)new GenBridgeController(false);
        } else if (memcmp(cid, kGenBridgeInstProcessorUID.toTUID(), sizeof(TUID)) == 0) {
            instance = (IComponent *)new GenBridgePlugin(true);
        } else if (memcmp(cid, kGenBridgeInstControllerUID.toTUID(), sizeof(TUID)) == 0) {
            instance = (IEditController *)new GenBridgeController(true);
        } else {
            return kResultFalse;
        }

        // _iid is a FIDString (const char *) and queryInterface's TUID parameter decays to the
        // same thing, so it goes straight through - casting to TUID would be a cast to an array
        // type, which the compiler rejects outright.
        tresult result = instance->queryInterface(_iid, obj);

        instance->release();        // queryInterface took its own reference

        return result;
    }

    tresult PLUGIN_API setHostContext(FUnknown * context) SMTG_OVERRIDE {
        (void)context;
        return kResultOk;
    }

private:
    static void ascii_to_utf16(const char * src, char16 * dst, int max) {
        int i = 0;

        for (; (src[i] != '\0') && (i < (max - 1)); i++) {
            dst[i] = (char16)src[i];
        }
        dst[i] = 0;
    }

    std::atomic<int32> refCount;
};

extern "C" {
SMTG_EXPORT_SYMBOL IPluginFactory * PLUGIN_API GetPluginFactory(void) {
    return new GenBridgeFactory();
}

// macOS loads a .vst3 as a bundle, so these are the entry points rather than a plain dylib's.
SMTG_EXPORT_SYMBOL bool bundleEntry(void * ref) {
    (void)ref;
    return true;
}

SMTG_EXPORT_SYMBOL bool bundleExit(void) {
    return true;
}
}
