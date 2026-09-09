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

// THE VST3 WRAPPER, AND NOTHING ELSE.
//
// What this file contains is COM: vtables, reference counting, class registration, and the
// conversions between VST3's own shapes and the plug-in's. What GenBridge actually DOES is in
// gbBridge.c, gbMeasure.c, gbState.c, gbParams.c and poc/ - plain C, which the sibling projects
// also use and which a C compiler checks the whole of.
//
// The division is a rule, not a habit: if a piece of code would still make sense in a command line
// tool, it does not belong here. process() turns a ProcessData into four calls on the bridge;
// setState turns an IBStream into a block of bytes; getParameterInfo turns what gbParams.c says
// into a ParameterInfo. Each of those is the whole of the method.
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
#include <cstring>
#include <string>   // reading a host's stream, which arrives in chunks of its choosing
#include <cstdio>
#include <cstdlib>
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

#include "gbBridge.h"
#include "gbDraw.h"
#include "gbEditor.h"
#include "gbLog.h"
#include "gbMidi.h"
#include "gbParams.h"

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

// THE CONTROLLER PASS-THROUGHS ARE DESCRIBED IN gbParams.h, and these three lines are what keeps
// that description honest. The numbers there are Vst::kAfterTouch, Vst::kPitchBend and
// Vst::kCountCtrlNumber written out, because ivstmidicontrollers.h is a C++ header and the C side
// cannot include it - so the build asserts they still agree rather than the two drifting apart in
// silence. See the note on GB_CC_PER_CHANNEL for why a parameter exists per controller per channel.
static_assert(GB_CC_AFTERTOUCH == kAfterTouch, "the SDK renumbered aftertouch");
static_assert(GB_CC_PITCHBEND == kPitchBend, "the SDK renumbered pitch bend");
static_assert(GB_CC_PER_CHANNEL == kCountCtrlNumber, "the SDK changed how many controllers there are");

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

// A NAME INTO THE UTF-16 VST3 ASKS FOR. The one conversion this file does over and over, and the
// only reason any name reaches it as a char * at all: everything that decides what a name IS lives
// in C.
static void to_utf16(const char * src, char16 * dst, int max) {
    int i = 0;

    for (; (src[i] != '\0') && (i < (max - 1)); i++) {
        dst[i] = (char16)src[i];
    }
    dst[i] = 0;
}

// ------------------------------------------------------------------------------------------------
// The processor.
//
// A SHIM, and deliberately a thin one: it owns a tGbBridge and turns VST3's calls into the bridge's.
// Anything here that is longer than a few lines is a conversion - walking a parameter queue, walking
// an event list, reading a stream - and every one of those is work the SDK's own types force.
// ------------------------------------------------------------------------------------------------

class GenBridgePlugin : public IComponent, public IAudioProcessor, public IConnectionPoint {
public:
    explicit GenBridgePlugin(bool instrumentIn) : refCount(1), instrument(instrumentIn) {
        bridge = gb_bridge_create(instrumentIn);
    }

    virtual ~GenBridgePlugin(void) {
        gb_bridge_destroy(bridge);
        bridge = nullptr;
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

        gb_bridge_watch_devices(bridge);

        return kResultOk;
    }

    tresult PLUGIN_API terminate(void) SMTG_OVERRIDE {
        gb_bridge_unwatch_devices(bridge);

        if (host != nullptr) {
            host->release();
            host = nullptr;
        }

        return kResultOk;
    }

    // ---- IConnectionPoint ----
    //
    // The host connects processor and controller to each other and this is the only channel between
    // them. All that travels over it is a handful of small messages: the status slot number sent
    // once, and thereafter only values the bridge worked out for itself and the host has to be told
    // about. The panel reads the meters and drift figures straight out of the status slot rather
    // than a message per frame.

    tresult PLUGIN_API connect(IConnectionPoint * other) SMTG_OVERRIDE {
        peer = other;

        tGbHostOps ops = { this, post_message };

        gb_bridge_connect(bridge, &ops);

        return kResultOk;
    }

    tresult PLUGIN_API disconnect(IConnectionPoint * other) SMTG_OVERRIDE {
        (void)other;
        gb_bridge_disconnect(bridge);
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
            //
            // THE INSTRUMENT NOW DECLARES ONE TOO, AND IT IS A SIDE-CHAIN (2026-09-09, a probe).
            //
            // The mode being aimed at is Live's External Instrument with the hardware latency
            // MEASURED rather than typed: the host's own interface input arrives here, the plug-in
            // passes it through, plays the synth over MIDI and times the round trip. None of the
            // ring, resampler or drift loop is needed for it - the host's input and output are the
            // same clock, which is the one thing this plug-in exists to reconcile when they are not.
            //
            // WHETHER LIVE WILL ROUTE ANYTHING INTO IT IS THE OPEN QUESTION, and it is not one to
            // settle by reasoning: this project's findings record that Live's routing model has been
            // wrong every time it was argued about rather than observed. So the bus is declared as
            // kAux and kDefaultInactive - the conventional side-chain shape, which a host that has no
            // use for it leaves alone - and process() logs what actually turns up. Nothing else is
            // changed until that log says something.
            if ((dir == kInput) && instrument) {
                return 1;
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

        info.mediaType    = kAudio;
        info.direction    = dir;
        info.channelCount = GB_CHANNELS;

        // AUX AND INACTIVE for the instrument's input, kMain and active for everything else. A
        // side-chain a host has not been asked to fill costs nothing; a MAIN input on an instrument
        // is the shape that makes a host go looking for a source and complain when there is none.
        if ((dir == kInput) && instrument) {
            info.busType = kAux;
            info.flags   = 0;                       // NOT kDefaultActive - the host opts in

            name_to_utf16("Host Input", info.name, 128);

            return kResultOk;
        }
        info.busType = kMain;
        info.flags   = BusInfo::kDefaultActive;

        name_to_utf16((dir == kInput) ? "Unused In" : "Device Out", info.name, 128);

        return kResultOk;
    }

    tresult PLUGIN_API getRoutingInfo(RoutingInfo & inInfo, RoutingInfo & outInfo) SMTG_OVERRIDE {
        (void)inInfo; (void)outInfo;
        return kNotImplemented;
    }

    // WORTH A LOG LINE WHILE THE SIDE-CHAIN IS A QUESTION. This is where a host says it intends to
    // use a bus, so it is the first evidence of whether Live has offered the input to a user at all -
    // and it arrives before any audio would.
    tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index, TBool state) SMTG_OVERRIDE {
        if (instrument && (type == kAudio) && (dir == kInput)) {
            gb_log_line("HOST INPUT: activateBus(aux input %d) -> %s", (int)index,
                        state ? "ACTIVE" : "inactive");
        }

        (void)index;

        return kResultOk;
    }

    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE {
        gb_bridge_set_active(bridge, state != 0);
        return kResultOk;
    }

    // THE BYTES IN AND OUT, and that is the whole of this file's part in the saved state. What the
    // bytes MEAN - the versioned line format, the per-device table, the measured pairs - is
    // gbState.c's, where it can be read without an SDK header in sight.
    tresult PLUGIN_API setState(IBStream * state) SMTG_OVERRIDE {
        if (state == nullptr) {
            return kResultFalse;
        }

        // READ OUTSIDE ANY LOCK OF OURS, deliberately: state->read() calls back into the host.
        // gb_bridge_set_state() takes the bridge's lock around the parse alone.
        std::string blob;
        char        chunk[1024];
        int32       read = 0;

        while ((state->read(chunk, (int32)sizeof(chunk), &read) == kResultOk) && (read > 0)) {
            blob.append(chunk, (size_t)read);

            if (read < (int32)sizeof(chunk)) {
                break;
            }
        }

        return gb_bridge_set_state(bridge, blob.data(), blob.size()) ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API getState(IBStream * state) SMTG_OVERRIDE {
        if (state == nullptr) {
            return kResultFalse;
        }
        size_t       length = 0;
        const char * blob   = gb_bridge_state(bridge, &length);
        int32        written = 0;

        return state->write((void *)blob, (int32)length, &written);
    }

    // ---- IAudioProcessor ----

    // BOTH VARIANTS NOW HAVE ONE AUDIO INPUT - the effect's main one, which it ignores, and the
    // instrument's side-chain. A host is also entitled to propose ZERO inputs for a side-chain it
    // does not intend to fill, and refusing that would be refusing a perfectly ordinary layout.
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement * inputs, int32 numIns,
                                          SpeakerArrangement * outputs, int32 numOuts) SMTG_OVERRIDE {
        (void)inputs;

        if ((numOuts != 1) || (outputs[0] != SpeakerArr::kStereo)) {
            return kResultFalse;
        }

        if (instrument) {
            return ((numIns == 0) || (numIns == 1)) ? kResultOk : kResultFalse;
        }

        return (numIns == 1) ? kResultOk : kResultFalse;
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
    uint32 PLUGIN_API getLatencySamples(void) SMTG_OVERRIDE {
        return gb_bridge_latency(bridge);
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup & setup) SMTG_OVERRIDE {
        gb_log_line("setupProcessing: rate %.0f, maxBlock %d, mode %d (%s)", setup.sampleRate,
                    (int)setup.maxSamplesPerBlock, (int)setup.processMode,
                    (setup.processMode == kRealtime) ? "realtime"
                    : ((setup.processMode == kPrefetch) ? "prefetch" : "OFFLINE"));

        gb_bridge_setup_processing(bridge, setup.sampleRate, setup.maxSamplesPerBlock,
                                   setup.processMode == kOffline);

        return kResultOk;
    }

    tresult PLUGIN_API setProcessing(TBool state) SMTG_OVERRIDE {
        if (state) {
            gb_bridge_processing_started(bridge);
        }

        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData & data) SMTG_OVERRIDE {
        if ((data.numOutputs < 1) || (data.outputs[0].numChannels < GB_CHANNELS)) {
            return kResultOk;
        }

        float ** out    = data.outputs[0].channelBuffers32;
        int32    frames = data.numSamples;

        report_host_input(data);

        if (frames <= 0) {
            return kResultOk;
        }

        // ONCE, AND BEFORE ANYTHING THAT SENDS. Every MIDI byte this call produces is stamped from
        // the instant this returns, so the whole stream stays in order however the host chops its
        // blocks up - see gb_bridge_block_begin().
        uint64_t blockHostTime = gb_bridge_block_begin(bridge, frames);

        take_parameter_changes(data, blockHostTime);

        if (instrument) {
            take_events(data, blockHostTime);
        }

        // THE HOST'S OWN INPUT, HANDED STRAIGHT ON. Which of the two the bridge uses is its
        // decision (Capture Source); this only makes sure both are available to it. numInputs is 0
        // whenever the host has routed nothing, and a null here is what the bridge treats as
        // silence rather than as an error.
        const float * const * in         = nullptr;
        int32                 inChannels = 0;

        if ((data.numInputs > 0) && (data.inputs != nullptr)
            && (data.inputs[0].channelBuffers32 != nullptr)) {
            in         = data.inputs[0].channelBuffers32;
            inChannels = data.inputs[0].numChannels;
        }

        gb_bridge_render(bridge, out, in, inChannels, frames, blockHostTime);

        return kResultOk;
    }

private:
    // ── The side-chain probe (2026-09-09) ───────────────────────────────────────────────────────
    //
    // Does anything actually arrive on the instrument's aux input under a real host? The whole
    // External-Instrument mode rests on the answer, and nothing in the code can give it.
    //
    // ONCE, AND ON CHANGE. process() runs about a hundred times a second; a line per block would be
    // a file nobody can read and would perturb the very timing this plug-in measures. So: the shape
    // of what was handed over is logged the first time and thereafter only when it changes, and
    // "silent or not" is one pass over the first channel with an early exit.
    void report_host_input(ProcessData & data) {
        if (!instrument) {
            return;
        }
        int32 channels = ((data.numInputs > 0) && (data.inputs != nullptr))
                         ? data.inputs[0].numChannels : -1;
        bool  buffers  = ((channels > 0) && (data.inputs[0].channelBuffers32 != nullptr)
                          && (data.inputs[0].channelBuffers32[0] != nullptr));
        bool  audible  = false;

        if (buffers) {
            const float * in = data.inputs[0].channelBuffers32[0];

            for (int32 i = 0; i < data.numSamples; i++) {
                if ((in[i] > 1.0e-6f) || (in[i] < -1.0e-6f)) {
                    audible = true;
                    break;
                }
            }
        }

        // THE SILENCE FLAG IS PART OF THE ANSWER, not decoration: a host that routes nothing may
        // still hand over buffers, and VST3 lets it say so in silenceFlags rather than by zeroing
        // them. "Buffers but always silent" and "no buffers at all" are different failures.
        bool declaredSilent = (buffers && (data.inputs[0].silenceFlags != 0));

        if ((data.numInputs == lastInputBuses) && (channels == lastInputChannels)
            && (audible == lastInputAudible) && (declaredSilent == lastInputSilentFlag)) {
            return;
        }
        lastInputBuses     = data.numInputs;
        lastInputChannels  = channels;
        lastInputAudible   = audible;
        lastInputSilentFlag = declaredSilent;

        gb_log_line("HOST INPUT: numInputs %d, channels %d, buffers %s, silenceFlags %s, signal %s",
                    (int)data.numInputs, (int)channels, buffers ? "yes" : "NO",
                    declaredSilent ? "set (host says silent)" : "clear",
                    audible ? "PRESENT" : "none");
    }

    int32 lastInputBuses{-1};
    int32 lastInputChannels{-2};
    bool  lastInputAudible{false};
    bool  lastInputSilentFlag{false};

    // WALKING THE HOST'S QUEUES, which is the one thing about a parameter change that is VST3's
    // business rather than the plug-in's. What each value MEANS is gb_bridge_parameter()'s.
    void take_parameter_changes(ProcessData & data, uint64_t blockHostTime) {
        if (data.inputParameterChanges == nullptr) {
            return;
        }

        int32 count = data.inputParameterChanges->getParameterCount();

        for (int32 i = 0; i < count; i++) {
            IParamValueQueue * queue = data.inputParameterChanges->getParameterData(i);

            if (queue == nullptr) {
                continue;
            }

            int32      points = queue->getPointCount();
            int32      offset = 0;
            ParamValue value  = 0.0;

            // The last point is what a continuous parameter settles at, and that is all any of
            // these need - except the measure trigger.
            if ((points <= 0) || (queue->getPoint(points - 1, offset, value) != kResultOk)) {
                continue;
            }
            ParamID id = queue->getParameterId();

            // A TRIGGER HAS TO BE FOUND AMONG ALL THE POINTS, because the editor raises it and drops
            // it again immediately - a momentary control has to return to rest or its next press
            // produces no edge at all. A host is free to deliver both of those in one block, and
            // reading only the last point then sees nothing but the release. The button did nothing,
            // every time.
            if (id == kParamMeasure) {
                bool sawPress = false;

                for (int32 point = 0; point < points; point++) {
                    int32      at   = 0;
                    ParamValue held = 0.0;

                    if ((queue->getPoint(point, at, held) == kResultOk) && (held >= 0.5)) {
                        sawPress = true;
                        break;
                    }
                }

                gb_bridge_measure_trigger(bridge, sawPress, value >= 0.5);
                continue;
            }

            gb_bridge_parameter(bridge, id, value, offset, blockHostTime);
        }
    }

    // The same for the event list: read them out of VST3's own struct and hand each one on as the
    // note it is.
    void take_events(ProcessData & data, uint64_t blockHostTime) {
        if (data.inputEvents == nullptr) {
            return;
        }

        int32 count = data.inputEvents->getEventCount();

        for (int32 i = 0; i < count; i++) {
            Event event;

            if (data.inputEvents->getEvent(i, event) != kResultOk) {
                continue;
            }

            if (event.type == Event::kNoteOnEvent) {
                gb_bridge_note(bridge, eGbNoteOn, event.noteOn.channel, event.noteOn.pitch,
                               event.noteOn.velocity, event.sampleOffset, blockHostTime);
            } else if (event.type == Event::kNoteOffEvent) {
                gb_bridge_note(bridge, eGbNoteOff, event.noteOff.channel, event.noteOff.pitch,
                               event.noteOff.velocity, event.sampleOffset, blockHostTime);
            } else if (event.type == Event::kPolyPressureEvent) {
                gb_bridge_note(bridge, eGbPolyPressure, event.polyPressure.channel,
                               event.polyPressure.pitch, event.polyPressure.pressure,
                               event.sampleOffset, blockHostTime);
            }
        }
    }

    // THE ONE THING THE BRIDGE CANNOT DO FOR ITSELF. Making an IMessage needs the host application
    // object and delivering one needs the peer connection - both C++ interfaces, both held here.
    // The bridge posts an id and an int; this puts them on the wire.
    //
    // Only the CONTROLLER can tell the host that the latency changed, because restartComponent
    // lives on IComponentHandler, which a processor never sees. So even that goes over this channel.
    static void post_message(void * user, const char * id, int value) {
        ((GenBridgePlugin *)user)->send_message(id, value);
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

    static void name_to_utf16(const char * src, char16 * dst, int max) {
        to_utf16(src, dst, max);
    }

    std::atomic<int32> refCount;
    IHostApplication * host{nullptr};
    IConnectionPoint * peer{nullptr};

    // EVERYTHING THE PLUG-IN ACTUALLY IS, behind one pointer. See gbBridge.h.
    tGbBridge *        bridge{nullptr};
    const bool         instrument;
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
        } else if (strcmp(id, "gbSource") == 0) {
            // A STATE RESTORE CHANGED THE CAPTURE SOURCE, and the parameter is the host's copy of
            // it. Without this the panel and the processor drift apart the first time Live
            // re-applies a state that disagrees - and Live does that often.
            int64 wantsHost = -1;

            if ((message->getAttributes()->getInt("value", wantsHost) == kResultOk)
                && (wantsHost >= 0)) {
                source = (wantsHost > 0) ? 1.0 : 0.0;

                if (componentHandler != nullptr) {
                    componentHandler->beginEdit(kParamSource);
                    componentHandler->performEdit(kParamSource, source);
                    componentHandler->endEdit(kParamSource);
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

        tGbActive active;

        gb_state_parse_active(blob.data(), blob.size(), &active);

        // The MIDI half is restored even when no audio device was stored: an instrument may have had
        // its destination chosen and its capture not.
        if (active.midiName[0] != '\0') {
            gb_midi_invalidate();

            int slot = gb_midi_slot_for_name(active.midiName);

            if (slot >= 0) {
                midiDest = (double)slot / (double)(GB_MIDI_SLOTS - 1);
            }
        }

        midiChannel = (double)active.midiChannel / (double)(GB_CHANNEL_SLOTS - 1);
        note        = (double)active.testNote / 127.0;
        source      = active.hostInput ? 1.0 : 0.0;

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

        int slot = gb_slot_for_uid(active.uid);

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
                //
                // THE HEIGHT IS DERIVED, NOT RESTORED, and that is not tidiness. The panel is drawn
                // on a fixed logical canvas scaled by WIDTH alone, so a window whose height does not
                // match GB_CANVAS_H's aspect simply loses whatever falls past its bottom edge. A
                // saved height is the aspect of the canvas AS IT WAS: adding one row on 2026-09-09
                // took GB_CANVAS_H from 556 to 592, and every project saved before that reopened too
                // short - the last rows missing until the user resized the window and
                // checkSizeConstraint() put the aspect right. Deriving it here makes any future
                // change of canvas height self-correcting for sessions saved before it.
                (void)h;

                if ((w >= (GB_CANVAS_W * 0.75)) && (w <= (GB_CANVAS_W * 2.0))) {
                    editorWidth  = w;
                    editorHeight = w * (GB_CANVAS_H / GB_CANVAS_W);
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

    // ---- the parameter table, which is NOT VST3'S ----------------------------------------------
    //
    // What the parameters are, what their ranges mean and how a value reads is the plug-in
    // describing itself, and it lives in gbParams.c where the panel can share it. All three methods
    // below are the conversion into VST3's own shapes and nothing else: a ParameterInfo, and a
    // String128 of UTF-16.

    int32 PLUGIN_API getParameterCount(void) SMTG_OVERRIDE {
        return gb_param_count(instrument);
    }

    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo & info) SMTG_OVERRIDE {
        tGbParamInfo described;

        // ZEROED FIRST, EVEN ON THE FAILING PATH. A caller has no business reading info after a
        // kInvalidArgument, and one that does should see nothing rather than the LAST parameter's
        // details - which is what leaving it untouched gave, since the caller's struct is usually
        // reused round a loop. The C++ version zeroed it here; a comparison of the two builds
        // through the same harness is what noticed the difference.
        memset(&info, 0, sizeof(info));

        if (!gb_param_info(index, instrument, &described)) {
            return kInvalidArgument;
        }
        info.unitId                 = 0;           // kRootUnitId, without pulling in ivstunits.h
        info.id                     = described.id;
        info.stepCount              = described.stepCount;
        info.defaultNormalizedValue = described.defaultNormalized;
        info.flags                  = described.automatable
                                      ? (ParameterInfo::kCanAutomate
                                         | (described.list ? ParameterInfo::kIsList : 0))
                                      : ParameterInfo::kIsHidden;

        to_utf16(described.title, info.title, 128);
        to_utf16(described.shortTitle, info.shortTitle, 128);

        if (described.units[0] != '\0') {
            to_utf16(described.units, info.units, 128);
        }

        return kResultOk;
    }

    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue valueNormalized, String128 string) SMTG_OVERRIDE {
        char text[DEVICE_NAME_LEN + 8];

        if (!gb_param_text(id, valueNormalized, text, sizeof(text))) {
            return kInvalidArgument;
        }

        to_utf16(text, string, 128);

        return kResultOk;
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
            case kParamSource:   return source;
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
            case kParamSource:   source   = value; return kResultOk;
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

    // WHERE THE AUDIO COMES FROM: 0 an audio device, 1 the host's own input. A parameter the
    // controller has to hold like any other - the panel reads its value from here, and a host
    // restoring a project sets it here.
    ParamValue          source{0.0};
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
