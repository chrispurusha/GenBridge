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

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/base/ipluginbase.h"

#include "gbDraw.h"
#include "gbEditor.h"

#include "device.h"
#include "drift.h"
#include "gbStatus.h"
#include "resampler.h"
#include "ring.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

#define GB_VENDOR         "Chris Purusha"
#define GB_PLUGIN_NAME    "GenBridge"
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

// TEMPORARY, until the editor exists. With no way to pick a device from inside a host, a fresh
// instance would sit silent and look broken, so it falls back to this. The device parameter and any
// saved state both take precedence, so choosing anything else immediately overrides it - and the
// whole block goes when the SynthLib chooser lands.
// A 32 input interface is common - the TD-50X here is one - so the first-channel list has to reach
// that far even though most devices are stereo. Slots past the device's real channel count simply
// fail to open, which the panel shows.
#define GB_MAX_FIRST_CHANNEL  (32)

#define GB_FALLBACK_DEVICE    "KRONOS"
#define GB_DEFAULT_FRAMES     (128)
#define GB_DEFAULT_RATE       (48000.0)

// Stable identity. A host remembers a plug-in by this, so it must never change once a project has
// been saved against it.
static const FUID kGenBridgeProcessorUID(0x4A1C8E52, 0x9D3B4F07, 0xA6E21B84, 0x53F0C97D);
static const FUID kGenBridgeControllerUID(0x8B70D6A1, 0x2F594C38, 0xE1A76025, 0x9C4D3B8F);

enum {
    kParamDevice = 0,
    kParamTrim,
    kParamRate,
    kParamFrames,
    kParamMode,          // mono or stereo
    kParamFirstChannel,  // which device channel the capture starts at
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
static void log_line(const char * format, ...) {
    if (access("/tmp/genbridge-log", F_OK) != 0) {
        return;
    }

    FILE * file = fopen("/tmp/genbridge.log", "a");

    if (file == nullptr) {
        return;
    }

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
    unsigned    frames{GB_DEFAULT_FRAMES};
    double      rate{GB_DEFAULT_RATE};
    unsigned    firstChannel{0};
    unsigned    channels{GB_CHANNELS};
    float       trim{1.0f};
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

    size_t pos = 0;

    while (pos < blob.size()) {
        size_t      end  = blob.find('\n', pos);
        std::string line = blob.substr(pos, (end == std::string::npos) ? std::string::npos : end - pos);

        pos = (end == std::string::npos) ? blob.size() : end + 1;

        if (line.compare(0, 7, "active=") == 0) {
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

    return out;
}

// ------------------------------------------------------------------------------------------------
// The processor.
// ------------------------------------------------------------------------------------------------

class GenBridgePlugin : public IComponent, public IAudioProcessor, public IConnectionPoint {
public:
    GenBridgePlugin(void) : refCount(1) {
        statusSlot = gb_status_claim();
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
        memcpy(classId, kGenBridgeControllerUID.toTUID(), sizeof(TUID));
        return kResultOk;
    }

    tresult PLUGIN_API setIoMode(IoMode mode) SMTG_OVERRIDE {
        (void)mode;
        return kResultOk;
    }

    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) SMTG_OVERRIDE {
        if (type == kAudio) {
            return 1;   // one in (ignored, but an effect must have one), one out
        }
        (void)dir;
        return 0;
    }

    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo & info) SMTG_OVERRIDE {
        if ((type != kAudio) || (index != 0)) {
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

        if ((numIns == 1) && (numOuts == 1) && (outputs[0] == SpeakerArr::kStereo)) {
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

    uint32 report_latency(void) const {
        double inputFrames = setpointFrames + resampler_latency_frames() + (double)deviceLatency;

        // Reported in the HOST's frames, and the ring is measured in the device's.
        return (uint32)(inputFrames / nominalRatio);
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup & setup) SMTG_OVERRIDE {
        hostRate      = setup.sampleRate;
        hostMaxFrames = (uint32)setup.maxSamplesPerBlock;
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

        publish_status(out, frames, fill);

        pthread_mutex_unlock(&configLock);

        return kResultOk;
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

            // Only the last point matters for either parameter: the trim is not smoothed yet, and
            // a device change is not a thing to apply twice in one block.
            if ((points <= 0) || (q->getPoint(points - 1, offset, value) != kResultOk)) {
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
            device_set_buffer_frames(info.id, settings->frames);
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

        double minimum = minimum_setpoint(deviceFrames);

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

        needResync.store(true);
        running = true;

        // What is now genuinely in force. The parameter handlers compare against these, so a
        // re-sent value that changes nothing costs nothing.
        appliedRate         = deviceRate;
        appliedFrames       = deviceFrames;
        appliedChannels     = captureChannels;
        appliedFirstChannel = settings->firstChannel;

        tGbStatus * status = gb_status(statusSlot);

        snprintf(status->deviceName, sizeof(status->deviceName), "%s", info.name);
        atomic_store(&status->deviceRate, (int)deviceRate);
        atomic_store(&status->deviceFrames, (int)deviceFrames);
        atomic_store(&status->setpointFrames, setpointFrames);
        atomic_store(&status->latencySamples, (int)getLatencySamples());
        atomic_store(&status->active, true);

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
        return ((double)hostMaxFrames * nominalRatio)
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

        if (running) {
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
        deviceSelector.clear();

        size_t pos = 0;

        while (pos < blob.size()) {
            size_t      end  = blob.find('\n', pos);
            std::string line = blob.substr(pos, (end == std::string::npos) ? std::string::npos : end - pos);

            pos = (end == std::string::npos) ? blob.size() : end + 1;

            if (line.compare(0, 7, "active=") == 0) {
                deviceSelector = line.substr(7);
            } else if (line.compare(0, 4, "dev=") == 0) {
                parse_device_line(line.substr(4), version);
            }
            // Anything else is from a newer build; skipping it is the point of the format.
        }

        return true;
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

class GenBridgeController : public IEditController, public IConnectionPoint {
public:
    GenBridgeController(void) : refCount(1) {}
    virtual ~GenBridgeController(void) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void ** obj) SMTG_OVERRIDE {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IEditController)
        QUERY_INTERFACE(iid, obj, IPluginBase::iid, IEditController)
        QUERY_INTERFACE(iid, obj, IEditController::iid, IEditController)
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

    int32 PLUGIN_API getParameterCount(void) SMTG_OVERRIDE { return kParamCount; }

    // The rate and buffer lists are the SAME arrays the editor steps through and the processor
    // applies, so an index cannot mean one thing here and another there.

    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo & info) SMTG_OVERRIDE {
        memset(&info, 0, sizeof(info));
        info.unitId = 0;           // kRootUnitId, without pulling in ivstunits.h

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
            default:           return 0.0;
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
            default:           return kInvalidArgument;
        }
    }

    tresult PLUGIN_API setComponentHandler(IComponentHandler * handler) SMTG_OVERRIDE {
        componentHandler = handler;
        return kResultOk;
    }

    IPlugView * PLUGIN_API createView(FIDString name) SMTG_OVERRIDE {
        if ((name == nullptr) || (strcmp(name, ViewType::kEditor) != 0)) {
            return nullptr;
        }

        editorView = gb_create_editor_view(this, componentHandler, statusSlot);

        return editorView;
    }

private:
    static void to_utf16(const char * src, char16 * dst, int max) {
        int i = 0;

        for (; (src[i] != '\0') && (i < (max - 1)); i++) {
            dst[i] = (char16)src[i];
        }
        dst[i] = 0;
    }

    std::atomic<int32>  refCount;
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

    int32 PLUGIN_API countClasses(void) SMTG_OVERRIDE { return 2; }

    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo * info) SMTG_OVERRIDE {
        if ((info == nullptr) || (index < 0) || (index > 1)) {
            return kInvalidArgument;
        }

        memset(info, 0, sizeof(PClassInfo));
        info->cardinality = PClassInfo::kManyInstances;

        if (index == 0) {
            memcpy(info->cid, kGenBridgeProcessorUID.toTUID(), sizeof(TUID));
            strncpy(info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1);
            strncpy(info->name, GB_PLUGIN_NAME, PClassInfo::kNameSize - 1);
        } else {
            memcpy(info->cid, kGenBridgeControllerUID.toTUID(), sizeof(TUID));
            strncpy(info->category, kVstComponentControllerClass, PClassInfo::kCategorySize - 1);
            strncpy(info->name, GB_PLUGIN_NAME " Controller", PClassInfo::kNameSize - 1);
        }

        return kResultOk;
    }

    // The subcategory lives only on PClassInfo2, which is why IPluginFactory2 is implemented at
    // all. "Fx|NoOfflineProcess|Tools" is what Inject declares, and it is what stops a host trying
    // to bounce a live capture faster than realtime.
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2 * info) SMTG_OVERRIDE {
        if ((info == nullptr) || (index < 0) || (index > 1)) {
            return kInvalidArgument;
        }

        memset(info, 0, sizeof(PClassInfo2));
        info->cardinality = PClassInfo::kManyInstances;
        strncpy(info->vendor, GB_VENDOR, PClassInfo2::kVendorSize - 1);
        strncpy(info->version, "0.1.0", PClassInfo2::kVersionSize - 1);
        strncpy(info->sdkVersion, kVstVersionString, PClassInfo2::kVersionSize - 1);

        if (index == 0) {
            memcpy(info->cid, kGenBridgeProcessorUID.toTUID(), sizeof(TUID));
            strncpy(info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1);
            strncpy(info->name, GB_PLUGIN_NAME, PClassInfo::kNameSize - 1);
            strncpy(info->subCategories, "Fx|NoOfflineProcess|Tools", PClassInfo2::kSubCategoriesSize - 1);
        } else {
            memcpy(info->cid, kGenBridgeControllerUID.toTUID(), sizeof(TUID));
            strncpy(info->category, kVstComponentControllerClass, PClassInfo::kCategorySize - 1);
            strncpy(info->name, GB_PLUGIN_NAME " Controller", PClassInfo::kNameSize - 1);
        }

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
            instance = (IComponent *)new GenBridgePlugin();
        } else if (memcmp(cid, kGenBridgeControllerUID.toTUID(), sizeof(TUID)) == 0) {
            instance = (IEditController *)new GenBridgeController();
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
