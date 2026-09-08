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

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <CoreAudio/HostTime.h>    // AudioGetCurrentHostTime, to stamp an event at its own offset
#pragma clang diagnostic pop

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

// HOW LONG A DEVICE/RATE/FRAMES CHANGE MUST STAND STILL BEFORE IT IS ACTED ON.
//
// Every one of those settings is a stepper, so moving two places sends two values, and each value
// used to mean a full teardown and rebuild - closing the device, opening whatever slot was passed
// over, then closing it again on the next press. Stepping past a device OPENED it, which is the very
// thing "nothing opens until explicitly chosen" exists to prevent; on this rig slot 0 is an iPhone
// Continuity microphone, so stepping down from a mixer woke it in passing.
//
// A quarter of a second is longer than a person's gap between arrow presses and far shorter than the
// open it defers - an open can spend 800 ms in device_set_sample_rate_and_wait() alone - so a burst
// collapses to one device change and a single deliberate change is not perceptibly slower.
//
// This lives in the PROCESSOR rather than in the editor on purpose. A drop-down would stop the
// panel's own arrows walking the list, but the host's generic panel and any automation lane can
// still sweep the parameter, and they reach this code by the same path.
#define GB_DEVICE_SETTLE_MS  (250.0)

// HOW LONG THE OFFSET MUST STAND STILL BEFORE THE HOST IS TOLD.
//
// Telling the host its latency moved makes it redo delay compensation across the whole session, and
// in Ableton that is a visible hitch - so it is the one thing that must not happen once per click.
// The offset steps 0.1 ms at a time and is dialled in by ear, which means a dozen or more clicks in
// quick succession: exactly the shape that turns a cheap control into a stuttering one.
//
// The VALUE still moves immediately, so the readout follows the pointer and nothing feels laggy.
// Only the notification waits. Slightly longer than the device settle because this is a control
// someone nudges repeatedly while listening, rather than one they set once.
#define GB_OFFSET_SETTLE_MS  (400.0)
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

// The longest the settle will wait for the previous note to decay before giving up on quiet and
// taking the floor anyway. A synth that never goes quiet is a measurement that never happens.
#define GB_MEASURE_SETTLE_MAX_S    (2.5)

// One-pole coefficient for the reported pipeline delay, at roughly two seconds of time constant
// over 128-frame blocks. Slow enough to ignore the ring's sawtooth, quick enough to follow a
// device change without the panel looking stuck.
#define GB_PIPELINE_SMOOTH    (0.0013)

// How far the reported latency must move before the host is told. Every telling costs a full delay
// compensation pass, and the figure now follows a live measurement rather than a constant, so it
// needs a deadband or it would fire on the ring's own noise. 128 frames is 2.7 ms at 48 kHz - below
// what this control is ever dialled in to correct.
#define GB_LATENCY_DEADBAND   (64)

// A sanity ceiling on a callback size read back out of a project file. Nothing sane hands over a
// third of a second in one go, and a corrupt or hand-edited value must not be able to ask for a
// ring measured in seconds.
#define GB_MAX_BLOCK_FRAMES   (16384)
// The onset threshold is RELATIVE to whatever the input is already doing, with an absolute floor
// under it. A synth with a hissy output, a hum, or a pad still decaying would sit above any fixed
// level and trip the detector the instant the note went out. Measuring the quiet first and then
// demanding a multiple of it is what makes the answer mean something.
#define GB_MEASURE_MARGIN     (0.02f)    // absolute minimum rise, for a genuinely silent input
#define GB_MEASURE_RATIO      (8.0f)     // ...or this much above the noise, whichever is greater
#define GB_MEASURE_CEILING    (0.70f)    // never demand more than this; see the note in the code
#define GB_MEASURE_CONFIRM    (2)        // consecutive blocks required, so one glitch is not an onset

// SEVERAL ROUND TRIPS, NOT ONE, because a single one is not a measurement of anything repeatable.
// USB MIDI transit, the synth's own scheduler and its envelope all move the onset a little from one
// note to the next, and a figure taken from one note carries all of it. Five is enough to throw
// away the extremes and still average three.
#define GB_MEASURE_TRIPS      (5)

// TRIMMED, NOT AVERAGED FLAT. One trip landing on a resync, or on a note the synth happened to
// voice-steal, would drag a plain mean by its whole error; dropping the highest and lowest first
// costs nothing when they are honest and removes the outlier when they are not.
#define GB_MEASURE_DROP       (1)

// The default only. Which note is played is settable - a drum machine may have nothing at all on
// middle C, and an Analog Rytm wants the lowest note there is.
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
    kParamTestNote,      // instrument only: which note Measure plays
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
    // THE GATE IS CACHED, because it is a syscall and this is called from threads that must not
    // spend them. access() on every call is cheap next to the fopen below when logging is ON, and
    // it is the entire cost when logging is OFF - which is almost always, and is exactly when it
    // must be free. Re-polled once a second so touching the file still enables logging mid-session
    // rather than needing a reload.
    static std::atomic<double> checkedAt{-1000.0};
    static std::atomic<bool>   enabled{false};
    struct timespec            ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    double now = (double)ts.tv_sec + ((double)ts.tv_nsec * 1e-9);

    if ((now - checkedAt.load()) >= 1.0) {
        checkedAt.store(now);
        enabled.store(access("/tmp/genbridge-log", F_OK) == 0);
    }

    if (!enabled.load()) {
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
    int         testNote{GB_MEASURE_NOTE};
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
        } else if (line.compare(0, 9, "testnote=") == 0) {
            out.testNote = atoi(line.substr(9).c_str());
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

        // Hot-plug. Nothing noticed a device appearing before this, so a plug-in waiting for a saved
        // interface would have waited until the user touched a control - see device_watch_list().
        device_watch_list(device_list_changed, this);

        return kResultOk;
    }

    // From a CoreAudio thread: drop the cached list and wake the worker, nothing more.
    static void device_list_changed(void * user) {
        GenBridgePlugin * self = (GenBridgePlugin *)user;

        gb_device_list_invalidate();
        self->request_device();
    }

    tresult PLUGIN_API terminate(void) SMTG_OVERRIDE {
        device_unwatch_list(this);
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
        log_line("connected to controller, published status slot %d", statusSlot.load());
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

        // UNDER THE LOCK, the mirror of getState() above. parse_state() clears and rewrites almost
        // every piece of configuration the worker reads - deviceSelector and savedDeviceName (both
        // std::string, so a concurrent read is a possibly-freed pointer rather than a stale value),
        // the remembered[] and measured[] tables, and the offset pair. A host may call this while
        // the plug-in is loaded and the worker is mid-reconfigure.
        //
        // The read loop above is deliberately OUTSIDE the lock: state->read() calls back into the
        // host, which must never happen with this held.
        lock_config_from_host("setState");

        bool ok      = parse_state(blob);
        bool wasOpen = running;

        pthread_mutex_unlock(&configLock);

        // ASK FOR THE DEVICE AGAIN, because the settings may have arrived AFTER it was opened.
        //
        // Nothing here controls when a host calls setState relative to everything else. If the
        // device parameter reaches the plug-in first - through the controller, or as a parameter
        // change in the first process() calls - the device is opened before this blob has been
        // read, so it comes up with no remembered entry and "leave the device alone" is the honest
        // default. The saved buffer size then lands in remembered[] with nothing to apply it.
        //
        // That is CT's "I still had to set 64 manually": the settings were restored correctly and
        // simply never reached the device. Rather than guess at a host's ordering, react to the
        // late arrival - request_device() is debounced, so a host that DID call in the tidy order
        // coalesces this into the open it was going to do anyway.
        if (ok && wasOpen) {
            request_device();
        }

        return ok ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API getState(IBStream * state) SMTG_OVERRIDE {
        if (state == nullptr) {
            return kResultFalse;
        }

        // UNDER THE LOCK, and this is the one that could actually crash rather than merely report a
        // wrong number. deviceSelector is a std::string that reconfigure() ASSIGNS under configLock,
        // and capture_live_settings() walks the remembered[] table the same worker appends to. A
        // concurrent read of a std::string being reassigned is not a stale value, it is a pointer
        // that may already have been freed.
        //
        // Blocking is fine here in a way it never is in process(): getState() is called by the host
        // on its own thread when it saves, and the worst wait is one device swap.
        lock_config_from_host("getState");

        capture_live_settings();

        std::string blob = "GENBRIDGE3\n";

        blob += "active=" + deviceSelector + "\n";

        // HOW MUCH THE HOST TAKES PER CALLBACK, which is a property of the HOST and its buffer
        // setting rather than of any device - so it is written once, outside the dev= lines.
        //
        // Saved because neither number available at the FIRST open is right: this host declares 256
        // and hands over 512, and the observation needs an undisturbed callback that a project load
        // does not provide. Without it the ring comes up at 560, discovers the truth a second later
        // and retunes to 880 - which costs a device reopen per instance, 1.4 seconds each on these
        // interfaces. With it, the first open is already correct.
        //
        // A stale value is safe: the retune corrects in BOTH directions now, and setupProcessing()
        // discards it outright if the host comes back declaring a different block size.
        if (observedMaxFrames > 0) {
            blob += "callback=" + std::to_string(observedMaxFrames) + "\n";
        }

        // A NEW KEY, not a version bump: the format skips what it does not recognise, so an older
        // build reading this simply does not get a name to show.
        {
            tGbStatus * status = gb_status(statusSlot);

            if ((status != NULL) && (status->deviceName[0] != '\0')) {
                savedDeviceName = status->deviceName;
            }

            if (!savedDeviceName.empty()) {
                blob += "activename=" + savedDeviceName + "\n";
            }
        }

        // BY NAME, not by index. The MIDI list shifts whenever a device is powered on or off, so an
        // index saved on Monday names something else on Tuesday - the same reasoning that keeps the
        // audio device stored as a UID. Ableton was not forgetting the destination; nothing was ever
        // writing it down.
        if (instrument) {
            char midiNameNow[GB_MIDI_NAME_LEN] = {0};

            current_midi_name(midiNameNow, sizeof(midiNameNow));
            blob += "midi=" + std::string(midiNameNow) + "\n";
            blob += "midich=" + std::to_string(midiChannel.load()) + "\n";
            blob += "testnote=" + std::to_string(testNote.load()) + "\n";

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

            // NOT LOGGED FROM HERE, and that is not tidiness. getState() is called by a host far
            // more often than a save: Ableton takes an undo snapshot on ordinary UI actions, and
            // this loop runs once per REMEMBERED device - up to 32 of them. log_line() opens and
            // closes the file on every call, so a line here is dozens of file operations on the
            // HOST'S MAIN THREAD every time someone moves a control. It was added to answer one
            // question ("did the buffer size ever reach the project file?"), it answered it, and it
            // would have been a fresh cause of the very beachball it was helping to chase.
            //
            // The "restoring:" line on the way back in survives, because setState runs once per
            // load and is where a value that failed to persist actually shows up as missing.
        }
        // Released before write(): the blob is a private copy by now, and state->write() calls back
        // into the host - which must never happen with this lock held.
        pthread_mutex_unlock(&configLock);

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

    // INFINITE, and for both variants.
    //
    // kNoTail - which is what a plain 0 means - is a promise that nothing comes out once the input
    // goes silent. That is true of a reverb with its input muted and false of everything this
    // plug-in does: the audio arrives from a piece of hardware and has no relationship to the input
    // bus at all. The effect is USED on a track with nothing feeding it, which is exactly the state
    // in which a host is entitled to stop processing a chain that has promised to be silent.
    //
    // JUCE draws the same distinction from the other side: its wrapper maps a plug-in's tail length
    // onto kNoTail or kInfiniteTail, so any JUCE generator reports infinite where this reported
    // none (juce_audio_plugin_client_VST3.cpp, getTailSamples).
    uint32 PLUGIN_API getTailSamples(void) SMTG_OVERRIDE {
        return kInfiniteTail;
    }

    // Everything between the device's converters and this plug-in's output, so the host can line
    // the track up against the rest of the session. Getting this wrong is the kind of bug people
    // live with for months without noticing: the audio is simply, quietly, in the wrong place.
    // THE PUBLISHED FIGURE, not a fresh computation.
    //
    // A host may call this at any time on any thread. Recomputing meant reading setpointFrames,
    // deviceLatency and nominalRatio - all plain doubles the worker rewrites under configLock during
    // a device swap - so the answer could be assembled from a half-updated set, and nominalRatio is
    // a DIVISOR in internal_latency_frames(): observed as 0 mid-swap it yields inf or NaN, handed
    // straight to the host as a latency.
    //
    // Making `running` atomic fixed the flag and not the payload it gates. One atomic snapshot,
    // written by the worker once a swap has finished, removes the composite read entirely - and it
    // is the right value by definition, because it is exactly what the host was last told.
    uint32 PLUGIN_API getLatencySamples(void) SMTG_OVERRIDE {
        return reportedLatency.load();
    }

    // HOW LONG THE HOST'S OWN THREAD WAITED FOR configLock, said out loud when it is long enough to
    // see. getState(), setState() and setupProcessing() all take that lock, and the worker holds it
    // across an entire device close and open - so a reconfigure can block whichever thread the host
    // called on. In Ableton that thread is the main one, and it calls getState() for undo snapshots
    // on ordinary UI actions: the beachball CT reports.
    //
    // This turns "occasional or regular spinning cursor" into a number with a cause beside it. If
    // these lines are absent while the cursor spins, the cause is somewhere else entirely and this
    // has ruled out the obvious suspect - which is worth as much.
    void lock_config_from_host(const char * who) {
        double began = now_ms();

        pthread_mutex_lock(&configLock);

        double waited = now_ms() - began;

        if (waited >= 20.0) {
            log_line("HOST THREAD BLOCKED: %s waited %.0f ms for configLock - a device open was in "
                     "flight. This is what a spinning cursor looks like from in here", who, waited);
        }
    }

    // WORTH INTERRUPTING THE HOST FOR? Opening or closing a device always is - the figure goes to or
    // from zero and everything downstream of the track moves. A drift of a frame or two is not: the
    // reported latency follows a live measurement now, so without this it would change most blocks
    // and each change is a full delay compensation pass across the session.
    bool latency_worth_reporting(uint32 nowLatency) const {
        uint32 was = reportedLatency.load();

        if ((was == 0) != (nowLatency == 0)) {
            return true;
        }

        uint32 moved = (nowLatency > was) ? (nowLatency - was) : (was - nowLatency);

        return moved >= GB_LATENCY_DEADBAND;
    }

    // WHAT THE PLUG-IN ITSELF ADDS, with no hardware correction in it. Split out of
    // report_latency() because the measurement has to subtract our share from the onset it sees,
    // and subtracting the REPORTED figure meant subtracting the previous measurement along with it:
    // every re-measure came back short by whatever correction was already in force, so the value
    // walked towards zero the more times it was run. Only ever grows out of the ring and the
    // converters, so it is the honest thing to net off.
    // Call with configLock HELD, after any change to the four fields below. One place, so a new
    // writer cannot forget half of them.
    void publish_config_snapshot(void) {
        snapHostRate.store(hostRate);
        snapRatio.store(nominalRatio);
        snapSetpoint.store(setpointFrames);
        snapDeviceLatency.store(deviceLatency);
    }

    // The snapshot's version of internal_latency_frames(). Safe from any thread; never touches a
    // field the worker can be rewriting.
    double snapshot_latency_frames(void) const {
        // THE SETPOINT, NOT THE MEASUREMENT - reverted 2026-09-08, and the reason is worth keeping.
        //
        // Reporting a smoothed measurement removed a 4-9 ms over-compensation and created something
        // far worse: the figure MOVES, every move past the deadband tells the host its latency
        // changed, and a host answers that by reactivating the plug-in. Reactivation closes and
        // reopens the device, which perturbs the ring, which moves the figure again. Ableton sat in
        // that loop - reconfigure every 1.5 seconds, each holding configLock for 1460 ms, the
        // device flapping between 64 and its restored 512 - which is both the beachball and the
        // "I set 64 and get 512" in one.
        //
        // A latency a host is told must be STABLE. pipelineAvg is still measured and still published
        // as "actual" beside this, so the gap that started this can be seen and dealt with by
        // shrinking it rather than by chasing it.
        double ratio = snapRatio.load();

        if (ratio <= 0.0) {
            return 0.0;    // mid-swap, or nothing open: no latency to report rather than inf
        }

        // Before a block has run there is nothing measured yet, so the setpoint is the estimate.
        return (snapSetpoint.load() + resampler_latency_frames() + (double)snapDeviceLatency.load())
               / ratio;
    }

    uint32 snapshot_latency(void) const {
        double total = snapshot_latency_frames();

        if (instrument) {
            total += (offsetMs.load() / 1000.0) * snapHostRate.load();
        }

        return (total > 0.0) ? (uint32)total : 0;
    }

    // The pipeline's delay for a given ring occupancy. A sample read out of a ring holding `fill`
    // frames entered it `fill` frames ago, so the occupancy IS the delay - the rest is what the
    // resampler and the device's own converters add.
    double latency_frames_for_fill(double fillFrames) const {
        double inputFrames = fillFrames + resampler_latency_frames() + (double)deviceLatency;

        if (nominalRatio <= 0.0) {
            return 0.0;
        }

        // Reported in the HOST's frames, and the ring is measured in the device's.
        return inputFrames / nominalRatio;
    }

    double internal_latency_frames(void) const {
        return latency_frames_for_fill(setpointFrames);
    }

    // THE PIPELINE'S ACTUAL DELAY AT THIS INSTANT, and it takes TWO numbers, not one.
    //
    // Occupancy on its own is not an age. The device writes a whole buffer at a time, so the fill
    // jumps up by deviceFrames at each capture callback and falls as the host drains it - while the
    // audio already in the ring is getting older at exactly the same rate. The two move in
    // ANTI-PHASE and their sum is constant; take the fill alone and what is left is a sawtooth of a
    // full device buffer. At 512 frames that is 10.7 ms, which is precisely the range a repeated
    // measurement was wandering over.
    //
    // So: how long ago the newest frame arrived, plus how many sit in front of it. deviceLatency
    // has the device's buffer inside it (device_latency_frames() sums bufferFrames + latency +
    // safety offset + stream latency), and that buffer term is what "how long ago" now measures
    // directly - counting it twice would put a whole buffer back on.
    //
    // The setpoint, NOT this, is what the host is told: the loop holds the AVERAGE fill there by
    // design, and a latency that moved with every block would have the host redo its delay
    // compensation continuously. This is for the measurement, which needs the instant it happened.
    double latency_frames_measured(double fillFrames, uint64_t at) const {
        if (nominalRatio <= 0.0) {
            return 0.0;
        }

        double buffered = (double)deviceLatency - (double)openDeviceFrames;

        if (buffered < 0.0) {
            buffered = 0.0;
        }

        double inRing = fillFrames + buffered + resampler_latency_frames();

        // NO CALLBACK-LEAD TERM HERE, and it was tried. The frames already handed over inside a
        // callback look like they belong - the fill at the fourth of four calls is 384 frames below
        // the fill at the first - but the ROUND TRIP this figure is subtracted from is invariant to
        // which call detects the onset, and adding the term destroys that.
        //
        // The reason is worth writing down. If the onset sits at position p in the ring, the call
        // that reads it finds it at cross = p - (frames already read), and that call's fill is
        // lower by exactly the same amount. The two cancel: whichever call detects it, the answer
        // is the same. Adding the lead uncancels them, and a measurement that had settled to 0.2 ms
        // across runs went back to a 7.3 ms spread - the very lottery it was meant to remove.
        //
        // The lead DOES belong in the delay from capture to where the host finally places the
        // audio, which is a different quantity - see the note on internal_latency_frames() and
        // findings 2026-09-08 (5).
        // HOW LONG AGO THE LAST CAPTURE CALLBACK LANDED - CLAMPED, and it needs to be.
        //
        // Before the first callback the timestamp is 0, and the gap from 0 is the machine's entire
        // uptime; after the device stops delivering it grows without limit. Either way the figure
        // stops meaning "how far into the current device period we are" and starts poisoning the
        // reported latency - it put 151 ms on the panel and failed two checks that had passed for
        // weeks. In normal running this term never exceeds one device period, so anything past a
        // few of them is not a measurement, it is a device that has gone quiet.
        double sinceWrite = frames_between(lastWriteHostTime.load(), at);
        double sanest     = ((double)openDeviceFrames * 4.0) / nominalRatio;

        if ((lastWriteHostTime.load() == 0) || (sinceWrite > sanest)) {
            sinceWrite = 0.0;
        }

        return sinceWrite + (inRing / nominalRatio);
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
        // UNDER THE LOCK. hostRate, hostMaxFrames, observedMaxFrames and observedFrames are all read
        // by the worker while it holds this - minimum_setpoint_for() is built on the first two and
        // retune() on the second two - and the worker is running by the time a host calls this. VST3
        // guarantees the AUDIO thread is stopped here, which is why the same fields are safe to touch
        // from observe_block(); it guarantees nothing about a thread of the plug-in's own.
        lock_config_from_host("setupProcessing");

        hostRate = setup.sampleRate;
        eventRate.store(setup.sampleRate);

        // A DIFFERENT DECLARED MAXIMUM INVALIDATES THE OBSERVATION, and nothing else does. What was
        // learned about one host block size says nothing about another, so this is the one place
        // that forgets it - see close_capture_locked(), which used to.
        // NOT ON THE FIRST CALL, which would throw away a callback size just restored from the
        // project - setState() and setupProcessing() arrive in whichever order the host likes, and
        // this used to clear a good value simply for being the first to see a block size at all.
        // A LATER change of declared size is a genuine reconfiguration and does discard it.
        if ((hostMaxFrames != 0) && ((uint32)setup.maxSamplesPerBlock != hostMaxFrames)) {
            observedMaxFrames = 0;
            observedFrames    = 0;
            retuneState.store(eRetuneWatching);
        }
        hostMaxFrames = (uint32)setup.maxSamplesPerBlock;

        pthread_mutex_unlock(&configLock);

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
        // The block timeline starts again with the transport. Carrying it across a stop would only
        // matter for the first block after one - the model is behind real time by then and
        // re-anchors on its own - but starting clean says what is meant.
        if (state) {
            nextBlockHostTime = 0;
        }

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

        // BEFORE ANYTHING THAT SENDS, and once. Every MIDI byte this call produces - notes,
        // controllers, the measurement's own note and its panic - is stamped from this one instant,
        // so the whole stream stays in order however the host chops its blocks up.
        //
        // It is deliberately NOT the moment the block is heard - the host's output latency sits in
        // between - but that term cancels here and does not belong in the compensation: the audio
        // comes back through a ring read at the top of process() too, so both ends of the round
        // trip are anchored to the same clock and the difference between them is free of it.
        uint64_t blockHostTime = block_host_time(frames);

        // ONLY WHILE A DEVICE IS ACTUALLY RUNNING. Blocks handed over before one opens say nothing
        // about what the ring will have to cover, and a host - or a checker - that drives a burst of
        // them unpaced while nothing is open would leave a fictitious cycle length behind that the
        // retune then treats as settled for the rest of the session.
        if (running) {
            observe_burst(frames);
        }

        apply_parameter_changes(data, blockHostTime);

        if (instrument) {
            forward_events(data, blockHostTime);
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

        run_measurement(out, frames, blockHostTime, fill);

        {
            tGbStatus * status = gb_status(statusSlot);

            if (status != nullptr) {
                // WITH the callback lead, unlike the measurement's own subtraction. This figure
                // answers "how far behind the audio is where the host finally puts it", and the
                // frames already handed over inside this callback are part of that distance. The
                // measurement subtracts a round trip that is invariant to them - see the note on
                // latency_frames_measured().
                double actual = latency_frames_measured(fill, blockActualHostTime)
                                + (double)burstBefore;

                if (instrument) {
                    actual += (offsetMs.load() / 1000.0) * hostRate;
                }

                atomic_store(&status->actualSamples, (int)actual);

                // THE SAME NUMBER THE HOST WILL BE TOLD, averaged. Seeded rather than eased in from
                // zero: a fresh device would otherwise report a latency climbing out of nothing for
                // its first second, and every step of that is a compensation pass.
                double instant = actual - ((offsetMs.load() / 1000.0) * hostRate);
                double prior   = pipelineAvg.load();

                // BOUNDED BY THE ESTIMATE IT REPLACED, and this is not belt and braces.
                //
                // A measured figure can be wrong in ways a constant cannot. The ring's occupancy is
                // read live, the frames-so-far-this-callback depend on a cycle boundary being
                // detected correctly, and either can be disturbed - a mis-detected boundary
                // accumulates a "callback" of arbitrary length, and a ring that has just been
                // primed or snapped reads full. The result reached a host as 41 ms of plug-in
                // latency where the setpoint says 20, which is far worse than the 4-9 ms error this
                // whole exercise set out to remove.
                //
                // The pipeline genuinely cannot sit more than about one callback either side of
                // what the setpoint implies - the drift loop holds it there and the burst explains
                // the rest - so anything outside that band is a fault, not a measurement.
                double estimate = latency_frames_for_fill(setpointFrames);
                double allow    = (double)((observedMaxFrames > 0) ? observedMaxFrames : hostMaxFrames)
                                  + ((double)openDeviceFrames / ((nominalRatio > 0.0) ? nominalRatio : 1.0));

                if (instant > (estimate + allow)) {
                    instant = estimate + allow;
                } else if (instant < (estimate - allow)) {
                    instant = (estimate > allow) ? (estimate - allow) : estimate;
                }

                pipelineAvg.store((prior > 0.0)
                                  ? (prior + ((instant - prior) * GB_PIPELINE_SMOOTH))
                                  : instant);

                // NOTHING IS TOLD TO THE HOST FROM HERE ANY MORE. This used to raise a flag when
                // the measured pipeline drifted past the deadband, which is what drove the
                // reconfigure loop above: a latency change is an instruction to the host to
                // reactivate us. The measurement is for the panel and the log now, and the figure
                // the host is given changes only when something structural does - a device, a rate,
                // a buffer, a retune, the trim.
                atomic_store(&status->measureTripNow, measureTrip);
                atomic_store(&status->measurePhase, (int)measureState);
            }
        }
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

        // THE ALL-NOTES-OFF IS THE WORKER'S JOB, NOT THIS THREAD'S. It is 32 MIDISend calls - one
        // note-off and one All Notes Off on each of 16 channels - and every one of them is a mach
        // message to the MIDI server. Half a millisecond of a 2.7 ms block, spent on the audio
        // thread, at the one moment the ring must not be starved: it underran, the ring resynced,
        // and store_measurement() then threw the run away for happening over a resync.
        //
        // The settle phase exists precisely to let things go quiet, and it is 350 ms long - orders
        // of magnitude more than the worker needs to get to this.
        measurePanic.store(true);
        wake_worker();

        measureUnderrunsAtStart = atomic_load(&ring.underflows);
        measureResyncsAtStart    = resyncs.load();

        measureState       = eMeasureSettle;
        measureFrames      = 0;
        measurePeak        = 0.0f;
        measureFloor       = 0.0f;
        measureConfirm     = 0;
        measureOnsetFrames = 0;
        measureOnsetFill   = 0.0;
        measureOnsetOurs   = 0.0;
        measureNoteTime    = 0;
        measureTrip        = 0;

        for (int i = 0; i < GB_MEASURE_TRIPS; i++) {
            measureTrips[i] = 0;
        }
        measureLatency.store(0);
    }

    // WORKER THREAD. Everything hanging on every channel we might have used, silenced: our own
    // test note is released explicitly and anything left by a previous attempt - or by playing -
    // goes with it.
    //
    // Sent immediately rather than stamped, because this thread has no block timeline to stamp
    // against. A played note scheduled up to one block ahead could in principle be overtaken by it,
    // which is a stuck note; a panic pressed in the middle of playing is not a case worth carrying
    // machinery for, and the note that follows would clear it.
    void send_all_notes_off(void) {
        int destination = midiDestination.load();

        for (uint8_t channel = 0; channel < 16; channel++) {
            uint8_t note[3]  = { (uint8_t)(0x80 | channel), (uint8_t)testNote.load(), 0 };
            uint8_t panic[3] = { (uint8_t)(0xB0 | channel), 123, 0 };   // All Notes Off

            gb_midi_send(destination, note, 3);
            gb_midi_send(destination, panic, 3);
        }
    }

    void run_measurement(float ** out, int32 frames, uint64_t blockHostTime, double fill) {
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
            // QUIET, NOT MERELY ELAPSED - and it is what made four trips out of five disappear.
            //
            // Between trips this phase is waiting for the note just played to DECAY, and 0.35 s is
            // nowhere near enough for a piano or a pad. The floor was then taken over a still
            // ringing note, the threshold came out eight times too high, the next note could not
            // cross it, and the trip timed out - which ends the run. Every measurement rested on
            // one reading and the averaging was decoration.
            //
            // So: the settle ends when the input has actually gone quiet, with the old duration as
            // a MINIMUM and a hard ceiling so a noisy input cannot hang the run.
            bool quiet   = (peak < GB_MEASURE_MARGIN);
            bool settled = ((double)measureFrames >= (GB_MEASURE_SETTLE_S * hostRate)) && quiet;

            if (settled || ((double)measureFrames >= (GB_MEASURE_SETTLE_MAX_S * hostRate))) {
                measureState     = eMeasureFloor;
                measureFrames    = 0;
                measureFloorLate = false;
                measureFloor  = 0.0f;
            }

            return;
        }

        if (measureState == eMeasureFloor) {
            // Establish what silence looks like on this input before deciding what a note looks
            // like. A noisy preamp would otherwise register an onset immediately.
            //
            // THE TRAILING PEAK, NOT THE PEAK OVER THE WHOLE WINDOW. A preset with reverb on it is
            // still decaying through this phase, so a peak held from the start of the window is a
            // measurement of the tail rather than of the floor - and the threshold is eight times
            // whatever this says. Too high a threshold does not fail loudly; it fires LATE into the
            // attack, which reads as a slower synth, which the host then over-compensates for, and
            // the take records early. A Kronos on a reverbed preset came out 5 ms adrift of an
            // Analog Rytm on a dry kick, both reporting the same figure.
            //
            // Restarting the hold two thirds of the way through leaves the last third - the part
            // nearest the note, and the quietest part of any decay - as the answer.
            if ((double)measureFrames >= (GB_MEASURE_FLOOR_S * hostRate * 0.667)) {
                if (!measureFloorLate) {
                    measureFloorLate = true;
                    measureFloor     = 0.0f;
                }
            }

            if (peak > measureFloor) {
                measureFloor = peak;
            }

            if ((double)measureFrames >= (GB_MEASURE_FLOOR_S * hostRate)) {
                uint8_t note[3] = { 0x90, (uint8_t)testNote.load(), 100 };

                // This trip's own clean-capture baseline. See where it is checked.
                measureTripUnderruns = atomic_load(&ring.underflows);
                measureTripResyncs   = resyncs.load();

                // AT THE START OF THE NEXT BLOCK, which is the one instant here that is certain to
                // be in the FUTURE - nextBlockHostTime is this block's start plus its frames, and
                // the model never hands back a start earlier than the clock. CoreMIDI therefore
                // holds the packet and releases it exactly then, so the moment the note left is a
                // number this code knows rather than one it hopes for.
                //
                // Stamping it at the block start instead put it in the past by however long the
                // block had taken to compute - run_measurement() runs after the audio is rendered -
                // so it went out at once, at an instant nothing recorded. Tens of microseconds on
                // its own, but it was the reference EVERY later arithmetic was measured from.
                measureNoteTime = nextBlockHostTime;

                gb_midi_send_at(midiDestination.load(), note, 3, measureNoteTime);

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
            //
            // TIMED ON THE CLOCK AT BOTH ENDS, not counted in blocks - and this is what stopped
            // the figure jumping about from run to run.
            //
            // Counting elapsed FRAMES assumes blocks arrive evenly spaced, and under a host that
            // hands over four of them per audio callback they do not: they arrive in a burst and
            // then nothing for the rest of the cycle. So the answer depended on WHICH call of the
            // callback the note happened to be detected in - 0, 128, 256 or 384 frames of pure
            // artefact at a 512-frame cycle, which is up to 8 ms of jump between two runs measuring
            // the same unchanged synth.
            //
            // Two real instants remove it: the note left at measureNoteTime, and this block's ring
            // read happened at blockActualHostTime. The RAW clock, deliberately - the model is
            // ahead of it inside a burst, and the ring holds only what physically arrived.
            //
            // THE SAMPLE, NOT THE BLOCK, for the last part of it: which sample of this block first
            // crossed says how far into it the sound began - 0 to a full buffer, half of one on
            // average, and always SHORT if discarded. One extra pass over the block that crossed,
            // and only that one.
            if (measureConfirm == 1) {
                int32 cross = frames;

                for (int32 i = 0; (i < frames) && (cross == frames); i++) {
                    for (int c = 0; c < GB_CHANNELS; c++) {
                        float magnitude = (out[c][i] < 0.0f) ? -out[c][i] : out[c][i];

                        if (magnitude > threshold) {
                            cross = i;
                            break;
                        }
                    }
                }

                double elapsed = frames_between(measureNoteTime, blockActualHostTime)
                                 + (double)cross;

                // OUR SHARE IS WORKED OUT HERE, AT THE ONSET, not two blocks later when the
                // confirmation arrives. It is built from three things read at one instant - the
                // fill, the clock, and when the last capture callback landed - and by the time the
                // confirmation comes in, the device has usually written again: lastWriteHostTime is
                // then AFTER the onset block, the "how long ago" term collapses to zero, and the
                // share comes out a whole device buffer short. It read as an 18 ms synth.
                measureOnsetFrames = (elapsed > 0.0) ? (uint32_t)elapsed : 0;
                measureOnsetFill   = fill;
                measureOnsetOurs   = latency_frames_measured(fill, blockActualHostTime);
            }
        } else {
            measureConfirm = 0;
        }

        if (measureConfirm >= GB_MEASURE_CONFIRM) {
            uint8_t off[3] = { 0x80, (uint8_t)testNote.load(), 0 };

            gb_midi_send_at(midiDestination.load(), off, 3, blockHostTime);

            // Our own share is subtracted here, while it is unambiguously the share that was in
            // force during the measurement.
            uint32_t total = measureOnsetFrames;
            // internal_latency(), NOT report_latency() - see the comment on that pair. Netting off
            // the reported figure would subtract the correction already in force as well as our own
            // buffering, so each run would return less than the last.
            //
            // BUT FROM THE OCCUPANCY THE ONSET ACTUALLY CAME THROUGH, not from the setpoint. The
            // ring only sits AT its setpoint on average: a host that hands over four blocks per
            // audio callback drains it in a burst, so the fill at the fourth is a whole cycle below
            // the fill at the first, and which of them a note happened to land in moved the answer
            // by that much. That is where the run-to-run scatter came from. The setpoint remains
            // what the HOST is told, because that is the long-run figure; the measurement nets off
            // the real one, and the difference between the two is exactly the momentary deviation
            // it should not be reporting as hardware.
            // THE MEAN CALLBACK LEAD, ADDED AS A CONSTANT - which is the difference between the two
            // failed attempts at this and the right answer.
            //
            // The round trip is invariant to WHICH call of a callback detects the onset, because
            // the position in the ring and that call's fill move together and cancel. That is why
            // adding the per-block burstBefore made it worse: it uncancelled them and put the
            // block's own 0..384 frames of variance straight into the answer.
            //
            // But the AVERAGE of that term is not zero, and it is not in the subtraction at all -
            // so every measurement carried it as a constant bias. At a 512 frame callback delivered
            // in 128s that is (512 - 128) / 2 = 192 frames, 4 ms, and both a KRONOS and an Analog
            // Rytm recorded 4 ms early on exactly that. A constant has no variance, so this is the
            // one form of the term that removes the bias without reintroducing the scatter.
            double   lead    = mean_callback_lead();
            double   oursNow = ((measureOnsetOurs > 0.0) ? measureOnsetOurs
                               : internal_latency_frames()) + lead;
            uint32_t ours    = (oursNow > 0.0) ? (uint32_t)oursNow : 0;

            // A ROUND TRIP CANNOT BE FASTER THAN OUR OWN PIPELINE. If it looks like it was, the
            // onset is not the note - a threshold crossing on something else, or a ring that was
            // not at its setpoint when the arithmetic assumed it was. Reported rather than clamped
            // to zero: a silent 0 looked exactly like a device that had never been measured.
            int      theirs = (total > ours) ? (int)(total - ours) : GB_MEASURE_TOO_EARLY;

            // THE TRIP IS ONLY KEPT IF THE CAPTURE WAS CLEAN THROUGH IT. This used to be judged
            // over the whole run and thrown away wholesale - one resync anywhere and every note was
            // discarded, which on a rig that resyncs at all means no measurement is ever possible.
            // Per trip, a disturbed one simply does not vote.
            bool clean = (atomic_load(&ring.underflows) == measureTripUnderruns)
                         && (resyncs.load() == measureTripResyncs);

            measureTrips[measureTrip] = (clean && (theirs > 0)) ? theirs : GB_MEASURE_TOO_EARLY;
            measureTrip++;

            measureOnset.store((int)total);
            measureOurs.store((int)ours);
            measureFillSeen.store(measureOnsetFill);
            measureTriggerPeak.store(peak);
            measureFloorSeen.store(measureFloor);

            if (measureTrip < GB_MEASURE_TRIPS) {
                // Round again. Back to settle, which is where the note just played gets time to
                // decay before the next floor is taken - a floor measured over a ringing note sets
                // a threshold the next one cannot cross.
                measureState     = eMeasureSettle;
                measureFrames    = 0;
                measureConfirm   = 0;
                measureFloor     = 0.0f;
                measureFloorLate = false;
                return;
            }

            measureLatency.store(trip_result());
            measureState = eMeasureIdle;
            measureStore.store(true);
            wake_worker();
            return;
        }

        if ((double)measureFrames >= (GB_MEASURE_TIMEOUT_S * hostRate)) {
            uint8_t off[3] = { 0x80, (uint8_t)testNote.load(), 0 };

            gb_midi_send_at(midiDestination.load(), off, 3, blockHostTime);

            // A TRIP THAT HEARD NOTHING ENDS THE RUN. Something is wrong with the destination, the
            // channel or the note, and four more silences take six more seconds to say so.
            measureLatency.store((measureTrip > 0) ? trip_result() : GB_MEASURE_TIMED_OUT);
            measureState = eMeasureIdle;
            measureStore.store(true);
            wake_worker();
        }
    }

    // The trimmed mean of the trips that voted. Sorted in place - five elements, on the audio
    // thread, and an insertion sort of five is nothing beside the block it sits in.
    int trip_result(void) {
        int valid[GB_MEASURE_TRIPS];
        int count = 0;

        for (int i = 0; i < measureTrip; i++) {
            if (measureTrips[i] > 0) {
                valid[count++] = measureTrips[i];
            }
        }

        if (count == 0) {
            return GB_MEASURE_TOO_EARLY;
        }

        for (int i = 1; i < count; i++) {
            int key = valid[i];
            int j   = i - 1;

            while ((j >= 0) && (valid[j] > key)) {
                valid[j + 1] = valid[j];
                j--;
            }
            valid[j + 1] = key;
        }

        // Trim only when there is enough left to be worth averaging. Three votes still give a
        // middle one; two would leave nothing after dropping both ends.
        int drop  = (count >= (2 * GB_MEASURE_DROP) + 1) ? GB_MEASURE_DROP : 0;
        int total = 0;
        int used  = 0;

        for (int i = drop; i < (count - drop); i++) {
            total += valid[i];
            used++;
        }

        measureTripsUsed.store(used);
        measureTripLow.store(valid[0]);
        measureTripHigh.store(valid[count - 1]);
        measureTripSpread.store((count > 1) ? (valid[count - 1] - valid[0]) : 0);

        return (used > 0) ? (total / used) : GB_MEASURE_TOO_EARLY;
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

    // WHERE THIS BLOCK BEGINS IN WALL TIME - AND IT IS NOT "NOW".
    //
    // A host is entitled to hand over several blocks per audio callback, and the one this is built
    // against does: Ableton declares 512 and calls with 128, which is FOUR process() calls computed
    // back to back inside one 512-frame cycle, microseconds apart. Reading the clock at the top of
    // each and calling it the block's start time gives all four nearly the same answer - so blocks
    // two, three and four are stamped up to 384 frames early, and worse, their events INTERLEAVE
    // with the previous block's: a note at offset 0 of the second call is truly 128 frames after a
    // note at offset 127 of the first, but comes out stamped 127 frames BEFORE it. CoreMIDI
    // delivers in timestamp order, so that is a note-off overtaking its note-on. It sounds exactly
    // as bad as it reads.
    //
    // THE FRAMES THEMSELVES CARRY THE PACING. A block starts where the frames already handed over
    // run out, so the model is one addition: carry the end of the last block forward and use it,
    // unless real time has already passed it - which is what a new device cycle looks like, and
    // what a late callback looks like too. Nothing has to classify cycles or measure a buffer.
    //
    // MidiSyncTool reached the same place from the other end (its findings, 2026-09-02: Live splits
    // a buffer at a loop wrap into 500 frames and then 12). Its version has to identify the cycle
    // because it needs the cycle's own length for its telemetry; this one does not.
    //
    // THE CAP IS FOR A HOST THAT RUNS FASTER THAN REALTIME. An offline bounce hands over blocks as
    // fast as it can compute them, and an unbounded model would schedule MIDI further and further
    // into the future. A quarter of a second is far longer than any single device cycle and far
    // shorter than a bounce takes to run away.
    uint64_t block_host_time(int32 frames) {
        uint64_t now   = AudioGetCurrentHostTime();

        // THE RAW CLOCK IS KEPT AS WELL AS THE MODEL, and the two are not interchangeable. Outgoing
        // MIDI is stamped on the MODEL, because that is where a note belongs musically. Anything
        // measured about the audio COMING BACK has to use the raw one, because the ring holds what
        // physically arrived by this instant - and for the second, third and fourth call of a
        // callback the model is up to a whole cycle ahead of it. See run_measurement().
        blockActualHostTime = now;
        uint64_t start = now;
        double   rate  = eventRate.load();

        // THE CYCLE BOUNDARY, decided here because both users need the same answer: this is where
        // the model is allowed to carry forward, and it is what sizes the ring in observe_burst().
        //
        // Two calls belong to one callback if the second arrives before half of the first could
        // have been played. Back to back they are microseconds apart; a real boundary is a whole
        // block. Three orders of magnitude between the regimes, so the threshold is not delicate.
        double gap = frames_between(lastCallHostTime, now);

        blockStartsCycle = (lastCallHostTime == 0) || (gap > ((double)lastCallFrames * 0.5));
        lastCallHostTime = now;
        lastCallFrames   = (uint32_t)frames;

        // MONOTONIC, AND THAT IS THE WHOLE POINT OF IT.
        //
        // The model may never hand back a time earlier than the end of the block before it. Letting
        // it re-anchor to the clock at every callback boundary looks right - the clock is the truth,
        // after all - and it inverts the event stream: audio callbacks jitter by a few hundred
        // microseconds, so a callback arriving EARLY gets a first stamp before the previous
        // callback's last one. CoreMIDI delivers in timestamp order, so a note-off can be handed
        // over ahead of the note-on it belongs to. Notes stick on, and the next ones are lost
        // behind them - which from the keyboard looks like the plug-in has stopped passing MIDI
        // through altogether, while a quantised clip, whose events are sparse and far apart, plays
        // perfectly.
        //
        // So: carry forward, always, and let the ceiling below be the only thing that pulls it back.
        // A block boundary that jitters early simply waits; the grid does not move.
        if (nextBlockHostTime > now) {
            start = nextBlockHostTime;
        }

        // A HARD CEILING ON TOP OF THE RULE, because the rule is a heuristic and this is not.
        //
        // Within a callback the model is legitimately ahead of the clock, and by definition never
        // by more than one callback's worth of frames. If it ever is, the boundary test has missed
        // one - a host rendering faster than realtime hands over call after call with no gap
        // between them, and every one of them reads as a continuation. The model then walks into
        // the future without limit, and MIDI stamped from it is not late, it is GONE: CoreMIDI
        // holds each packet until a time that may be minutes away. Nothing sounds, and the symptom
        // is "the plug-in stopped passing notes through" with everything else working perfectly.
        //
        // Two callbacks of slack, so ordinary jitter never touches it, and falling back to the
        // clock when it trips. The cost of the ceiling being wrong is one callback of MIDI timing;
        // the cost of no ceiling is silence.
        //
        // It is also what makes the monotonic rule above safe. Never going backwards on its own
        // would let one fast burst push the grid permanently into the future; never going forwards
        // too far on its own inverts the stream. Together they say: follow the frames, and if that
        // has drifted implausibly far from the clock, admit it and start again.
        // GENEROUS, DELIBERATELY. The ceiling is a last resort against a runaway, not a tuning
        // knob, and tripping it IS a backwards step - the one thing the rule above exists to
        // prevent. Set it at twice the measured callback it came out at 256 frames while the host
        // was handing over bursts of 512, so it tripped on every burst and inverted the stream it
        // was supposed to protect.
        //
        // A host taking more than eight of its own declared blocks in one callback is pathological,
        // and the measured burst is not always available - observedMaxFrames is only collected
        // while a device is running, and MIDI flows whether one is or not.
        double cycle = (double)hostMaxFrames * 8.0;

        if ((double)observedMaxFrames * 2.0 > cycle) {
            cycle = (double)observedMaxFrames * 2.0;
        }

        if (cycle < 4096.0) {
            cycle = 4096.0;
        }

        if (rate > 0.0) {
            uint64_t ceiling = now + AudioConvertNanosToHostTime(
                                   (uint64_t)((cycle / rate) * 1.0e9));

            if (start > ceiling) {
                start = now;
            }
        }

        // AND IT HAS TO COME BACK, WITHOUT EVER GOING BACKWARDS.
        //
        // The model advances by the host's NOMINAL frame count while the clock advances at the
        // device's real rate, and no two crystals agree - so a model that only ever carries forward
        // creeps ahead by a few parts per million for as long as the session lasts. Left alone it
        // walks into the ceiling, and the ceiling resets to the clock, which is the one backwards
        // step this whole arrangement exists to avoid.
        //
        // So when it is further ahead than a callback can account for, each block advances by a
        // shade LESS than its own length. Two per cent of a block is 53 microseconds at 128 frames
        // - inaudible on any single block, and it closes a whole callback of excess inside half a
        // second. The advance stays positive, so the stream stays monotonic while it converges.
        double advance = (double)frames;

        if (rate > 0.0) {
            double ahead = frames_between(now, start);
            double allow = (observedMaxFrames > 0) ? (double)observedMaxFrames : (double)hostMaxFrames;

            if (ahead > allow) {
                double shave = (ahead - allow);
                double most  = advance * 0.02;

                advance -= (shave > most) ? most : shave;
            }
        }

        nextBlockHostTime = (rate > 0.0)
                            ? start + AudioConvertNanosToHostTime(
                                  (uint64_t)((advance / rate) * 1.0e9))
                            : 0;

        blockHostTimeNow = start;

        return start;
    }

    // HOW FAR INTO A CALLBACK AN AVERAGE BLOCK SITS. A host that hands over one block per callback
    // has none of this; one that delivers 512 in four 128s has a block starting 0, 128, 256 or 384
    // frames in, averaging 192. Audio thread only, and both figures it reads are audio-thread state.
    double mean_callback_lead(void) const {
        double burst = (double)observedMaxFrames;
        double call  = (double)lastCallFrames;

        return (burst > call) ? ((burst - call) / 2.0) : 0.0;
    }

    // How many of the host's frames separate two host-clock instants. 0 if they are the wrong way
    // round, which is what a caller wants everywhere it is used here.
    double frames_between(uint64_t from, uint64_t to) const {
        double rate = eventRate.load();

        if ((to <= from) || (rate <= 0.0)) {
            return 0.0;
        }

        return ((double)AudioConvertHostTimeToNanos(to - from) / 1.0e9) * rate;
    }

    // WHERE AN EVENT BELONGS IN WALL TIME, from where it belongs in the block. Offset 0 is the top
    // of this block, which is now; anything later is that many frames into the future and CoreMIDI
    // will hold it until then.
    uint64_t host_time_for(uint64_t blockHostTime, int32 sampleOffset) const {
        double rate = eventRate.load();

        if ((sampleOffset <= 0) || (rate <= 0.0)) {
            return blockHostTime;
        }

        double nanos = ((double)sampleOffset / rate) * 1.0e9;

        return blockHostTime + AudioConvertNanosToHostTime((uint64_t)nanos);
    }

    void forward_events(ProcessData & data, uint64_t blockHostTime) {
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

            // STAMPED WITH ITS OWN OFFSET, not sent on arrival. See gb_midi_send_at(): a block's
            // worth of notes fired at the block boundary is a bias of half a buffer, always early,
            // and it grows with every increase in the host's buffer size.
            // COUNTED, BOTH SIDES. "Notes are not getting through" has three quite different
            // causes - the host is not delivering them, the plug-in is dropping them, or the send
            // is failing - and from outside they look identical. The panel shows in/out, so one
            // glance says which half to look at.
            eventsIn.fetch_add(1);

            if (gb_midi_send_at(destination, message, 3,
                                host_time_for(blockHostTime, event.sampleOffset))) {
                eventsOut.fetch_add(1);
            }
        }
    }

    // Back from a normalised parameter to the wire.
    // ON THE SAME CLOCK AS THE NOTES, and that is not a nicety. Half a stream stamped into the
    // future and half sent immediately is a stream that arrives out of ORDER - CoreMIDI delivers by
    // timestamp, so an "immediate" controller overtakes every note still waiting for its offset.
    // A bend that lands before the note it belongs to is heard as a glitch, not as a timing error.
    void send_controller(ParamID id, ParamValue value, uint64_t hostTime) {
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

            gb_midi_send_at(destination, message, 2, hostTime);
            return;
        }

        if (controller == kPitchBend) {
            // FOURTEEN BITS, centred at 8192 - the one controller that is not a 0..127 byte, and
            // the one a keyboard player notices immediately if it is wrong.
            int bend = (int)((value * 16383.0) + 0.5);

            message[0] = (uint8_t)(0xE0 | channel);
            message[1] = (uint8_t)(bend & 0x7F);
            message[2] = (uint8_t)((bend >> 7) & 0x7F);

            gb_midi_send_at(destination, message, 3, hostTime);
            return;
        }

        if (controller > 127) {
            return;
        }

        message[0] = (uint8_t)(0xB0 | channel);
        message[1] = (uint8_t)controller;
        message[2] = (uint8_t)((value * 127.0) + 0.5);

        gb_midi_send_at(destination, message, 3, hostTime);
    }

    // WHAT THE HOST TAKES PER CALLBACK, NOT PER CALL - and getting this wrong was a glitch you could
    // hear.
    //
    // The ring has to cover the largest single demand made on it, and that demand is a whole audio
    // callback. Ableton declares 512, calls with 128, and takes all four back to back. The retune
    // read "the largest block actually seen" as 128 and shrank the setpoint to suit, so the ring
    // held 400 frames against a callback that drained 512 of them. Every cycle ran it dry: 45
    // underruns and 44 resyncs in two seconds on a KRONOS at a 64 frame device buffer, one audible
    // click each. It survived a 512 frame device buffer only because the setpoint then came out at
    // 960 by accident, which is more than one callback - which is why this never showed up until
    // someone ran a small buffer.
    //
    // The DECLARED maximum was right all along. What is measured here is not a correction to it so
    // much as a confirmation, and on a host that genuinely uses less than it declares - one block
    // per callback, smaller than declared - it still reclaims the difference.
    //
    // CALLED BEFORE ANY EARLY RETURN, unlike observe_block(). Sitting inside that one, it saw
    // nothing at all while the ring was resyncing - which is exactly when the numbers are needed,
    // and is why the first attempt at this fix changed nothing.
    //
    // The boundary itself is decided in block_host_time(); this only adds up what fell inside one.
    void observe_burst(int32 frames) {
        // CLAMPED TO ONE CALLBACK. burstFrames only resets when a cycle boundary is detected, so a
        // boundary that is missed accumulates without limit - and this figure feeds the reported
        // latency. Nothing can legitimately have handed over more than a callback's worth before
        // the current call, by definition of what a callback is.
        burstBefore = blockStartsCycle ? 0 : burstFrames;

        if ((observedMaxFrames > 0) && (burstBefore > observedMaxFrames)) {
            burstBefore = observedMaxFrames;
        }

        if (blockStartsCycle) {
            // A CEILING, because a detection that misfires would otherwise stick for the session.
            // Nothing legitimately takes more than a few of its declared blocks per callback, and
            // sizing a ring for a figure this side of that costs a few milliseconds where trusting
            // a runaway one costs seconds.
            uint32_t ceiling = (hostMaxFrames > 0) ? (hostMaxFrames * 4) : 8192;

            if ((burstFrames > observedMaxFrames) && (burstFrames <= ceiling)) {
                observedMaxFrames = burstFrames;
            }

            burstFrames = (uint32_t)frames;
        } else {
            burstFrames += (uint32_t)frames;
        }
    }

    void observe_block(int32 frames) {
        // A RING TOO SMALL IS FIXED AT ONCE, AND NOT ONLY DOWNWARDS.
        //
        // The retune below exists to RECLAIM frames once the host has been watched, and it is
        // deliberately slow and one-way about it. This is the opposite case and it cannot wait: a
        // ring sized for less than one callback is drained dry every cycle, which is an audible
        // click each time - and there was no path to it except revert_retune(), which only fires
        // AFTER an underrun has already been heard.
        //
        // It reaches here because neither number available at the first open is right. CT's Live
        // DECLARES 256 and hands over 512 in one callback; the declaration under-states, and the
        // observation is still partial during a project load - which is the least undisturbed
        // moment there is and the one where a busy CPU splits a burst in two. Both said 256, the
        // ring came up at 560, and only switching the buffer away and back settled it at 880.
        //
        // So: whenever the burst actually seen needs more ring than is in force, ask for it. Once
        // per increase, since observedMaxFrames only ever grows.
        if (running && (observedMaxFrames > 0) && (retuneState != eRetuneRequested)) {
            double needed = minimum_setpoint_for(observedMaxFrames) * GB_AUTO_MARGIN;

            if (needed > (setpointFrames + GB_RETUNE_MIN_GAIN)) {
                retuneState = eRetuneRequested;
                wake_worker();
                return;
            }
        }

        if (retuneState != eRetuneWatching) {
            return;
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

    void apply_parameter_changes(ProcessData & data, uint64_t blockHostTime) {
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
                    // NOT LOGGED FROM HERE. log_line() opens and closes the file on every call, and
                    // this is the audio thread - three syscalls in the middle of a 2.7 ms block, at
                    // the exact moment a measurement is about to start. It showed up as a resync
                    // during the run and the run then discarded itself for not being clean: a
                    // measurement failing because of the act of measuring. The worker logs it.
                    start_measurement();
                }

                measureArmed = (value >= 0.5);
                continue;
            }

            ParamID id = q->getParameterId();

            if ((id >= GB_CC_BASE) && (id < (GB_CC_BASE + GB_CC_COUNT))) {
                if (instrument) {
                    // The point's own offset, exactly as a note gets its own - a controller move
                    // inside a block belongs where the host put it.
                    send_controller(id, value, host_time_for(blockHostTime, offset));
                }

                continue;
            }

            if (q->getParameterId() == kParamTrim) {
                trimGain.store((float)(value * 2.0));
            } else if (q->getParameterId() == kParamDevice) {
                int wanted = gb_device_slot(value);

                if (wanted != wantedDevice.load()) {
                    // THE FIRST ONE AFTER A RESTORE IS THE HOST'S, NOT THE USER'S, and that
                    // distinction is the whole fix. A host saves this parameter as a SLOT INDEX, and
                    // an index is a position in a list that changes shape the moment a device is
                    // unplugged - so the value restored with the project names whatever has moved
                    // into that position, which on this machine is a Continuity microphone. Honour
                    // the saved UID for that first value and let every later change through: a
                    // later change can only have come from the user or from automation, and both
                    // are deliberate.
                    if (deviceParamSeen.exchange(true) == true) {
                        savedDevicePending.store(false);
                    }
                    wantedDevice.store(wanted);
                    request_device();       // signals the worker; opens nothing on this thread
                }
            } else if (q->getParameterId() == kParamMidiDest) {
                int wanted = (int)(value * (double)(GB_MIDI_SLOTS - 1) + 0.5);

                midiDestination.store(wanted);
                // NO SHARED BUFFER. This used to snprintf the destination's name into a member
                // char array - on the AUDIO THREAD - which getState() then read from the host's
                // thread and parse_state() wrote from it. The name is derivable from the atomic
                // above wherever it is actually wanted (current_midi_name()), so the buffer that
                // was being raced over simply does not need to exist.
            } else if (q->getParameterId() == kParamMidiChannel) {
                midiChannel.store((int)(value * (double)(GB_CHANNEL_SLOTS - 1) + 0.5));
            } else if (q->getParameterId() == kParamTestNote) {
                int note = (int)((value * 127.0) + 0.5);

                testNote.store((note < 0) ? 0 : ((note > 127) ? 127 : note));
                continue;
            } else if (q->getParameterId() == kParamOffsetMs) {
                offsetMs.store(GB_OFFSET_MIN_MS + (value * (GB_OFFSET_MAX_MS - GB_OFFSET_MIN_MS)));

                // The correction is part of the reported figure, so the host has to be told - but
                // NOT on this click. See GB_OFFSET_SETTLE_MS: the worker waits for the value to stop
                // moving and then tells it once, because each telling costs the host a full delay
                // compensation pass and this control is dialled in a dozen clicks at a time.
                //
                // Nothing is computed here. What the latency becomes depends on state the worker
                // owns, and this runs on the audio thread before process() has even taken its
                // trylock, so the whole decision belongs on the other side of the queue.
                lastOffsetChangeMs.store(now_ms());
                offsetDirty.store(true);
                wake_worker();
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

        // WHEN THE NEWEST FRAME IN THE RING GOT HERE. Without it the ring's occupancy cannot be
        // turned into an age: see latency_frames_measured().
        self->lastWriteHostTime.store(AudioGetCurrentHostTime());

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

    static double now_ms(void) {
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1.0e6);
    }

    void request_device(void) {
        lastDeviceRequestMs.store(now_ms());
        deviceDirty.store(true);
        wake_worker();
    }

    // The panel reads this out of the shared block rather than being messaged, like every other live
    // figure - see gbStatus.h.
    void publish_waiting(bool waiting, const char * name) {
        tGbStatus * status = gb_status(statusSlot);

        if (status == NULL) {
            return;
        }

        // NAME FIRST, FLAG SECOND. The panel only reads the name while the flag is up, so writing
        // them in this order means it can never show "waiting for" against a stale name.
        if (waiting && (name != NULL)) {
            snprintf(status->waitingName, sizeof(status->waitingName), "%s", name);
        }
        atomic_store(&status->waitingForDevice, waiting ? 1 : 0);
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

            // SETTLE BEFORE ACTING - see GB_DEVICE_SETTLE_MS. Waiting here rather than in the
            // parameter handler keeps every route to a device change on one path: the panel's
            // arrows, the host's generic control and an automation lane all arrive as
            // request_device(), and all of them get coalesced by the same clock.
            //
            // deviceDirty is not consumed until the wait is over, so a request arriving mid-wait
            // simply pushes the deadline out rather than being lost or acted on twice.
            while (deviceDirty.load() && !workerQuit.load()) {
                double waited = now_ms() - lastDeviceRequestMs.load();

                if (waited >= GB_DEVICE_SETTLE_MS) {
                    break;
                }
                usleep((useconds_t)((GB_DEVICE_SETTLE_MS - waited) * 1000.0));
            }

            if (!workerQuit.load() && deviceDirty.exchange(false)) {
                reconfigure();
            }

            if (retuneState.load() == eRetuneRequested) {
                retune();
            }

            if (revertWanted.exchange(false)) {
                revert_retune();
            }

            if (measurePanic.exchange(false)) {
                log_line("measure requested");
                send_all_notes_off();
            }

            if (measureStore.exchange(false)) {
                store_measurement();
            }

            // SETTLE THE OFFSET BEFORE ACTING ON IT, the same shape as the device debounce above and
            // for the same reason: a burst of clicks should cost the host one delay-compensation
            // pass, not one per click. offsetDirty is not consumed until the wait is over, so a
            // click arriving mid-wait pushes the deadline out instead of being lost.
            while (offsetDirty.load() && !workerQuit.load()) {
                double waited = now_ms() - lastOffsetChangeMs.load();

                if (waited >= GB_OFFSET_SETTLE_MS) {
                    break;
                }
                usleep((useconds_t)((GB_OFFSET_SETTLE_MS - waited) * 1000.0));
            }

            if (!workerQuit.load() && offsetDirty.exchange(false)) {
                uint32 nowLatency = snapshot_latency();

                if (nowLatency != reportedLatency) {
                    reportedLatency = nowLatency;
                    latencyDirty.store(true);
                }
            }

            // THE MEASURED PIPELINE HAS MOVED FAR ENOUGH TO SAY SO. The reported figure follows a
            // live average now rather than a constant, so something has to notice when it has
            // drifted past the deadband - a device swap and a ring that has finally settled both
            // land here. The audio thread raises the flag; this is the only place that acts on it.
            if (latencyDirty.load()) {
                // WHERE THE NUMBER COMES FROM, every time it changes. A total on its own starts an
                // argument that only the breakdown can settle - "43.5 ms" is a ring, a device
                // buffer, a resampler and a measured round trip, and which of them is the big one
                // decides what to do about it. Twice now a figure has been questioned and the
                // components were only on the panel, where they cannot be pasted into a message.
                double ratio    = (snapRatio.load() > 0.0) ? snapRatio.load() : 1.0;
                double perMs    = (snapHostRate.load() > 0.0) ? (snapHostRate.load() / 1000.0) : 48.0;
                double ringPart = snapSetpoint.load() / ratio;
                double devPart  = (double)snapDeviceLatency.load() / ratio;
                double filtPart = resampler_latency_frames() / ratio;
                double offPart  = (offsetMs.load() / 1000.0) * snapHostRate.load();

                log_line("latency %u smp (%.1f ms) = pipeline %.0f (setpoint would say %.0f: ring "
                         "%.0f + device %.0f + filter %.0f) + measured %.0f (%.1f ms)",
                         reportedLatency.load(), (double)reportedLatency.load() / perMs,
                         pipelineAvg.load(), ringPart + devPart + filtPart,
                         ringPart, devPart, filtPart, offPart, offPart / perMs);
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
    // Set by reconfigure() around its close/open pair; see close_capture_locked(). Worker only.
    AudioObjectID keepSettingsFor{0};

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
        // THE SAVED DEVICE FIRST while a restore is still pending, whatever slot the parameter
        // names. Resolving it also puts the parameter right, so the panel and the host stop
        // disagreeing with what is actually open.
        if (savedDevicePending.load() && !deviceSelector.empty()) {
            found = device_find(deviceSelector.c_str(), true, &chosen);

            if (found) {
                savedDevicePending.store(false);
                publish_waiting(false, nullptr);

                int slot = gb_slot_for_uid(chosen.uid);

                if (slot >= 0) {
                    wantedDevice.store(slot);
                    index = slot;
                    send_message("gbDeviceSlot", slot);
                }
                log_line("saved device '%s' present - opening it", chosen.name);
            } else {
                // Device not present, don't modify deviceSelector - preserve the saved intent.
                // publish_waiting(true, savedDeviceName.empty() ? deviceSelector.c_str()
                //                                               : savedDeviceName.c_str());
                log_line("saved device '%s' not present - waiting (not modifying deviceSelector)",
                         savedDeviceName.empty() ? deviceSelector.c_str() : savedDeviceName.c_str());
            }
        } else if (index >= 0) {
            publish_waiting(false, nullptr);
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

        // What close_capture_locked() must NOT hand back, because we are about to reopen it. Zero
        // when the reconfigure is a genuine device change or a close, where the restore is right.
        keepSettingsFor = found ? chosen.id : 0;

        double heldFrom = now_ms();
        pthread_mutex_lock(&configLock);
        close_capture_locked();
        keepSettingsFor = 0;

        if (found) {
            // Recorded only on SUCCESS. Writing it before the attempt meant a device that failed to
            // open still became the thing the project saved and reopened with.
            if (open_capture_locked(chosen)) {
                deviceSelector = chosen.uid;
                appliedDevice  = index;
            } else {
                running = false;
                log_line("open failed - selection unchanged, staying closed");
            }
        }

        uint32 nowLatency = running ? report_latency() : 0;
        double held       = now_ms() - heldFrom;

        pthread_mutex_unlock(&configLock);

        // THE OTHER HALF OF THE SPINNING CURSOR. This is how long the worker owned the lock that
        // every host-thread entry point waits for; lock_config_from_host() reports the wait from
        // the other side. The two together say whether a beachball was this plug-in's doing.
        if (held >= 20.0) {
            // BROKEN DOWN, because "it held the lock for 1441 ms" says a refactor is needed and not
            // WHICH of the three things inside it to move first.
            log_line("reconfigure held configLock for %.0f ms (close+probe %.0f, rate+buffer %.0f, "
                     "open %.0f) - anything the host asked of this plug-in on its own thread waited "
                     "behind it", held,
                     (gPhaseIdle > heldFrom) ? (gPhaseIdle - heldFrom) : 0.0,
                     (gPhaseOpen > gPhaseProps) ? (gPhaseOpen - gPhaseProps) : 0.0,
                     (gPhaseOpen > 0.0) ? (now_ms() - gPhaseOpen) : 0.0);
        }

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
    // SLOT 0 IS "NONE", and every real device sits one higher.
    //
    // Before this there was no way to express "nothing", which made two different states share one
    // value: a fresh instance that had chosen nothing and an instance that had chosen the first
    // device both read 0. The panel showed that as the first device's name while its own header said
    // "no device selected" - the plug-in contradicting itself in two lines of the same window - and
    // there was no way back to nothing once a device had been picked.
    //
    // Devices moved UP by one rather than "None" being bolted on at the end, because 0 is the value
    // a VST3 parameter defaults to and the value a host restores when a project names no device. It
    // costs nothing on load: setComponentState() resolves a saved project by its device UID and
    // recomputes the parameter from that, so a saved set still reopens on the device it named. What
    // does shift is a recorded AUTOMATION lane of the device parameter, which would now name the
    // device one place earlier - accepted deliberately at v0.1.0 for a setup control nobody
    // automates.
    static bool resolve_slot(const tDeviceInfo * list, uint32_t count, int slot, tDeviceInfo * out) {
        uint32_t seen = 1;      // slot 0 is None, so the first real device is slot 1

        if (slot <= 0) {
            return false;       // None: nothing to open, and reconfigure() closes what is open
        }

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
        publish_config_snapshot();
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

        // Reads both ways now: reclaiming frames when the host uses less than it declared, and
        // CLAIMING them when it hands over more per callback than either number suggested.
        log_line("retuned %s: host declares %u and takes %u per callback - setpoint %.0f -> %.0f, "
                 "latency %u -> %u",
                 (setpointFrames > before) ? "UP - the ring was smaller than one callback" : "down",
                 hostMaxFrames, observedMaxFrames, before, setpointFrames, reportedLatency.load(),
                 nowLatency);

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

        // JUDGED PER TRIP NOW, not over the whole run. A resync anywhere used to throw away every
        // note in the run, so on a rig that resyncs at all no measurement was ever possible - and
        // the resync was often caused by the act of measuring. A disturbed trip simply does not
        // vote; the run is only refused when nothing clean survived.
        if ((result >= 0) && (measureTripsUsed.load() <= 0)) {
            log_line("measurement discarded: no trip completed over a clean capture "
                     "(%u underruns, %d resyncs during the run) - try again", underruns, resynced);

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
        // EVERY TRIP, NOT JUST THE SUMMARY. An average and a range still hide the shape: five
        // readings clustered with one wild outlier, and five spread evenly, produce the same two
        // numbers and mean quite different things. Safe to read here - the run is idle by the time
        // the worker gets to this, so the array is not being written.
        char trips[128];
        int  at = 0;

        trips[0] = '\0';

        for (int i = 0; (i < measureTrip) && (at < (int)sizeof(trips) - 12); i++) {
            at += snprintf(&trips[at], sizeof(trips) - (size_t)at, "%s%.1f",
                           (i == 0) ? "" : "/",
                           (measureTrips[i] > 0)
                           ? ((double)measureTrips[i] / (hostRate / 1000.0)) : -1.0);
        }

        log_line("measured: %d of %d trips used [%s], range %.1f..%.1f ms, spread %d frames (%.1f ms). "
                 "last onset %d, our "
                 "share %d (ring %.0f of %.0f), hardware %d (%.1f ms). floor %.4f, triggered at "
                 "%.4f, underruns %u resyncs %d during. '%s' -> '%s'",
                 measureTripsUsed.load(), GB_MEASURE_TRIPS, trips,
                 (double)measureTripLow.load() / (hostRate / 1000.0),
                 (double)measureTripHigh.load() / (hostRate / 1000.0),
                 measureTripSpread.load(),
                 (double)measureTripSpread.load() / (hostRate / 1000.0),
                 measureOnset.load(), measureOurs.load(),
                 measureFillSeen.load(), setpointFrames, result,
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

    // No configLock here, and it must stay that way - it is called from the worker's idle path and
    // from store_measurement() after that has released the lock. So it publishes only what the
    // snapshot already holds; the snapshot itself is refreshed by whoever changed the config, under
    // the lock they were already holding.
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

        // FROM THE SNAPSHOT, not the fields. This is called both with configLock held (from the
        // open and from retune) and without it (from the worker's idle publish), so it cannot take
        // the lock and must not read anything the lock protects.
        double setpoint = snapSetpoint.load();
        double ratio    = snapRatio.load();

        if (ratio <= 0.0) {
            ratio = 1.0;
        }

        atomic_store(&status->ringSamples, (int)(setpoint / ratio));
        atomic_store(&status->deviceSamples, (int)((double)snapDeviceLatency.load() / ratio));
        atomic_store(&status->filterSamples, (int)(resampler_latency_frames() / ratio));
        atomic_store(&status->measuredSamples, (int)hardwareSamples);
        atomic_store(&status->measuredLow, measureTripLow.load());
        atomic_store(&status->measuredHigh, measureTripHigh.load());
        atomic_store(&status->measuredTrips, measureTripsUsed.load());
        atomic_store(&status->offsetSamples, (int)((offsetMs.load() / 1000.0) * snapHostRate.load()));
        atomic_store(&status->latencySamples, (int)snapshot_latency());
        atomic_store(&status->recommendedFrames, recommendedSetpoint);
        atomic_store(&status->eventsIn, (int)eventsIn.load());
        atomic_store(&status->eventsOut, (int)eventsOut.load());
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

        // SAY SO IF THE REQUEST WAS TRIMMED. The clamp above is the right behaviour - capturing the
        // nearest thing that exists beats refusing to open - but on its own it left the panel and
        // the host showing a channel the device does not have while a different one was being
        // captured. The editor's arrows cannot reach an impossible value any more; a DEVICE CHANGE
        // still can, because the parameter is global to the instance while the channel setting is
        // per device. Same message the device slot already uses on the same kind of mismatch.
        if (first != settings->firstChannel) {
            settings->firstChannel = first;
            send_message("gbFirstChannel", (int)first);
        }

        // Same again for the width. A one-channel device cannot give a stereo pair, and the capture
        // callback widens the single channel rather than failing - but the panel must not go on
        // saying "Stereo" over a mono capture.
        if (wantCount != settings->captureChannels) {
            settings->captureChannels = wantCount;
            send_message("gbMode", (int)((wantCount > 1) ? 1 : 0));
        }
        captureChannels = wantCount;

        // LEAVE A DEVICE SOMEBODY ELSE IS DRIVING ALONE.
        //
        // Rate and buffer size are global to the device, so setting either reaches into every other
        // client of it - the host included. The default has always been to touch neither, but that
        // protection ended the moment a size was picked in the panel, and the case where it matters
        // most is the easiest to walk into: a mixer serving as the host's OWN output and as this
        // plug-in's capture source. There the device's buffer frame size IS the host's block size,
        // so imposing one is the plug-in setting its own process() call rate on hardware it does not
        // own - and the smaller the size, the harder every subsequent device change becomes.
        //
        // Probed BEFORE device_open() below, so what it reports is other clients and never our own
        // stream. Reported rather than silently obeyed: the panel setting is still whatever the user
        // chose, and the log says why the device did not take it.
        // OUR OWN GHOST FIRST. CoreAudio tears an IOProc down asynchronously, so a device we closed
        // moments ago can still report that it is running - and the probe below reads that as
        // "another client has it" and declines to touch the buffer.
        //
        // That is the whole of CT's "I'm attempting to set 64 and getting 512" on project load. The
        // sequence is: the device opens before the saved buffer size is known, so it comes up at
        // whatever it was - 512; the restored state then arrives and asks for 64; reconfigure()
        // closes the stream and immediately probes; the probe sees the stream it just closed; the
        // set is skipped; and 512 stands for the session. Setting 64 by hand later works because by
        // then the ghost is long gone - which is exactly why retrying "fixed" it, and why the
        // device is plainly not really shared.
        //
        // Bounded, so a device that is genuinely being driven by someone else still reaches the
        // shared path below after a fifth of a second rather than being waited on for ever.
        // 120 ms, not longer, and the reason is the lock this runs under - see the todo about
        // reconfigure()'s scope. Every millisecond spent here is a millisecond the host's main
        // thread can be blocked in getState().
        device_wait_until_idle(info.id, 120);

        gPhaseIdle = now_ms();

        bool deviceIsShared = device_is_running_somewhere(info.id);

        if (deviceIsShared && ((settings->rate > 0.0) || (settings->frames > 0))) {
            log_line("device '%s' is already running for another client - leaving its rate and"
                     " buffer size alone (asked for %.0f Hz, %u frames). Its buffer is whatever that"
                     " client set, and the ring and reported latency follow it",
                     info.name, settings->rate, settings->frames);
        }

        {
            tGbStatus * status = gb_status(statusSlot);

            if (status != nullptr) {
                atomic_store(&status->deviceShared, deviceIsShared ? 1 : 0);
            }
        }

        gPhaseProps = now_ms();

        // Rate before buffer size: changing the nominal rate can reset the buffer size on some
        // devices, so doing it the other way round silently loses the buffer setting.
        if (!deviceIsShared && (settings->rate > 0.0)) {
            // WHAT IT WAS, so close_capture_locked() can hand the device back as it found it.
            // Recorded only when we are actually about to change it, and only for the device we
            // changed - restoring one we never touched would be its own kind of interference.
            if (device_sample_rate(info.id) != settings->rate) {
                restoreDevice = info.id;
                restoreRate   = device_sample_rate(info.id);
            }
            device_set_sample_rate_and_wait(info.id, settings->rate);
        }

        if (!deviceIsShared && (settings->frames > 0)) {
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

            if (device_buffer_frames(info.id) != wanted) {
                restoreDevice = info.id;
                restoreFrames = device_buffer_frames(info.id);
            }
            // The result MATTERS now that it is confirmed rather than assumed - see the note on
            // device_set_buffer_frames(). A false here is a device that had the size asked of it,
            // took the call and never changed, which is what "something else already has it open"
            // looks like from this side.
            if (!device_set_buffer_frames(info.id, wanted) && (restoreDevice == info.id)) {
                // Nothing was changed, so there is nothing to hand back on close.
                restoreDevice = 0;
                restoreFrames = 0;
            }
        }

        double deviceRate = device_sample_rate(info.id);

        if ((deviceRate <= 0.0) || (hostRate <= 0.0)) {
            return false;
        }

        uint32_t deviceFrames = device_buffer_frames(info.id);

        // SAID OUT LOUD WHEN IT IS NOT WHAT WAS ASKED FOR. Everything downstream uses the real
        // size - the ring's floor and the device latency both - so the arithmetic was never wrong;
        // what was missing was anyone being told, and a device silently running eight times the
        // requested buffer is most of a plug-in's reported latency.
        if ((settings->frames > 0) && (deviceFrames != settings->frames)) {
            uint32_t canDo   = 0;
            uint32_t canDoTo = 0;
            bool     ranged  = device_buffer_frame_range(info.id, &canDo, &canDoTo);

            // TWO QUITE DIFFERENT CAUSES, and the range tells them apart. If the size asked for is
            // inside what the device says it supports, the driver took the call and ignored it -
            // which is what a device ALREADY OPEN by something else does, because the buffer belongs
            // to whoever opened it first. If it is outside, the driver simply cannot do it.
            bool inRange = ranged && (settings->frames >= canDo)
                           && ((canDoTo == 0) || (settings->frames <= canDoTo));

            // AND IF THE OTHER HOLDER IS ONE OF US, SAY SO BY NAME. A host loads every plug-in
            // into one process, so the status block this instance publishes into is the same array
            // every other GenBridge in the session publishes into - which makes "something else has
            // it open" answerable rather than a shrug. Two instances pointed at one device is an
            // ordinary thing to end up with, and the second one silently inherits the first one's
            // buffer.
            const char * culprit = nullptr;

            for (uint32_t slot = 0; slot < GB_STATUS_SLOTS; slot++) {
                tGbStatus * other = gb_status(slot);

                if ((other == nullptr) || (slot == (uint32_t)statusSlot)
                    || !atomic_load(&other->active)) {
                    continue;
                }

                if (strcmp(other->deviceName, info.name) == 0) {
                    culprit = other->deviceName;
                    break;
                }
            }

            log_line("asked %s for %u frames and it gave %u - %s. Its range is %u..%u. The ring and "
                     "the reported latency follow the %u, which is why they look large against the "
                     "setting on the panel",
                     info.name, settings->frames, deviceFrames,
                     (culprit != nullptr)
                     ? "ANOTHER GenBridge IN THIS HOST ALREADY HAS THIS DEVICE OPEN, and the buffer "
                       "belongs to whoever opened it first - set them both the same, or point them "
                       "at different devices"
                     : (inRange ? "the size is one it says it supports, so something outside this "
                        "host already has the device open and the buffer belongs to whoever opened "
                        "it first"
                        : "outside what its driver will do"),
                     ranged ? canDo : 0, ranged ? canDoTo : 0, deviceFrames);
        }

        nominalRatio   = deviceRate / hostRate;
        deviceLatency  = device_latency_frames(info.id, true);
        manualSetpoint = (settings->targetMs > 0.0);
        setpointFrames = manualSetpoint
                         ? ((settings->targetMs / 1000.0) * deviceRate)
                         : 0.0;
        trimGain.store(settings->trim);

        // THE DECLARATION IS THE FLOOR, AND THE OBSERVATION ONLY EVER RAISES IT HERE.
        //
        // This used to trust the observation outright, which is wrong at the FIRST open: the burst
        // measurement needs a whole undisturbed callback to be right, and a project load is the
        // least undisturbed moment there is. A busy load separates back-to-back calls by more than
        // half a block, the boundary test reads that as two cycles, and the burst comes out half
        // what it really is.
        //
        // Measured by CT: an Analog Rytm at a 64 frame device buffer came up with setpoint 560,
        // which is 1.25 * (256 + 64 + 128) - a 256 frame callback. Switching the buffer to 128 and
        // back settled it at 880, which is the same arithmetic on the true 512. Only the first
        // value was ever wrong, and it was wrong in the direction that underruns.
        //
        // maxSamplesPerBlock is a contract and is safe to size for; the retune still reclaims the
        // difference once it has watched for a couple of seconds and can be believed.
        // ONCE THE OBSERVATION HAS SETTLED, it is the better number and is used as it stands - that
        // is the whole point of the retune, and a reopen must not throw the reclaimed frames away.
        // While it is still WATCHING, it is a partial count and only ever raises the declaration.
        bool trustObserved = (retuneState.load() != eRetuneWatching) && (observedMaxFrames > 0);

        uint32_t effectiveHostFrames = trustObserved
                                       ? observedMaxFrames
                                       : ((observedMaxFrames > hostMaxFrames) ? observedMaxFrames
                                          : hostMaxFrames);
        double   minimum              = minimum_setpoint_for(effectiveHostFrames, deviceFrames);

        // THE FLOOR IS ADVICE, NOT A LIMIT — for a setting the user typed. Auto still takes it, and
        // still adds the margin; an explicit target is now honoured as given, however low.
        //
        // It is advice because the floor is deliberately conservative and cannot be otherwise: it
        // covers the WORST phase alignment between two unrelated clocks and the LARGEST block the
        // host says it may ever ask for, neither of which is what a given session actually does. A
        // number that pessimistic is worth showing and wrong to impose — clamping silently replaced
        // what the user asked for with a figure they could not see, which reads as the control not
        // working.
        //
        // Safe to allow because the failure is bounded and visible. ring_read() hands the device
        // silence on an underrun and does NOT advance the read cursor, so the loop still sees the
        // true depth and pulls to refill; nothing is corrupted and nothing runs away. The panel
        // already shows fill/setpoint, underruns and resyncs, so too tight a setting reports itself
        // in the one place the user is looking while they choose it.
        recommendedSetpoint = minimum * GB_AUTO_MARGIN;

        if (setpointFrames <= 0.0) {
            setpointFrames = recommendedSetpoint;           // auto
        } else if (setpointFrames < minimum) {
            log_line("setpoint %.0f is below the %.0f frame floor (recommended %.0f) — honouring it;"
                     " watch the underrun count", setpointFrames, minimum, recommendedSetpoint);
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

        gPhaseOpen = now_ms();

        if (!device_open(&capture, info.id, true, first, captureChannels,
                         deviceFrames * 4, capture_callback, this)) {
            log_line("device_open failed for '%s' (%u ch from %u)", info.name, captureChannels.load(),
                     first + 1);
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

        // NOT ARMED FOR A MANUAL SETPOINT. Retuning exists to claw back latency the host's declared
        // block size overstates, and it does that by REPLACING the setpoint. Against a figure the
        // user typed that is not a saving, it is the plug-in overruling them a couple of seconds
        // after they set it — the same silent overrule the clamp above used to do, arriving late.
        retuneState.store(((observedMaxFrames > 0) || manualSetpoint)
                          ? eRetuneSettled : eRetuneWatching);

        tGbStatus * status = gb_status(statusSlot);

        snprintf(status->deviceName, sizeof(status->deviceName), "%s", info.name);
        atomic_store(&status->deviceRate, (int)deviceRate);
        atomic_store(&status->deviceFrames, (int)deviceFrames);
        atomic_store(&status->setpointFrames, setpointFrames);
        atomic_store(&status->active, true);

        publish_config_snapshot();
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

        // HAND THE DEVICE BACK AS WE FOUND IT.
        //
        // Rate and buffer size are global to the device and nothing put them back, so a device this
        // plug-in had merely PASSED THROUGH was left reconfigured for everything else on the machine
        // - including, when it is the host's own device, the host. Switching away from it therefore
        // did not release it in any meaningful sense.
        //
        // AFTER device_close(), so our own stream is already gone and the device is not being
        // reconfigured underneath a running IOProc of ours. The rate is set without waiting for
        // confirmation: we are on our way out, nothing here depends on it having landed, and the
        // wait costs up to 800 ms of the caller's time - which on this path is a device change the
        // user is waiting on.
        // The snapshot describes a device that is now gone. Cleared here so nothing reports the
        // latency of a bridge that is no longer running.
        nominalRatio   = 0.0;
        setpointFrames = 0.0;
        deviceLatency  = 0;
        publish_config_snapshot();

        // NOT WHEN WE ARE ABOUT TO REOPEN THE SAME DEVICE. reconfigure() closes and reopens, and
        // handing the buffer back to what it was in between means every reconfigure drives the
        // device 64 -> 512 -> 64 for no one's benefit. On a USB interface each of those costs over
        // a second inside CoreAudio, and between the two the device really IS at 512 - which is what
        // gets read off the panel and reported as "I set 64 and I keep getting 512".
        //
        // The RECORD is kept, so the eventual real close still hands the device back as it was
        // found. Only the pointless middle of a reopen is skipped.
        if ((restoreDevice != 0) && (restoreDevice == keepSettingsFor)) {
            log_line("reopening the same device - leaving its buffer alone rather than restoring "
                     "%u frames and setting it straight back", restoreFrames);
        } else if (restoreDevice != 0) {
            if (restoreFrames > 0) {
                device_set_buffer_frames(restoreDevice, restoreFrames);
            }

            if (restoreRate > 0.0) {
                device_set_sample_rate(restoreDevice, restoreRate);
            }
            log_line("restored device settings: %u frames, %.0f Hz", restoreFrames, restoreRate);

            restoreDevice = 0;
            restoreFrames = 0;
            restoreRate   = 0.0;
        }

        // observedMaxFrames DELIBERATELY SURVIVES A CLOSE. The largest block the host has actually
        // asked for is a property of the HOST, not of the device being closed - so a device change,
        // or the reactivation that telling the host about a latency change itself provokes, must not
        // throw the observation away. Resetting it here is what made a reopen go straight back to
        // the conservative setpoint and re-learn from scratch, which vst3check catches as "a reopen
        // keeps the tuned setpoint". It is cleared in setupProcessing() instead, and only when the
        // host declares a different maximum.
        observedFrames = 0;
        retuneState.store((observedMaxFrames > 0) ? eRetuneSettled : eRetuneWatching);

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
        savedDeviceName.clear();

        // A RESTORE IS NOT A CHOICE. Everything after this point until the user actually picks
        // something is the project being reopened, and the saved UID - not the saved slot index -
        // is what says which device that was. See the device parameter in process() and reconfigure().
        savedDevicePending.store(false);
        deviceParamSeen.store(false);

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

                int slot = gb_midi_slot_for_name(wanted.c_str());

                if (slot >= 0) {
                    midiDestination.store(slot);
                }
            } else if (line.compare(0, 7, "midich=") == 0) {
                midiChannel.store(atoi(line.substr(7).c_str()));
            } else if (line.compare(0, 9, "callback=") == 0) {
                // Into observedMaxFrames directly, so the first open sizes from it exactly as it
                // would from a live observation - see the note where it is written.
                unsigned frames = (unsigned)strtoul(line.substr(9).c_str(), nullptr, 10);

                if ((frames > 0) && (frames <= (GB_MAX_BLOCK_FRAMES))) {
                    observedMaxFrames = frames;
                }
            } else if (line.compare(0, 9, "testnote=") == 0) {
                int note = atoi(line.substr(9).c_str());

                testNote.store((note < 0) ? 0 : ((note > 127) ? 127 : note));
            } else if (line.compare(0, 11, "activename=") == 0) {
                // Written purely so the panel can NAME what it is waiting for. The device is absent
                // by definition in that state, so its name cannot be looked up - a UID is all there
                // would otherwise be to show, and a CoreAudio UID is not something to hand a user.
                savedDeviceName = line.substr(11);
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

        // A device was named in the project, so it - and not a slot index recorded when the device
        // list had a different shape - decides what gets opened, until it either turns up or the
        // user chooses something else.
        savedDevicePending.store(!deviceSelector.empty());

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

        // AND WHAT CAME BACK OUT OF IT. Paired with the "saving:" line above, these two settle
        // whether a buffer size survived the round trip through the project file.
        log_line("restoring: device '%s' frames %u rate %.0f", uid.c_str(), frames, rate);

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
    // Assigned when the host connects the two ends (its own thread) and read from the audio thread
    // by publish_status() on every block.
    std::atomic<int>    statusSlot{-1};
    // Written by whichever thread last told the host, read by getLatencySamples() on any thread -
    // see its comment. Atomic because those are genuinely different threads, not for ordering.
    std::atomic<uint32> reportedLatency{0};

    // ---- THE PUBLISHED SNAPSHOT --------------------------------------------------------------
    //
    // hostRate, nominalRatio, setpointFrames and deviceLatency are plain fields, written by the
    // worker under configLock while it swaps a device. Several readers need them and CANNOT take
    // that lock: process() must never block, and the panel publisher is called from paths that
    // already hold it. Reading the raw fields from there is a data race, and not a harmless one -
    // nominalRatio is a divisor, so observing the 0 it briefly holds mid-swap yields inf or NaN.
    //
    // Making `running` atomic fixed the FLAG and not the state it gates, which is the more dangerous
    // shape: the obvious warning sign disappears while the composite read stays broken. So the
    // derived values get published as atomics, once, by the thread that changed them - and every
    // lock-free reader uses these and never the fields behind them.
    // THE HOST RATE, READABLE WITHOUT THE LOCK. snapHostRate is the config snapshot's copy and is 0
    // until a capture device opens, but events are forwarded whether one is open or not - the DAW
    // still plays the hardware. Written in setupProcessing() and read by forward_events(), which
    // runs before the trylock.
    std::atomic<double> eventRate{0.0};

    // THE PIPELINE'S DELAY, SMOOTHED - and it is what the host is told, rather than the setpoint
    // the ring is aimed at.
    //
    // The two are not the same thing and the difference is audible. The reported figure used to be
    // built from setpointFrames, on the reasonable ground that a latency moving every block would
    // have the host redo delay compensation continuously. But the ring only sits AT its setpoint on
    // average and, on a bursty host, sits below it: measured on this rig, told 2679 against an
    // actual 2221-2506, a gap of 4 to 9 ms. A host compensating by 9 ms for a 5 ms delay records
    // everything 4 ms early, which is exactly the report that led here - and no amount of measuring
    // the SYNTH can fix a figure that is wrong about US.
    //
    // A two-second average, so it follows the ring without chasing its sawtooth, and a deadband
    // before the host is told (see latency_worth_reporting) so a figure that now moves does not
    // cost a compensation pass every block.
    std::atomic<double> pipelineAvg{0.0};

    // Note events taken from the host, and note events that reached a MIDI destination. See the
    // note where they are counted.
    std::atomic<uint32_t> eventsIn{0};
    std::atomic<uint32_t> eventsOut{0};

    // AUDIO THREAD ONLY, and it is the whole state of the block timeline: the wall time at which
    // the frames handed over so far run out. See block_host_time().
    uint64_t            nextBlockHostTime{0};

    // The unmodelled clock, read at the top of the current block, and the model's answer for the
    // same block. Their difference is the frames already handed over inside this callback. AUDIO
    // THREAD ONLY.
    uint64_t            blockActualHostTime{0};
    uint64_t            blockHostTimeNow{0};

    // Whether the current call opened a new audio callback, and what the previous call was, which
    // is how that is decided. AUDIO THREAD ONLY.
    bool                blockStartsCycle{true};
    uint64_t            lastCallHostTime{0};
    uint32_t            lastCallFrames{0};

    // Frames handed over so far inside the current callback, and how many of them came BEFORE the
    // current call. The second is the one the latency arithmetic wants, and it has to be counted
    // rather than inferred from the model: the model legitimately runs ahead of the clock across
    // callbacks as well as within them, so model-minus-clock grows without bound and is not this.
    // AUDIO THREAD ONLY.
    uint32_t            burstFrames{0};
    uint32_t            burstBefore{0};

    std::atomic<double> snapHostRate{0.0};
    std::atomic<double> snapRatio{1.0};
    std::atomic<double> snapSetpoint{0.0};
    std::atomic<uint32> snapDeviceLatency{0};

    // The block-size observation, and where it has got to.
    enum tRetuneState { eRetuneWatching = 0, eRetuneRequested, eRetuneSettled, eRetuneBlocked };

    std::atomic<tRetuneState> retuneState{eRetuneSettled};
    std::atomic<bool>   revertWanted{false};

    // What the conservative floor would have picked, and whether the setpoint in force came from
    // the user rather than from it. Both are written under configLock on the open path.
    double              recommendedSetpoint{0.0};
    bool                manualSetpoint{false};
    std::atomic<int>    midiDestination{0};
    std::atomic<bool>   offlineRender{false};
    std::atomic<double> offsetMs{0.0};
    std::atomic<int>    midiChannel{0};        // 0 = whatever the note arrived on

    enum tMeasureState { eMeasureIdle = 0, eMeasureSettle, eMeasureFloor, eMeasureListening };

    tMeasureState       measureState{eMeasureIdle};
    uint32_t            measureFrames{0};

    // Whether the floor phase has reached its final third, where the hold restarts. AUDIO THREAD.
    bool                measureFloorLate{false};
    uint32_t            measureOnsetFrames{0};

    // The ring occupancy at the block the onset was found in, in DEVICE frames - what the sound
    // actually came through, as against the setpoint it is supposed to sit at. See run_measurement().
    double              measureOnsetFill{0.0};

    // The host-clock instant the test note was SCHEDULED for, and therefore the instant it left.
    // AUDIO THREAD ONLY.
    uint64_t            measureNoteTime{0};

    // Our own share of the round trip, as it stood at the onset block. AUDIO THREAD ONLY.
    double              measureOnsetOurs{0.0};

    // The round trips of one Measure press, and where the current one is up to. AUDIO THREAD ONLY.
    int                 measureTrips[GB_MEASURE_TRIPS]{0};
    int                 measureTrip{0};
    uint32_t            measureTripUnderruns{0};
    int                 measureTripResyncs{0};

    // How many trips survived to be averaged, and what the extremes were. Published because an
    // average on its own hides the thing worth knowing: five readings within a millisecond mean the
    // number can be trusted, and the same average from readings 9 ms apart means it cannot. The
    // panel shows the range beside the figure for exactly that reason.
    std::atomic<int>    measureTripsUsed{0};
    std::atomic<int>    measureTripLow{0};
    std::atomic<int>    measureTripHigh{0};
    std::atomic<int>    measureTripSpread{0};

    // Which note the measurement plays. A drum machine may have nothing on middle C at all - an
    // Analog Rytm wants the lowest note there is - so this is settable and saved.
    std::atomic<int>    testNote{GB_MEASURE_NOTE};
    int                 measureConfirm{0};
    float               measurePeak{0.0f};
    float               measureFloor{0.0f};
    bool                measureArmed{false};
    std::atomic<int>    measureLatency{0};
    std::atomic<bool>   measureStore{false};

    // Raised by the audio thread when Measure is pressed; the worker sends the All Notes Off and
    // writes the log line, neither of which belongs in a process() call.
    std::atomic<bool>   measurePanic{false};
    std::atomic<bool>   latencyDirty{false};

    // Raised by the audio thread when the measured pipeline has drifted past the deadband; the
    // worker decides what to do about it.
    std::atomic<bool>   latencyStale{false};

    // Phase marks inside one reconfigure, for the lock-hold breakdown. Worker thread only.
    double              gPhaseIdle{0.0};
    double              gPhaseProps{0.0};
    double              gPhaseOpen{0.0};

    // The offset moved and the host has not been told yet, and when it last moved - see
    // GB_OFFSET_SETTLE_MS.
    std::atomic<bool>   offsetDirty{false};
    std::atomic<double> lastOffsetChangeMs{0.0};
    std::atomic<int>    measureOnset{0};
    std::atomic<int>    measureOurs{0};

    // The ring occupancy the onset actually came through, published so the log can show it beside
    // the setpoint it is meant to be sitting at. A run that disagrees with the others usually
    // disagrees here first.
    std::atomic<double> measureFillSeen{0.0};
    std::atomic<float>  measureTriggerPeak{0.0f};
    std::atomic<float>  measureFloorSeen{0.0f};
    // Written by start_measurement() on the AUDIO thread (it runs from the parameter pass, before
    // process() takes its trylock) and read by store_measurement() on the worker, which subtracts
    // them to decide whether a measurement was clean. Two threads, so not plain ints.
    std::atomic<uint32_t> measureUnderrunsAtStart{0};
    std::atomic<int>      measureResyncsAtStart{0};

    tMeasured           measured[GB_MAX_MEASURED];
    uint32_t            measuredCount{0};

    // WHICH PAIR THE LIVE offsetMs IS FOR. Without this the correction could not survive its own
    // application: changing the reported latency makes the host reactivate the plug-in, which
    // reopens the device, and a reopen that re-seeded the offset from the table would undo every
    // nudge the moment it took effect. Re-seeding is now gated on the pair actually having changed,
    // so reopening the same device leaves the value exactly where the user put it.
    char                offsetUid[DEVICE_UID_LEN]{0};
    char                offsetDest[GB_MIDI_NAME_LEN]{0};
    // In force for the current device/destination pair. Written by the worker (store_measurement and
    // the open), read by publish_latency_breakdown() - which is called both with configLock held and
    // without it, so it can take neither and this has to carry its own guarantee.
    std::atomic<uint32_t> hardwareSamples{0};
    uint32_t            observedMaxFrames{0};
    uint64_t            observedFrames{0};
    uint32_t            openDeviceFrames{0};

    // The host-clock instant of the most recent capture callback. Written by the device thread,
    // read by the audio thread; see latency_frames_measured() for why occupancy alone is not enough.
    std::atomic<uint64_t> lastWriteHostTime{0};
    std::atomic<bool>  needResync{true};
    std::atomic<float> trimGain{1.0f};
    std::atomic<bool>  deviceDirty{false};

    // When the last device/rate/frames request arrived, so a burst of them can settle into one
    // device change - see GB_DEVICE_SETTLE_MS.
    std::atomic<double> lastDeviceRequestMs{0.0};

    // What the currently open device's rate and buffer size were before this plug-in changed them,
    // and which device that was. Zero means "changed nothing, restore nothing". Worker thread only,
    // written under configLock alongside the open and close they belong to.
    AudioObjectID       restoreDevice{0};
    uint32_t            restoreFrames{0};
    double              restoreRate{0.0};
    std::atomic<int>   resyncs{0};       // how often the ring had to be snapped back; 0 is healthy
    std::atomic<bool>  workerQuit{false};
    std::atomic<int>   wantedDevice{-1};
    std::atomic<bool>  savedDevicePending{false};    // a project named a device; honour it, not a slot
    std::atomic<bool>  deviceParamSeen{false};       // the host's restored value has been and gone
    std::string        savedDeviceName;              // for the panel, since an absent device has no name
    std::atomic<double> wantedRate{0.0};
    std::atomic<int>   wantedFrames{0};
    std::atomic<int>   wantedChannels{0};
    std::atomic<int>   wantedFirstChannel{-1};
    // Written by the worker under configLock, read by the CoreAudio IO thread (capture_callback,
    // which takes no lock and must not) and by the audio thread's pre-trylock parameter pass. Three
    // threads, so it cannot be a plain int whatever the practical consequence of a torn read is.
    std::atomic<uint32_t> captureChannels{GB_CHANNELS};

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
    std::atomic<bool> running{false};
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
        } else if (strcmp(id, "gbMode") == 0) {
            // The device could not give a stereo pair and the open took what it had - see the clamp
            // in open_capture_locked(). Mirrors gbFirstChannel below for the other half of the same
            // request.
            int64 stereo = -1;

            if ((message->getAttributes()->getInt("value", stereo) == kResultOk) && (stereo >= 0)) {
                mode = (stereo > 0) ? 1.0 : 0.0;

                if (componentHandler != nullptr) {
                    componentHandler->beginEdit(kParamMode);
                    componentHandler->performEdit(kParamMode, mode);
                    componentHandler->endEdit(kParamMode);
                }

                if (editorView != nullptr) {
                    gb_editor_refresh_values(editorView);
                }
            }
        } else if (strcmp(id, "gbFirstChannel") == 0) {
            // The device could not give the channel that was asked for and the open used another -
            // see the clamp in open_capture_locked(). Without this the panel goes on naming a
            // channel nothing is being captured from, which is the same class of disagreement
            // gbDeviceSlot below exists to settle.
            int64 channel = -1;

            if ((message->getAttributes()->getInt("value", channel) == kResultOk) && (channel >= 0)) {
                firstChannel = (double)channel / (double)(GB_MAX_FIRST_CHANNEL - 1);

                if (componentHandler != nullptr) {
                    componentHandler->beginEdit(kParamFirstChannel);
                    componentHandler->performEdit(kParamFirstChannel, firstChannel);
                    componentHandler->endEdit(kParamFirstChannel);
                }

                if (editorView != nullptr) {
                    gb_editor_refresh_values(editorView);
                }
            }
        } else if (strcmp(id, "gbDeviceSlot") == 0) {
            // The processor opened the device the PROJECT named, which is not the slot the restored
            // parameter pointed at - the list had a different shape when that index was saved. Put
            // the parameter right so the panel, the host and the open device finally agree.
            int64 slot = -1;

            if ((message->getAttributes()->getInt("value", slot) == kResultOk) && (slot >= 0)) {
                device = gb_device_normalized((int)slot);

                if (componentHandler != nullptr) {
                    componentHandler->beginEdit(kParamDevice);
                    componentHandler->performEdit(kParamDevice, device);
                    componentHandler->endEdit(kParamDevice);
                }

                if (editorView != nullptr) {
                    gb_editor_refresh_values(editorView);
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
        note        = (double)active.testNote / 127.0;

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

    // THE CONTROLLER'S OWN STATE, which is a different thing from the component's and is exactly
    // where the GUI's own settings belong: the processor has no business knowing how big a window is,
    // and a size on a dev= line would travel with a device it has nothing to do with.
    //
    // Line based and versioned like the component's blob above, and for the same reason - an older
    // build skips a key it does not know rather than rejecting the lot. Both of these returned
    // kResultOk without reading or writing a byte, which is why the editor came back at its default
    // size in every session however the last one was left.
    tresult PLUGIN_API setState(IBStream * state) SMTG_OVERRIDE {
        if (state == nullptr) {
            return kResultFalse;
        }

        std::string blob;
        char        chunk[256];
        int32       read = 0;

        while ((state->read(chunk, (int32)sizeof(chunk), &read) == kResultOk) && (read > 0)) {
            blob.append(chunk, (size_t)read);

            if (read < (int32)sizeof(chunk)) {
                break;
            }
        }
        size_t at = blob.find("editor=");

        if (at != std::string::npos) {
            double w = 0.0;
            double h = 0.0;

            if (sscanf(blob.c_str() + at, "editor=%lf,%lf", &w, &h) == 2) {
                // SANITY-CHECKED, not trusted. This blob comes out of a saved project and a
                // nonsensical size would leave an editor the user cannot see or cannot fit on a
                // screen, with no way back to the default short of editing the project file. The
                // bounds are checkSizeConstraint()'s own.
                if ((w >= (GB_CANVAS_W * 0.75)) && (w <= (GB_CANVAS_W * 2.0)) && (h > 0.0)) {
                    editorWidth  = w;
                    editorHeight = h;
                }
            }
        }

        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream * state) SMTG_OVERRIDE {
        if (state == nullptr) {
            return kResultFalse;
        }

        char  blob[128];
        int   len   = snprintf(blob, sizeof(blob), "GENBRIDGEGUI1\neditor=%.0f,%.0f\n",
                               editorWidth, editorHeight);
        int32 wrote = 0;

        return (state->write(blob, (int32)len, &wrote) == kResultOk) ? kResultOk : kResultFalse;
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

        if ((index == kParamTestNote) && instrument) {
            info.id                     = kParamTestNote;
            info.stepCount              = 127;
            info.defaultNormalizedValue = (double)GB_MEASURE_NOTE / 127.0;
            info.flags                  = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;

            to_utf16("Test Note", info.title, 128);
            to_utf16("Note", info.shortTitle, 128);

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

        if (id == kParamTestNote) {
            // C-2 IS NOTE 0, the convention where middle C is C3 - which is what the hardware this
            // is aimed at prints on its own screen. An Analog Rytm's lowest pad is C-2 and a user
            // reading "C-1" there would be a semitone-free octave out.
            static const char * const kName[12] = { "C",  "C#", "D",  "D#", "E",  "F",
                                                    "F#", "G",  "G#", "A",  "A#", "B" };
            int note = (int)((valueNormalized * 127.0) + 0.5);

            note = (note < 0) ? 0 : ((note > 127) ? 127 : note);

            snprintf(buffer, sizeof(buffer), "%s%d (%d)", kName[note % 12], (note / 12) - 2, note);
            to_utf16(buffer, string, 128);
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
            case kParamTestNote: return note;
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
            case kParamTestNote: note     = value; return kResultOk;
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
                                           editorWidth, editorHeight,
                                           editor_gone, editor_resized, this);

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

    static void editor_resized(void * user, double width, double height) {
        GenBridgeController * self = (GenBridgeController *)user;

        self->editorWidth  = width;
        self->editorHeight = height;
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
    double              editorWidth{GB_CANVAS_W};
    double              editorHeight{GB_CANVAS_H};
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
    ParamValue          note{(double)GB_MEASURE_NOTE / 127.0};   // the note Measure plays
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
