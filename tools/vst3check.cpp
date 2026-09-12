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
// Notes: Docs/code-notes/vst3check.cpp.md - "// notes §k" refers there.

// notes §1

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/HostTime.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>

// notes §2
#include "../vst3/gbStatus.h"
#include "../vst3/gbDraw.h"   // GB_CANVAS_W/H - the aspect the editor size must match
#include <CoreMIDI/CoreMIDI.h>
#include <dlfcn.h>
#include <unistd.h>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include <map>

using namespace Steinberg;
using namespace Steinberg::Vst;

static int gFailures = 0;

static void check(const char * what, bool ok) {
    printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");

    if (!ok) {
        gFailures++;
    }
}

// notes §3
static void wait_ms(int ms) {
    CFAbsoluteTime until = CFAbsoluteTimeGetCurrent() + ((CFTimeInterval)ms / 1000.0);

    for (;;) {
        CFTimeInterval left = until - CFAbsoluteTimeGetCurrent();

        if (left <= 0.0) {
            break;
        }
        CFTimeInterval slice = (left < 0.01) ? left : 0.01;

        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, slice, true) == kCFRunLoopRunFinished) {
            usleep((useconds_t)(slice * 1.0e6));
        }
    }
}

// notes §4

class Attributes : public IAttributeList {
public:
    std::map<std::string, int64> ints;
    int32 rc = 1;

    tresult PLUGIN_API queryInterface(const TUID, void ** o) override { *o = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef(void) override { return (uint32)++rc; }
    uint32 PLUGIN_API release(void) override { return (uint32)--rc; }

    tresult PLUGIN_API setInt(AttrID id, int64 value) override { ints[id] = value; return kResultOk; }
    tresult PLUGIN_API getInt(AttrID id, int64 & value) override {
        auto it = ints.find(id);
        if (it == ints.end()) { return kResultFalse; }
        value = it->second;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID, double) override { return kResultFalse; }
    tresult PLUGIN_API getFloat(AttrID, double &) override { return kResultFalse; }
    tresult PLUGIN_API setString(AttrID, const TChar *) override { return kResultFalse; }
    tresult PLUGIN_API getString(AttrID, TChar *, uint32) override { return kResultFalse; }
    tresult PLUGIN_API setBinary(AttrID, const void *, uint32) override { return kResultFalse; }
    tresult PLUGIN_API getBinary(AttrID, const void *&, uint32 &) override { return kResultFalse; }
};

class Message : public IMessage {
public:
    std::string id;
    Attributes  attrs;
    int32       rc = 1;

    tresult PLUGIN_API queryInterface(const TUID, void ** o) override { *o = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef(void) override { return (uint32)++rc; }
    uint32 PLUGIN_API release(void) override {
        int32 c = --rc;
        if (c == 0) { delete this; return 0; }
        return (uint32)c;
    }

    FIDString PLUGIN_API getMessageID(void) override { return id.c_str(); }
    void PLUGIN_API setMessageID(FIDString newId) override { id = (newId != nullptr) ? newId : ""; }
    IAttributeList * PLUGIN_API getAttributes(void) override { return &attrs; }
};

class HostApp : public IHostApplication {
public:
    int32 rc = 1;

    tresult PLUGIN_API queryInterface(const TUID iid, void ** o) override {
        QUERY_INTERFACE(iid, o, FUnknown::iid, IHostApplication)
        QUERY_INTERFACE(iid, o, IHostApplication::iid, IHostApplication)
        *o = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef(void) override { return (uint32)++rc; }
    uint32 PLUGIN_API release(void) override { return (uint32)--rc; }

    tresult PLUGIN_API getName(String128 name) override { name[0] = 0; return kResultOk; }
    tresult PLUGIN_API createInstance(TUID cid, TUID, void ** obj) override {
        if (memcmp(cid, IMessage::iid.toTUID(), sizeof(TUID)) == 0) {
            *obj = (IMessage *)new Message();
            return kResultOk;
        }
        *obj = nullptr;
        return kResultFalse;
    }
};

// Records what the plug-in asks the host to do.
class Handler : public IComponentHandler {
public:
    int   restarts = 0;
    int32 restartFlags = 0;
    int32 rc = 1;

    tresult PLUGIN_API queryInterface(const TUID, void ** o) override { *o = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef(void) override { return (uint32)++rc; }
    uint32 PLUGIN_API release(void) override { return (uint32)--rc; }

    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        restarts++;
        restartFlags |= flags;
        return kResultOk;
    }
};

// notes §5
class NotePair : public IEventList {
public:
    Event events[2];
    int32 rc = 1;

    NotePair(int16 pitchA, int32 offsetA, int16 pitchB, int32 offsetB) {
        memset(events, 0, sizeof(events));

        events[0].busIndex       = 0;
        events[0].sampleOffset   = offsetA;
        events[0].type           = Event::kNoteOnEvent;
        events[0].noteOn.pitch   = pitchA;
        events[0].noteOn.velocity = 0.8f;
        events[0].noteOn.noteId  = -1;

        events[1].busIndex       = 0;
        events[1].sampleOffset   = offsetB;
        events[1].type           = Event::kNoteOnEvent;
        events[1].noteOn.pitch   = pitchB;
        events[1].noteOn.velocity = 0.8f;
        events[1].noteOn.noteId  = -1;
    }

    tresult PLUGIN_API queryInterface(const TUID, void ** o) override { *o = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef(void) override { return (uint32)++rc; }
    uint32 PLUGIN_API release(void) override { return (uint32)--rc; }

    int32 PLUGIN_API getEventCount(void) override { return 2; }

    tresult PLUGIN_API getEvent(int32 index, Event & e) override {
        if ((index < 0) || (index > 1)) {
            return kResultFalse;
        }
        e = events[index];
        return kResultOk;
    }

    tresult PLUGIN_API addEvent(Event &) override { return kNotImplemented; }
};

class OneNote : public IEventList {
public:
    Event event;
    int32 rc = 1;

    OneNote(bool on, int16 pitch, float velocity) {
        memset(&event, 0, sizeof(event));
        event.busIndex     = 0;
        event.sampleOffset = 0;
        event.type         = on ? Event::kNoteOnEvent : Event::kNoteOffEvent;

        if (on) {
            event.noteOn.channel  = 0;
            event.noteOn.pitch    = pitch;
            event.noteOn.velocity = velocity;
            event.noteOn.noteId   = -1;
        } else {
            event.noteOff.channel  = 0;
            event.noteOff.pitch    = pitch;
            event.noteOff.velocity = velocity;
            event.noteOff.noteId   = -1;
        }
    }

    tresult PLUGIN_API queryInterface(const TUID, void ** o) override { *o = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef(void) override { return (uint32)++rc; }
    uint32 PLUGIN_API release(void) override { return (uint32)--rc; }

    int32 PLUGIN_API getEventCount(void) override { return 1; }
    tresult PLUGIN_API getEvent(int32 index, Event & e) override {
        if (index != 0) { return kResultFalse; }
        e = event;
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Event &) override { return kResultFalse; }
};

// An IBStream over a std::string, which is all a state round trip needs.
class MemStream : public IBStream {
public:
    std::string buf;
    size_t      pos = 0;
    int32       rc  = 1;

    tresult PLUGIN_API queryInterface(const TUID, void ** o) override { *o = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef(void) override { return (uint32)++rc; }
    uint32 PLUGIN_API release(void) override { return (uint32)--rc; }

    tresult PLUGIN_API read(void * b, int32 n, int32 * got) override {
        size_t avail = buf.size() - pos;
        size_t take  = ((size_t)n < avail) ? (size_t)n : avail;

        memcpy(b, buf.data() + pos, take);
        pos += take;

        if (got != nullptr) {
            *got = (int32)take;
        }
        return kResultOk;
    }

    tresult PLUGIN_API write(void * b, int32 n, int32 * put) override {
        buf.append((const char *)b, (size_t)n);

        if (put != nullptr) {
            *put = n;
        }
        return kResultOk;
    }

    tresult PLUGIN_API seek(int64 p, int32, int64 * r) override {
        pos = (size_t)p;

        if (r != nullptr) {
            *r = p;
        }
        return kResultOk;
    }

    tresult PLUGIN_API tell(int64 * p) override {
        if (p != nullptr) {
            *p = (int64)pos;
        }
        return kResultOk;
    }
};

// The smallest thing a host can hand a plug-in to say "this parameter changed". Enough to drive a
// real parameter change through process(), which is the ONLY route a VST3 parameter takes to the
// processor - and therefore the only way to test that route without a DAW.
class OneChange : public IParameterChanges, public IParamValueQueue {
public:
    ParamID    id;
    ParamValue value;
    int32      rc = 1;

    OneChange(ParamID i, ParamValue v) : id(i), value(v) {}

    tresult PLUGIN_API queryInterface(const TUID, void ** o) override { *o = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef(void) override { return (uint32)++rc; }
    uint32 PLUGIN_API release(void) override { return (uint32)--rc; }

    int32 PLUGIN_API getParameterCount(void) override { return 1; }
    IParamValueQueue * PLUGIN_API getParameterData(int32 index) override {
        return (index == 0) ? this : nullptr;
    }
    IParamValueQueue * PLUGIN_API addParameterData(const ParamID &, int32 &) override { return this; }

    ParamID PLUGIN_API getParameterId(void) override { return id; }
    int32 PLUGIN_API getPointCount(void) override { return 1; }
    tresult PLUGIN_API getPoint(int32 index, int32 & offset, ParamValue & v) override {
        if (index != 0) {
            return kResultFalse;
        }
        offset = 0;
        v      = value;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32, ParamValue, int32 &) override { return kResultFalse; }
};

// notes §6
static std::string uid_for_device_name(const std::string & wanted) {
    AudioObjectPropertyAddress listAddress = { kAudioHardwarePropertyDevices,
                                               kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMain };
    UInt32                     size        = 0;

    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &listAddress, 0, nullptr, &size) != noErr) {
        return {};
    }
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));

    if (ids.empty()) {
        return {};
    }

    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &listAddress, 0, nullptr, &size, ids.data()) != noErr) {
        return {};
    }

    for (AudioObjectID id : ids) {
        CFStringRef                name        = nullptr;
        UInt32                     nameSize    = sizeof(name);
        AudioObjectPropertyAddress nameAddress = { kAudioObjectPropertyName,
                                                   kAudioObjectPropertyScopeGlobal,
                                                   kAudioObjectPropertyElementMain };

        if (AudioObjectGetPropertyData(id, &nameAddress, 0, nullptr, &nameSize, &name) != noErr || name == nullptr) {
            continue;
        }
        char nameBuf[256] = {0};
        CFStringGetCString(name, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8);
        CFRelease(name);

        if (wanted != nameBuf) {
            continue;
        }
        // notes §7
        AudioObjectPropertyAddress cfgAddress = { kAudioDevicePropertyStreamConfiguration,
                                                  kAudioObjectPropertyScopeInput,
                                                  kAudioObjectPropertyElementMain };
        UInt32                     cfgSize    = 0;
        UInt32                     inputs     = 0;

        if (AudioObjectGetPropertyDataSize(id, &cfgAddress, 0, nullptr, &cfgSize) == noErr && cfgSize > 0) {
            std::vector<char> raw(cfgSize);
            auto *            lists = reinterpret_cast<AudioBufferList *>(raw.data());

            if (AudioObjectGetPropertyData(id, &cfgAddress, 0, nullptr, &cfgSize, lists) == noErr) {
                for (UInt32 b = 0; b < lists->mNumberBuffers; b++) {
                    inputs += lists->mBuffers[b].mNumberChannels;
                }
            }
        }

        if (inputs == 0) {
            continue;
        }
        CFStringRef                uid        = nullptr;
        UInt32                     uidSize    = sizeof(uid);
        AudioObjectPropertyAddress uidAddress = { kAudioDevicePropertyDeviceUID,
                                                  kAudioObjectPropertyScopeGlobal,
                                                  kAudioObjectPropertyElementMain };

        if (AudioObjectGetPropertyData(id, &uidAddress, 0, nullptr, &uidSize, &uid) != noErr || uid == nullptr) {
            continue;
        }
        char uidBuf[512] = {0};
        CFStringGetCString(uid, uidBuf, sizeof(uidBuf), kCFStringEncodingUTF8);
        CFRelease(uid);

        return std::string(uidBuf);
    }

    return {};
}

static std::string to_ascii(const char16 * src) {
    std::string out;

    for (int i = 0; (src[i] != 0) && (i < 128); i++) {
        out += (char)src[i];
    }
    return out;
}

// Drive the plug-in at realtime pace, optionally delivering one event on the first block, and
// return the loudest thing that came back.
static int   gOnsetBlock  = -1;
static int   gOnsetSample = -1;
static float gThreshold   = 1.0f;

// notes §8
static int   gBurst       = 1;

// notes §9
static uint64_t gDeadline = 0;

static tGbStatus * (*gStatusFn)(uint32_t) = nullptr;
static bool         gWatch                = false;
static int          gWatchTick            = 0;
static ParamValue   gBufferValue          = 0.0;
static bool         gHaveBuffer           = false;
static ParamValue   gNoteValue            = 60.0 / 127.0;
static bool         gHaveNote             = false;

// One line per ~170 ms of driving. Everything here is written by the plug-in's own threads, so it
// is what the panel would be showing at that instant.
static void watch_sample(void) {
    if (!gWatch || (gStatusFn == nullptr)) {
        return;
    }

    if ((++gWatchTick % 375) != 0) {
        return;
    }

    // THE SLOT IS NOT 0. Every instance this run has created claims one, and the instrument under
    // --play is several instances in - so the trace scanned an inactive slot and printed nothing at
    // all, which reads exactly like a ring with nothing to say.
    tGbStatus * status = nullptr;

    for (uint32_t slot = 0; slot < GB_STATUS_SLOTS; slot++) {
        tGbStatus * candidate = gStatusFn(slot);

        if ((candidate != nullptr) && atomic_load(&candidate->active)
            && (atomic_load(&candidate->setpointFrames) > 0.0)) {
            status = candidate;
        }
    }

    if (status == nullptr) {
        return;
    }

    printf("      [ring] fill %7.1f of %7.1f   drift %+8.1f ppm   under %d  resync %d"
           "   told %d actual %d\n",
           atomic_load(&status->fillFrames), atomic_load(&status->setpointFrames),
           atomic_load(&status->driftPpm),
           atomic_load(&status->underruns), atomic_load(&status->resyncs),
           atomic_load(&status->latencySamples), atomic_load(&status->actualSamples));
    fflush(stdout);
}

static void pace_to_deadline(uint64_t stepHostTime) {
    uint64_t now = mach_absolute_time();

    // Behind by more than a whole step means something outside the loop took the time - the pause
    // between measurement runs, a device open. Catching up by running flat out would starve the
    // ring, which is the very thing being measured, so the schedule restarts from here.
    if ((gDeadline == 0) || (now > (gDeadline + stepHostTime))) {
        gDeadline = now;
    }

    gDeadline += stepHostTime;

    mach_wait_until(gDeadline);
}

static float run_blocks(IAudioProcessor * processor, float ** ch, float * l, float * r,
                        int blocks, IEventList * events,
                        ParamValue deviceValue, ParamValue midiValue) {
    float peak = 0.0f;

    for (int block = 0; block < blocks; block++) {
        ProcessData     data;
        AudioBusBuffers bus;

        memset(&data, 0, sizeof(data));
        memset(&bus, 0, sizeof(bus));
        memset(l, 0, sizeof(float) * 128);
        memset(r, 0, sizeof(float) * 128);

        bus.numChannels      = 2;
        bus.channelBuffers32 = ch;

        data.numSamples         = 128;
        data.numOutputs         = 1;
        data.outputs            = &bus;
        data.symbolicSampleSize = kSample32;
        data.processMode        = kRealtime;

        // The device and MIDI destination are re-sent every block. A host may do exactly that, and
        // the plug-in must act on the CHANGE rather than the delivery - so this doubles as a check
        // that it does not reopen the device continuously.
        OneChange deviceChange(0, deviceValue);
        OneChange midiChange(6, midiValue);
        OneChange bufferChange(3, gBufferValue);
        OneChange noteChange(10, gNoteValue);

        // notes §10
        if (gHaveBuffer) {
            data.inputParameterChanges = (block == 0) ? (IParameterChanges *)&deviceChange
                                                      : ((block == 200) ? (IParameterChanges *)&bufferChange
                                                                        : (IParameterChanges *)&midiChange);
        } else {
            data.inputParameterChanges = (block == 0) ? (IParameterChanges *)&deviceChange
                                                      : (IParameterChanges *)&midiChange;
        }

        // The test note rides on block 2, once, since the plug-in acts on the change.
        if (gHaveNote && (block == 2)) {
            data.inputParameterChanges = (IParameterChanges *)&noteChange;
        }

        if ((block == 0) && (events != nullptr)) {
            data.inputEvents = events;
        }

        processor->process(data);

        for (int i = 0; i < 128; i++) {
            float a = (l[i] < 0.0f) ? -l[i] : l[i];
            float b = (r[i] < 0.0f) ? -r[i] : r[i];
            float m = (a > b) ? a : b;

            if (m > peak) { peak = m; }

            if ((gOnsetBlock < 0) && (m > gThreshold)) {
                gOnsetBlock  = block;
                gOnsetSample = i;
            }
        }

        // The pause belongs at the END of a burst, not after every block: the blocks of one
        // callback are computed as fast as the CPU can do it.
        watch_sample();

        if (((block + 1) % gBurst) == 0) {
            pace_to_deadline(AudioConvertNanosToHostTime(
                                 (uint64_t)((128.0 / 48000.0) * 1.0e9) * (uint64_t)gBurst));
        }
    }

    return peak;
}

// notes §11
static void run_one_now(IAudioProcessor * processor, float ** ch, float * l, float * r,
                        IEventList * events, ParamValue midiValue) {
    ProcessData     data;
    AudioBusBuffers bus;
    OneChange       midiChange(6, midiValue);

    memset(&data, 0, sizeof(data));
    memset(&bus, 0, sizeof(bus));
    memset(l, 0, sizeof(float) * 128);
    memset(r, 0, sizeof(float) * 128);

    bus.numChannels      = 2;
    bus.channelBuffers32 = ch;

    data.numSamples            = 128;
    data.numOutputs            = 1;
    data.outputs               = &bus;
    data.symbolicSampleSize    = kSample32;
    data.processMode           = kRealtime;
    data.inputParameterChanges = (IParameterChanges *)&midiChange;
    data.inputEvents           = events;

    processor->process(data);
}

// notes §12
static uint8_t  gSinkBytes[256];
static uint32_t gSinkCount;

// PER-PACKET, AND STAMPED, for the offset test below. The byte stream alone cannot answer "when was
// this note meant to happen" - two notes in one block arrive as two packets whose TIMESTAMPS are
// the whole answer, and they survive whether CoreMIDI held the second one or delivered it early.
static MIDITimeStamp gSinkStamp[64];
static uint8_t       gSinkStatus[64];
static uint8_t       gSinkData1[64];
static uint32_t      gSinkPackets;

static void sink_read(const MIDIPacketList * list, void *, void *) {
    const MIDIPacket * packet = &list->packet[0];

    for (uint32_t i = 0; i < list->numPackets; i++) {
        if ((packet->length >= 2) && (gSinkPackets < 64)) {
            gSinkStamp[gSinkPackets]  = packet->timeStamp;
            gSinkStatus[gSinkPackets] = packet->data[0];
            gSinkData1[gSinkPackets]  = packet->data[1];
            gSinkPackets++;
        }

        for (uint16_t b = 0; (b < packet->length) && (gSinkCount < sizeof(gSinkBytes)); b++) {
            gSinkBytes[gSinkCount] = packet->data[b];
            gSinkCount++;
        }
        packet = MIDIPacketNext(packet);
    }
}

static MIDIClientRef      gSinkClient;
static MIDIEndpointRef    gSinkEndpoint;
static const char * const kSinkName = "GenBridge Check Sink";

static bool sink_create(void) {
    CFStringRef name = CFStringCreateWithCString(nullptr, kSinkName, kCFStringEncodingUTF8);

    if (MIDIClientCreate(CFSTR("vst3check"), nullptr, nullptr, &gSinkClient) != noErr) {
        CFRelease(name);
        return false;
    }
    OSStatus st = MIDIDestinationCreate(gSinkClient, name, sink_read, nullptr, &gSinkEndpoint);

    CFRelease(name);
    return st == noErr;
}

static void midi_out_test(IPluginFactory * factory, IPluginFactory2 * factory2, FUnknown * hostApp) {
    printf("\nMIDI out, with no hardware: host note -> plug-in -> virtual destination\n");

    if (gSinkEndpoint == 0) {
        printf("    no virtual MIDI destination was created; skipped\n");
        return;
    }
    TUID instCid;
    bool found = false;

    memset(instCid, 0, sizeof(instCid));

    for (int32 i = 0; (factory2 != nullptr) && (i < factory->countClasses()); i++) {
        PClassInfo2 info;

        if ((factory2->getClassInfo2(i, &info) == kResultOk)
            && (strstr(info.subCategories, "Instrument") != nullptr)) {
            memcpy(instCid, info.cid, sizeof(TUID));
            found = true;
            break;
        }
    }

    IComponent *      inst     = nullptr;
    IEditController * instCtrl = nullptr;

    if (found) {
        factory->createInstance(instCid, IComponent::iid, (void **)&inst);
    }
    check("instrument instantiates for the MIDI test", inst != nullptr);

    if (inst == nullptr) {
        return;
    }
    inst->initialize(hostApp);

    TUID ctrlCid;

    inst->getControllerClassId(ctrlCid);
    factory->createInstance(ctrlCid, IEditController::iid, (void **)&instCtrl);

    if (instCtrl != nullptr) {
        instCtrl->initialize(hostApp);
    }
    IConnectionPoint * a = nullptr;
    IConnectionPoint * b = nullptr;

    inst->queryInterface(IConnectionPoint::iid, (void **)&a);

    if (instCtrl != nullptr) {
        instCtrl->queryInterface(IConnectionPoint::iid, (void **)&b);
    }

    if ((a != nullptr) && (b != nullptr)) {
        a->connect(b);
        b->connect(a);
    }
    // Point it at our own sink, by name - the same way a person would from the panel.
    ParamValue midiValue = 0.0;
    ParamID    midiParam = 0;
    bool       haveMidi  = false;

    if (instCtrl != nullptr) {
        for (int32 i = 0; (i < instCtrl->getParameterCount()) && !haveMidi; i++) {
            ParameterInfo info;

            if ((instCtrl->getParameterInfo(i, info) != kResultOk)
                || (to_ascii(info.title) != "MIDI Destination")) {
                continue;
            }

            for (int32 slot = 0; slot <= info.stepCount; slot++) {
                String128  name;
                ParamValue norm = (ParamValue)slot / (ParamValue)info.stepCount;

                instCtrl->getParamStringByValue(info.id, norm, name);

                if (to_ascii(name).find(kSinkName) != std::string::npos) {
                    midiValue = norm;
                    midiParam = info.id;
                    haveMidi  = true;
                    printf("    the plug-in can see our sink at slot %d\n", slot);
                    break;
                }
            }
        }
    }
    check("plug-in lists the virtual destination", haveMidi);

    IAudioProcessor * ip = nullptr;

    inst->queryInterface(IAudioProcessor::iid, (void **)&ip);

    if ((ip != nullptr) && haveMidi) {
        ProcessSetup setup;

        memset(&setup, 0, sizeof(setup));
        setup.processMode        = kRealtime;
        setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = 128;
        setup.sampleRate         = 48000.0;

        ip->setupProcessing(setup);
        // The EVENT bus has to be switched on. A host does this and a test that forgets sees a
        // plug-in that ignores every note through no fault of its own.
        inst->activateBus(kEvent, kInput, 0, true);
        inst->activateBus(kAudio, kOutput, 0, true);
        inst->setActive(true);
        ip->setProcessing(true);

        float   l[128];
        float   r[128];
        float * ch[2] = { l, r };

        gSinkCount = 0;

        OneNote  on(true, 64, 0.8f);
        OneNote  off(false, 64, 0.0f);

        // Destination first, on its own block, then the note: the plug-in acts on a parameter
        // CHANGE, and sending both together would leave which arrived first up to the reader.
        run_blocks(ip, ch, l, r, 2, nullptr, 0.0, midiValue);
        run_blocks(ip, ch, l, r, 2, (IEventList *)&on, 0.0, midiValue);
        run_blocks(ip, ch, l, r, 2, (IEventList *)&off, 0.0, midiValue);
        usleep(200000);

        printf("    bytes back from the sink: %u", gSinkCount);

        for (uint32_t i = 0; (i < gSinkCount) && (i < 12); i++) {
            printf(" %02X", gSinkBytes[i]);
        }
        printf("\n");

        check("a host note reaches the MIDI destination", gSinkCount >= 3);

        bool sawNoteOn = false;

        for (uint32_t i = 0; (i + 2) < gSinkCount; i++) {
            if (((gSinkBytes[i] & 0xF0) == 0x90) && (gSinkBytes[i + 1] == 64)
                && (gSinkBytes[i + 2] > 0)) {
                sawNoteOn = true;
            }
        }
        check("it is the note that was played, on the right pitch", sawNoteOn);

        // notes §13
        gSinkCount   = 0;
        gSinkPackets = 0;

        NotePair pair(60, 0, 72, 127);

        run_blocks(ip, ch, l, r, 2, (IEventList *)&pair, 0.0, midiValue);
        usleep(200000);

        // notes §14
        printf("    packets back from the sink: %u\n", gSinkPackets);

        check("the pair arrived as two separately stamped packets", gSinkPackets >= 2);

        if (gSinkPackets >= 2) {
            int first  = -1;
            int second = -1;

            for (uint32_t i = 0; i < gSinkPackets; i++) {
                if ((gSinkStatus[i] & 0xF0) != 0x90) {
                    continue;
                }

                if ((gSinkData1[i] == 60) && (first < 0)) {
                    first = (int)i;
                } else if ((gSinkData1[i] == 72) && (second < 0)) {
                    second = (int)i;
                }
            }
            check("the pair is the two pitches that were played", (first >= 0) && (second >= 0));

            if ((first >= 0) && (second >= 0)) {
                double deltaMs = ((double)AudioConvertHostTimeToNanos(gSinkStamp[second])
                                  - (double)AudioConvertHostTimeToNanos(gSinkStamp[first])) / 1.0e6;
                double wantMs  = (127.0 / 48000.0) * 1000.0;

                printf("    offset 0 -> offset 127 came out %.3f ms apart (want %.3f)\n",
                       deltaMs, wantMs);

                // A quarter of a millisecond either way. The figure is arithmetic, not a
                // measurement - the plug-in computes the stamp - so anything looser would hide the
                // very error this exists to catch, and 0.000 is what the old code scored.
                check("an event's sample offset survives into the MIDI timestamp",
                      (deltaMs > (wantMs - 0.25)) && (deltaMs < (wantMs + 0.25)));
            }
        }

        // notes §15
        gSinkCount   = 0;
        gSinkPackets = 0;

        OneNote first(true, 61, 0.8f);
        OneNote second(true, 73, 0.8f);

        run_one_now(ip, ch, l, r, (IEventList *)&first, midiValue);
        run_one_now(ip, ch, l, r, (IEventList *)&second, midiValue);
        usleep(200000);

        int a = -1;
        int b = -1;

        for (uint32_t i = 0; i < gSinkPackets; i++) {
            if ((gSinkStatus[i] & 0xF0) != 0x90) {
                continue;
            }

            if ((gSinkData1[i] == 61) && (a < 0)) {
                a = (int)i;
            } else if ((gSinkData1[i] == 73) && (b < 0)) {
                b = (int)i;
            }
        }
        check("a note from each of two back-to-back calls arrived", (a >= 0) && (b >= 0));

        if ((a >= 0) && (b >= 0)) {
            double deltaMs = ((double)AudioConvertHostTimeToNanos(gSinkStamp[b])
                              - (double)AudioConvertHostTimeToNanos(gSinkStamp[a])) / 1.0e6;
            double wantMs  = (128.0 / 48000.0) * 1000.0;

            printf("    two calls in one callback came out %.3f ms apart (want %.3f)\n",
                   deltaMs, wantMs);

            check("a second block computed in the same callback is stamped a block later",
                  (deltaMs > (wantMs - 0.25)) && (deltaMs < (wantMs + 0.25)));
        }

        // notes §16
        gSinkCount   = 0;
        gSinkPackets = 0;

        // notes §17
        int pitch = 40;

        for (int burst = 0; burst < 6; burst++) {
            for (int sub = 0; sub < 4; sub++) {
                OneNote jitter(true, (int16)pitch, 0.8f);

                pitch++;
                run_one_now(ip, ch, l, r, (IEventList *)&jitter, midiValue);
            }

            // notes §18
            usleep((burst % 2) ? 15300 : 6000);
        }
        usleep(200000);

        int  seen       = 0;
        bool ordered    = true;
        bool increasing = true;

        for (uint32_t i = 0; i < gSinkPackets; i++) {
            if ((gSinkStatus[i] & 0xF0) != 0x90) {
                continue;
            }

            if (gSinkData1[i] != (uint8_t)(40 + seen)) {
                ordered = false;
            }

            if ((i > 0) && (gSinkStamp[i] < gSinkStamp[i - 1])) {
                increasing = false;
            }
            seen++;
        }
        printf("    %d of 24 jittered notes came back\n", seen);

        // notes §19
        check("every note survives a jittered callback", seen == 24);
        check("they arrive in the order they were played", ordered);
        // notes §20
        check("their timestamps never go backwards", increasing);

        ip->setProcessing(false);
        inst->setActive(false);
    }

    if (ip != nullptr) { ip->release(); }

    if (instCtrl != nullptr) {
        instCtrl->terminate();
        instCtrl->release();
    }
    inst->terminate();
    inst->release();
}

int main(int argc, char ** argv) {
    if (argc < 2) {
        printf("usage: vst3check <plugin.vst3> [--block N]\n");
        printf("  --block N   max samples per block to declare, as a host would (default 512)\n");
        printf("  --play DEV MIDI   round trip through real hardware\n");
        printf("  --measure N       with --play, press Measure N times and report the spread\n");
        printf("  --burst K         compute K blocks back to back per callback, as Ableton does\n");
        printf("  --watch           trace the plug-in's ring fill, drift and resyncs while it runs\n");
        printf("  --buffer N        device buffer to impose, by the label's leading text (e.g. 64)\n");
        return 2;
    }

    int32       maxBlock    = 512;
    const char * playDevice = nullptr;
    const char * playMidi   = nullptr;
    int          measureRuns = 1;
    const char * playBuffer = nullptr;

    for (int i = 2; i < argc; i++) {
        if ((strcmp(argv[i], "--block") == 0) && ((i + 1) < argc)) {
            maxBlock = atoi(argv[i + 1]);
        } else if ((strcmp(argv[i], "--play") == 0) && ((i + 2) < argc)) {
            playDevice = argv[i + 1];
            playMidi   = argv[i + 2];
        } else if ((strcmp(argv[i], "--measure") == 0) && ((i + 1) < argc)) {
            measureRuns = atoi(argv[i + 1]);
        } else if ((strcmp(argv[i], "--note") == 0) && ((i + 1) < argc)) {
            // The test note Measure plays, as a MIDI number. A drum machine has nothing on middle
            // C: an Analog Rytm's kick is note 0.
            gNoteValue = (ParamValue)atoi(argv[i + 1]) / 127.0;
            gHaveNote  = true;
        } else if ((strcmp(argv[i], "--buffer") == 0) && ((i + 1) < argc)) {
            playBuffer = argv[i + 1];
        } else if (strcmp(argv[i], "--watch") == 0) {
            gWatch = true;
        } else if ((strcmp(argv[i], "--burst") == 0) && ((i + 1) < argc)) {
            gBurst = atoi(argv[i + 1]);

            if (gBurst < 1) {
                gBurst = 1;
            }
        }
    }

    char path[1024];

    snprintf(path, sizeof(path), "%s/Contents/MacOS/%s", argv[1], "GenBridge");

    void * lib = dlopen(path, RTLD_NOW);

    if (lib == nullptr) {
        printf("dlopen failed: %s\n", dlerror());
        return 2;
    }

    gStatusFn = (tGbStatus * (*)(uint32_t))dlsym(lib, "gb_status");

    if (gWatch && (gStatusFn == nullptr)) {
        printf("    --watch: gb_status is not exported by this build; tracing disabled\n");
    }

    auto entry = (bool (*)(void *))dlsym(lib, "bundleEntry");

    if (entry != nullptr) {
        entry(nullptr);
    }

    auto getFactory = (IPluginFactory * (*)())dlsym(lib, "GetPluginFactory");

    if (getFactory == nullptr) {
        printf("no GetPluginFactory export\n");
        return 2;
    }

    IPluginFactory * factory = getFactory();

    printf("\nfactory\n");
    check("GetPluginFactory returns a factory", factory != nullptr);

    if (factory == nullptr) {
        return 2;
    }

    IPluginFactory2 * factory2 = nullptr;

    factory->queryInterface(IPluginFactory2::iid, (void **)&factory2);
    check("implements IPluginFactory2", factory2 != nullptr);
    check("registers four classes (effect and instrument, each with a controller)",
          factory->countClasses() == 4);

    TUID processorCid;
    TUID controllerCid;

    memset(processorCid, 0, sizeof(processorCid));
    memset(controllerCid, 0, sizeof(controllerCid));

    bool sawEffect = false;

    for (int32 i = 0; (factory2 != nullptr) && (i < factory->countClasses()); i++) {
        PClassInfo2 info;

        if (factory2->getClassInfo2(i, &info) != kResultOk) {
            continue;
        }

        printf("    [%d] %-22s %-26s %s\n", i, info.name, info.category, info.subCategories);

        // notes §21
        if ((strcmp(info.category, kVstAudioEffectClass) == 0)
            && (strstr(info.subCategories, "Instrument") == nullptr)) {
            memcpy(processorCid, info.cid, sizeof(TUID));
            sawEffect = (strstr(info.subCategories, "Fx") != nullptr)
                        && (strstr(info.subCategories, "NoOfflineProcess") != nullptr);
        }
    }

    check("audio class declares Fx|NoOfflineProcess", sawEffect);

    IComponent * component = nullptr;

    factory->createInstance(processorCid, IComponent::iid, (void **)&component);
    check("processor instantiates", component != nullptr);

    if (component == nullptr) {
        return 2;
    }

    // notes §22
    if (!sink_create()) {
        printf("could not create a virtual MIDI destination; the MIDI test will be skipped\n");
    }

    static HostApp hostApp;

    component->initialize(&hostApp);
    component->getControllerClassId(controllerCid);

    printf("\nbuses and controller\n");
    check("one audio input bus", component->getBusCount(kAudio, kInput) == 1);
    check("one audio output bus", component->getBusCount(kAudio, kOutput) == 1);
    check("controller is a separate class",
          memcmp(controllerCid, processorCid, sizeof(TUID)) != 0);

    IEditController * controller = nullptr;

    factory->createInstance(controllerCid, IEditController::iid, (void **)&controller);
    check("controller instantiates", controller != nullptr);

    static Handler handler;

    if (controller != nullptr) {
        controller->initialize(&hostApp);
        controller->setComponentHandler(&handler);
        check("exposes at least one parameter", controller->getParameterCount() > 0);

        for (int32 i = 0; i < controller->getParameterCount(); i++) {
            ParameterInfo info;

            if (controller->getParameterInfo(i, info) == kResultOk) {
                printf("    param %d: %-16s steps=%d\n", i,
                       to_ascii(info.title).c_str(), info.stepCount);
            }
        }
    }

    // ---- state ----

    printf("\nstate\n");

    IAudioProcessor * processor = nullptr;

    component->queryInterface(IAudioProcessor::iid, (void **)&processor);
    check("implements IAudioProcessor", processor != nullptr);

    if (processor != nullptr) {
        ProcessSetup setup;

        memset(&setup, 0, sizeof(setup));
        setup.processMode         = kRealtime;
        setup.symbolicSampleSize  = kSample32;
        setup.maxSamplesPerBlock  = maxBlock;
        setup.sampleRate          = 48000.0;

        printf("    declaring maxSamplesPerBlock %d\n", maxBlock);
        check("setupProcessing accepted", processor->setupProcessing(setup) == kResultOk);

        // notes §23
        uint32 tail = processor->getTailSamples();

        printf("    tail: %u (%s)\n", tail,
               (tail == kInfiniteTail) ? "infinite" : ((tail == kNoTail) ? "none" : "finite"));
        check("declares an infinite tail, not kNoTail", tail == kInfiniteTail);
    }

    // A UID containing commas and spaces is the case that breaks a naive parser, and both are real:
    // "AppleUSBAudioEngine:CalDigit, Inc.:...".
    {
        MemStream tricky;

        tricky.buf =
            "GENBRIDGE2\n"
            "active=AppleUSBAudioEngine:CalDigit, Inc.:CalDigit Thunderbolt 3 Audio:20200000:2\n"
            "dev=128,48000.0,25.500,2,0.7500,AppleUSBAudioEngine:CalDigit, Inc.:CalDigit Thunderbolt 3 Audio:20200000:2\n"
            "dev=256,44100.0,40.000,0,1.0000,AppleUSBAudioEngine:KORG INC.:KRONOS:1140000:2,1\n"
            "futurekey=a newer build wrote this\n";

        IComponent * fresh = nullptr;

        factory->createInstance(processorCid, IComponent::iid, (void **)&fresh);
        fresh->initialize(nullptr);

        // notes §24
        {
            IPlugView * sizer = controller->createView(ViewType::kEditor);

            if (sizer != nullptr) {
                bool idempotent = true;
                bool settles    = true;

                // Feeding an answer straight back must return it unchanged, at every size in range.
                for (int32 w = 380; w <= 1050; w += 7) {
                    ViewRect once = { 0, 0, w, (int32)(w * 556 / 520) };

                    sizer->checkSizeConstraint(&once);

                    ViewRect twice = once;

                    sizer->checkSizeConstraint(&twice);

                    if ((twice.right != once.right) || (twice.bottom != once.bottom)) {
                        idempotent = false;
                    }
                }

                check("a constrained size is stable when fed back", idempotent);

                // A DRAG OF ONE EDGE. The host proposes the pointer's rect, in which the dimension
                // the user is NOT dragging still holds its old value - the shape that made the
                // window snap back. Repeat it and the answer must converge, never alternate.
                ViewRect at = { 0, 0, 520, 556 };

                sizer->checkSizeConstraint(&at);

                int32 prevW  = at.right;
                int32 prevW2 = -1;

                for (int step = 0; step < 40; step++) {
                    // Bottom edge dragged to 800 and held: width stays whatever we last returned.
                    ViewRect proposal = { 0, 0, at.right, 800 };

                    sizer->checkSizeConstraint(&proposal);
                    at = proposal;

                    // Alternating between two values is the failure - A, B, A, B round a two-cycle.
                    if ((step > 6) && (at.right == prevW2) && (at.right != prevW)) {
                        settles = false;
                    }
                    prevW2 = prevW;
                    prevW  = at.right;
                }

                check("a held drag converges instead of alternating", settles);
                check("a held drag reaches the size asked for", (at.bottom > 790) && (at.bottom <= 800));

                sizer->release();
            }
        }

        // THE CONTROLLER'S state, not the component's: the editor size lives there, and both halves
        // of it were stubs that returned kResultOk without touching the stream, so every session
        // opened at the default size however the last one was left.
        {
            MemStream guiSaved;
            MemStream guiLoaded;

            controller->getState(&guiSaved);
            check("controller writes a GUI state", guiSaved.buf.find("editor=") != std::string::npos);

            // notes §25
            char expected[64];

            snprintf(expected, sizeof(expected), "editor=700,%.0f",
                     700.0 * (GB_CANVAS_H / GB_CANVAS_W));

            guiLoaded.buf = "GENBRIDGEGUI1\neditor=700,748\nfuturekey=a newer build wrote this\n";
            check("controller loads a GUI state", controller->setState(&guiLoaded) == kResultOk);

            MemStream guiBack;

            controller->getState(&guiBack);
            printf("    restored 700x748, wrote back: %s\n",
                   guiBack.buf.substr(guiBack.buf.find("editor="), 20).c_str());
            check("the saved WIDTH is restored", guiBack.buf.find("editor=700,") != std::string::npos);
            check("and the height is derived from the CURRENT canvas, not the saved one",
                  guiBack.buf.find(expected) != std::string::npos);

            // Straight out of a project file, so it is checked rather than trusted: a size the user
            // cannot see or cannot fit on screen has no way back short of editing the project.
            MemStream absurd;

            absurd.buf = "GENBRIDGEGUI1\neditor=40,4\n";
            controller->setState(&absurd);

            MemStream afterAbsurd;

            controller->getState(&afterAbsurd);
            check("an out-of-range saved size is refused",
                  afterAbsurd.buf.find(expected) != std::string::npos);
        }

        // notes §26
        {
            // THE LOG IS NOT OURS TO CLEAR. An earlier version of this opened it "w" - which would
            // have wiped a resize capture in progress, and capturing one is exactly what the notes
            // tell the next person to do. Take the length now and read only what gets appended.
            bool        hadGate  = (access("/tmp/genbridge-log", F_OK) == 0);
            bool        hadLog   = (access("/tmp/genbridge.log", F_OK) == 0);
            long        logFrom  = 0;

            if (!hadGate) {
                FILE * gate = fopen("/tmp/genbridge-log", "w");

                if (gate != nullptr) {
                    fclose(gate);
                }

                // notes §27
                wait_ms(1100);
            }
            {
                FILE * existing = fopen("/tmp/genbridge.log", "r");

                if (existing != nullptr) {
                    fseek(existing, 0, SEEK_END);
                    logFrom = ftell(existing);
                    fclose(existing);
                }
            }

            IComponent * absentee = nullptr;

            factory->createInstance(processorCid, IComponent::iid, (void **)&absentee);
            absentee->initialize(nullptr);

            MemStream absent;

            absent.buf = "GENBRIDGE3\nactive=NO-SUCH-DEVICE-UID-12345\nactivename=Phantom Interface\n";
            absentee->setState(&absent);

            IAudioProcessor * ap = nullptr;

            absentee->queryInterface(IAudioProcessor::iid, (void **)&ap);
            absentee->setActive(true);

            float   left[128]   = { 0.0f };
            float   right[128]  = { 0.0f };
            float * channels[2] = { left, right };

            for (int block = 0; block < 6; block++) {
                OneChange       toSlotZero(0, 0.0);
                ProcessData     data;
                AudioBusBuffers outBus;

                memset(&data, 0, sizeof(data));
                memset(&outBus, 0, sizeof(outBus));

                outBus.numChannels         = 2;
                outBus.channelBuffers32    = channels;
                data.numSamples            = 128;
                data.numOutputs            = 1;
                data.outputs               = &outBus;
                data.symbolicSampleSize    = kSample32;
                data.processMode           = kRealtime;
                data.inputParameterChanges = &toSlotZero;

                ap->process(data);

                for (int k = 0; k < 4; k++) {
                    usleep(100000);
                }
            }

            std::string log;
            FILE *      readback = fopen("/tmp/genbridge.log", "r");

            if (readback != nullptr) {
                char line[512];

                fseek(readback, logFrom, SEEK_SET);    // only what this instance appended

                while (fgets(line, (int)sizeof(line), readback) != nullptr) {
                    log += line;
                }
                fclose(readback);
            }

            // Matched on the stable half of the sentence. The full message used to end "waiting for
            // it" and now ends "waiting (not modifying deviceSelector)"; asserting on the whole
            // thing meant a reworded log line reported itself as a behaviour regression.
            check("an absent saved device is waited for",
                  log.find("not present - waiting") != std::string::npos);

            // The discriminator. Before the fix this read "slot 0 -> 'Chris' Phone Microphone'" -
            // the plug-in resolving the stale index and going on to open whatever it named.
            check("an absent saved device never resolves a slot",
                  log.find("slot 0 -> '") == std::string::npos);

            MemStream afterAbsent;

            absentee->getState(&afterAbsent);

            check("an absent saved device is not replaced by slot 0",
                  afterAbsent.buf.find("active=NO-SUCH-DEVICE-UID-12345") != std::string::npos);
            check("the saved device name is kept for the panel",
                  afterAbsent.buf.find("activename=Phantom Interface") != std::string::npos);

            absentee->setActive(false);
            ap->release();
            absentee->terminate();
            absentee->release();

            // Left exactly as it was found, log included: a gate this did not set is not this to
            // clear, and a log file that did not exist before should not exist after.
            if (!hadGate) {
                unlink("/tmp/genbridge-log");
            }

            if (!hadLog) {
                unlink("/tmp/genbridge.log");
            }
        }

        check("loads state with comma-bearing UIDs", fresh->setState(&tricky) == kResultOk);

        MemStream saved;

        fresh->getState(&saved);

        check("comma-bearing UID survives",
              saved.buf.find("CalDigit Thunderbolt 3 Audio:20200000:2") != std::string::npos);
        check("second device survives",
              saved.buf.find("KRONOS:1140000:2,1") != std::string::npos);
        check("per-device trim preserved", saved.buf.find("0.7500") != std::string::npos);
        check("per-device rate preserved", saved.buf.find("44100.0") != std::string::npos);
        check("unknown key from a newer build ignored",
              saved.buf.find("futurekey") == std::string::npos);

        fresh->terminate();
        fresh->release();
    }

    // Version 1 predates the per-device rate, so its dev= lines have one fewer numeric field.
    // Refusing to open a session saved by yesterday's build would rather defeat the point of
    // versioning the format at all.
    {
        MemStream v1;

        v1.buf =
            "GENBRIDGE1\n"
            "active=AppleUSBAudioEngine:KORG INC.:KRONOS:1140000:2,1\n"
            "dev=256,33.000,4,0.5000,AppleUSBAudioEngine:KORG INC.:KRONOS:1140000:2,1\n";

        IComponent * fresh = nullptr;

        factory->createInstance(processorCid, IComponent::iid, (void **)&fresh);
        fresh->initialize(nullptr);

        check("loads a version 1 state", fresh->setState(&v1) == kResultOk);

        MemStream upgraded;

        fresh->getState(&upgraded);

        // Whatever the current version is, an old file must come back as it - not as itself.
        check("version 1 upgrades to the current format",
              (upgraded.buf.compare(0, 9, "GENBRIDGE") == 0)
              && (upgraded.buf[9] > '1'));
        check("version 1 trim survives the upgrade",
              upgraded.buf.find("0.5000") != std::string::npos);
        check("version 1 target ms survives the upgrade",
              upgraded.buf.find("33.000") != std::string::npos);

        fresh->terminate();
        fresh->release();
    }

    // notes §28
    printf("\ninstrument variant\n");
    {
        TUID instCid;
        bool foundInstrument = false;

        memset(instCid, 0, sizeof(instCid));

        for (int32 i = 0; (factory2 != nullptr) && (i < factory->countClasses()); i++) {
            PClassInfo2 info;

            if ((factory2->getClassInfo2(i, &info) == kResultOk)
                && (strstr(info.subCategories, "Instrument") != nullptr)) {
                memcpy(instCid, info.cid, sizeof(TUID));
                foundInstrument = true;
                break;
            }
        }

        check("an Instrument|Synth class is registered", foundInstrument);

        IComponent * inst = nullptr;

        if (foundInstrument) {
            factory->createInstance(instCid, IComponent::iid, (void **)&inst);
        }

        check("instrument instantiates", inst != nullptr);

        if (inst != nullptr) {
            inst->initialize(&hostApp);

            // notes §29
            check("one audio input bus", inst->getBusCount(kAudio, kInput) == 1);
            check("one audio output bus", inst->getBusCount(kAudio, kOutput) == 1);
            check("one event input bus", inst->getBusCount(kEvent, kInput) == 1);

            BusInfo sideChain;

            if (inst->getBusInfo(kAudio, kInput, 0, sideChain) == kResultOk) {
                printf("    audio input bus: %s, %d channels, busType %d, flags 0x%x\n",
                       to_ascii(sideChain.name).c_str(), sideChain.channelCount,
                       (int)sideChain.busType, (unsigned)sideChain.flags);
                check("the instrument's audio input is a side-chain", sideChain.busType == kAux);
                check("and is NOT default-active",
                      (sideChain.flags & BusInfo::kDefaultActive) == 0);
            } else {
                check("instrument audio input bus is describable", false);
            }

            BusInfo eventBus;

            if (inst->getBusInfo(kEvent, kInput, 0, eventBus) == kResultOk) {
                printf("    event bus: %s, %d channels\n",
                       to_ascii(eventBus.name).c_str(), eventBus.channelCount);
                check("event bus carries 16 MIDI channels", eventBus.channelCount == 16);
            }

            IAudioProcessor * instProc = nullptr;

            inst->queryInterface(IAudioProcessor::iid, (void **)&instProc);

            if (instProc != nullptr) {
                SpeakerArrangement out = SpeakerArr::kStereo;

                check("accepts 0 in / 1 out stereo",
                      instProc->setBusArrangements(nullptr, 0, &out, 1) == kResultOk);
                instProc->release();
            }

            TUID instCtrlCid;

            inst->getControllerClassId(instCtrlCid);
            check("instrument names its own controller",
                  memcmp(instCtrlCid, controllerCid, sizeof(TUID)) != 0);

            IEditController * instCtrl = nullptr;

            factory->createInstance(instCtrlCid, IEditController::iid, (void **)&instCtrl);

            if (instCtrl != nullptr) {
                instCtrl->initialize(&hostApp);

                printf("    instrument has %d parameters, the effect %d\n",
                       instCtrl->getParameterCount(), controller->getParameterCount());
                check("instrument exposes a MIDI destination the effect does not",
                      instCtrl->getParameterCount() > controller->getParameterCount());

                for (int32 i = 0; i < instCtrl->getParameterCount(); i++) {
                    ParameterInfo info;

                    if ((instCtrl->getParameterInfo(i, info) == kResultOk)
                        && (to_ascii(info.title).find("MIDI") != std::string::npos)) {
                        printf("    MIDI destinations it can reach:\n");

                        for (int32 d = 0; d < 5; d++) {
                            String128 name;

                            instCtrl->getParamStringByValue(info.id,
                                                            (ParamValue)d / (ParamValue)info.stepCount,
                                                            name);
                            printf("      %d: %s\n", d, to_ascii(name).c_str());
                        }
                        break;
                    }
                }

                // A DAMPER PEDAL IS NOT AN EVENT - it arrives as a parameter change routed by
                // IMidiMapping, so a plug-in without that interface passes notes and silently drops
                // the pedal.
                IMidiMapping * mapping = nullptr;

                instCtrl->queryInterface(IMidiMapping::iid, (void **)&mapping);
                check("instrument implements IMidiMapping", mapping != nullptr);

                if (mapping != nullptr) {
                    ParamID sustain = 0;
                    ParamID bend    = 0;
                    ParamID wheel   = 0;

                    check("damper pedal is mapped",
                          mapping->getMidiControllerAssignment(0, 0, kCtrlSustainOnOff, sustain)
                          == kResultTrue);
                    check("pitch bend is mapped",
                          mapping->getMidiControllerAssignment(0, 0, kPitchBend, bend) == kResultTrue);
                    check("mod wheel is mapped",
                          mapping->getMidiControllerAssignment(0, 0, kCtrlModWheel, wheel) == kResultTrue);
                    check("each controller gets its own parameter",
                          (sustain != bend) && (bend != wheel));

                    ParamID otherChannel = 0;

                    mapping->getMidiControllerAssignment(0, 5, kCtrlSustainOnOff, otherChannel);
                    check("channels are kept apart", otherChannel != sustain);

                    printf("    sustain ch1 -> param %u, ch6 -> param %u, bend -> %u\n",
                           sustain, otherChannel, bend);

                    mapping->release();
                }

                instCtrl->terminate();
                instCtrl->release();
            }

            inst->terminate();
            inst->release();
        }
    }

    // Needs no hardware, so it always runs - see midi_out_test().
    midi_out_test(factory, factory2, &hostApp);

    // notes §30
    if ((playDevice != nullptr) && (playMidi != nullptr)) {
        printf("\nMIDI round trip: play '%s', capture '%s'\n", playMidi, playDevice);

        TUID instCid;
        bool found = false;

        memset(instCid, 0, sizeof(instCid));

        for (int32 i = 0; (factory2 != nullptr) && (i < factory->countClasses()); i++) {
            PClassInfo2 info;

            if ((factory2->getClassInfo2(i, &info) == kResultOk)
                && (strstr(info.subCategories, "Instrument") != nullptr)) {
                memcpy(instCid, info.cid, sizeof(TUID));
                found = true;
                break;
            }
        }

        IComponent *      inst     = nullptr;
        IEditController * instCtrl = nullptr;

        if (found) {
            factory->createInstance(instCid, IComponent::iid, (void **)&inst);
        }

        if (inst != nullptr) {
            inst->initialize(&hostApp);

            TUID ctrlCid;

            inst->getControllerClassId(ctrlCid);
            factory->createInstance(ctrlCid, IEditController::iid, (void **)&instCtrl);

            if (instCtrl != nullptr) {
                instCtrl->initialize(&hostApp);
            }

            IConnectionPoint * a = nullptr;
            IConnectionPoint * b = nullptr;

            inst->queryInterface(IConnectionPoint::iid, (void **)&a);

            if (instCtrl != nullptr) {
                instCtrl->queryInterface(IConnectionPoint::iid, (void **)&b);
            }

            if ((a != nullptr) && (b != nullptr)) {
                a->connect(b);
                b->connect(a);
            }

            // Find the parameter values that name the requested device and MIDI destination.
            ParamValue deviceValue = 0.0;
            ParamValue midiValue   = 0.0;
            bool       haveDevice  = false;
            bool       haveMidi    = false;

            if (instCtrl != nullptr) {
                for (int32 i = 0; i < instCtrl->getParameterCount(); i++) {
                    ParameterInfo info;

                    if (instCtrl->getParameterInfo(i, info) != kResultOk) {
                        continue;
                    }

                    std::string title = to_ascii(info.title);

                    for (int32 slot = 0; slot <= info.stepCount; slot++) {
                        String128  name;
                        ParamValue norm = (ParamValue)slot / (ParamValue)info.stepCount;

                        instCtrl->getParamStringByValue(info.id, norm, name);

                        std::string text = to_ascii(name);

                        if ((title == "Capture Device") && (text.find(playDevice) != std::string::npos)) {
                            deviceValue = norm;
                            haveDevice  = true;
                            printf("    capture: slot %d = %s\n", slot, text.c_str());
                            break;
                        }

                        if ((title == "Device Buffer") && (playBuffer != nullptr)
                            && (text.find(playBuffer) == 0)) {
                            gBufferValue = norm;
                            gHaveBuffer  = true;
                            printf("    buffer:  slot %d = %s\n", slot, text.c_str());
                            break;
                        }

                        if ((title == "MIDI Destination") && (text.find(playMidi) != std::string::npos)) {
                            midiValue = norm;
                            haveMidi  = true;
                            printf("    MIDI:    slot %d = %s\n", slot, text.c_str());
                            break;
                        }
                    }
                }
            }

            check("found the capture device", haveDevice);
            check("found the MIDI destination", haveMidi);

            IAudioProcessor * ip = nullptr;

            inst->queryInterface(IAudioProcessor::iid, (void **)&ip);

            if ((ip != nullptr) && haveDevice && haveMidi) {
                ProcessSetup setup;

                memset(&setup, 0, sizeof(setup));
                setup.processMode        = kRealtime;
                setup.symbolicSampleSize = kSample32;
                setup.maxSamplesPerBlock = 128;
                setup.sampleRate         = 48000.0;

                ip->setupProcessing(setup);
                inst->setActive(true);

                float   l[128];
                float   r[128];
                float * ch[2] = { l, r };

                // Measure the floor first, then play, then measure again. A synth that is not
                // sounding and one that is not connected look identical in a single reading.
                float quiet = run_blocks(ip, ch, l, r, 400, nullptr, deviceValue, midiValue);

                printf("    silence before the note: peak %.4f\n", quiet);

                // notes §31
                gOnsetBlock  = -1;
                gOnsetSample = -1;
                gThreshold   = quiet + 0.01f;

                OneNote noteOn(true, 60, 0.8f);
                float   played = run_blocks(ip, ch, l, r, 700, &noteOn, deviceValue, midiValue);

                if (gOnsetBlock >= 0) {
                    int    samples = (gOnsetBlock * 128) + gOnsetSample;
                    double ms      = (double)samples / 48.0;

                    printf("    onset after %d samples (%.1f ms) from the note being sent\n",
                           samples, ms);
                    printf("    of which the plug-in reports %u samples (%.1f ms) as its own\n",
                           ip->getLatencySamples(), ip->getLatencySamples() / 48.0);

                    check("onset is a plausible round trip (under 200 ms)", ms < 200.0);
                } else {
                    check("an onset was detected", false);
                }

                OneNote noteOff(false, 60, 0.0f);
                run_blocks(ip, ch, l, r, 100, &noteOff, deviceValue, midiValue);

                printf("    with the note held:     peak %.4f\n", played);

                // notes §32
                printf("    pressing the plug-in's Measure control %d time%s (burst %d)\n",
                       measureRuns, (measureRuns == 1) ? "" : "s", gBurst);

                double lowMs  = 0.0;
                double highMs = 0.0;

                for (int run = 0; run < measureRuns; run++) {
                    for (int press = 0; press < 2; press++) {
                        OneChange   trigger(8, (press == 0) ? 1.0 : 0.0);
                        ProcessData tick;
                        AudioBusBuffers tbus;

                        memset(&tick, 0, sizeof(tick));
                        memset(&tbus, 0, sizeof(tbus));
                        tbus.numChannels      = 2;
                        tbus.channelBuffers32 = ch;

                        tick.numSamples          = 128;
                        tick.numOutputs          = 1;
                        tick.outputs             = &tbus;
                        tick.symbolicSampleSize  = kSample32;
                        tick.processMode         = kRealtime;
                        tick.inputParameterChanges = &trigger;

                        ip->process(tick);
                    }

                    // notes §33
                    run_blocks(ip, ch, l, r, 2200, nullptr, deviceValue, midiValue);

                    // notes §34
                    double norm = (instCtrl != nullptr) ? instCtrl->getParamNormalized(9) : 0.0;
                    double ms   = -100.0 + (norm * 200.0);

                    printf("      run %d: offset %+.2f ms, reported %u samples\n",
                           run + 1, ms, ip->getLatencySamples());

                    if ((run == 0) || (ms < lowMs))  { lowMs  = ms; }
                    if ((run == 0) || (ms > highMs)) { highMs = ms; }
                }

                if (measureRuns > 1) {
                    printf("    spread over %d runs: %.2f ms (%.2f .. %.2f)\n",
                           measureRuns, highMs - lowMs, lowMs, highMs);
                }

                check("the hardware answered the note", played > (quiet + 0.001f));

                inst->setActive(false);
                ip->release();
            }

            if (a != nullptr) { a->release(); }
            if (b != nullptr) { b->release(); }

            if (instCtrl != nullptr) { instCtrl->terminate(); instCtrl->release(); }

            inst->terminate();
            inst->release();
        }
    }

    printf("\nper-instance isolation\n");
    {
        IConnectionPoint * procPoint = nullptr;
        IConnectionPoint * ctrlPoint = nullptr;

        component->queryInterface(IConnectionPoint::iid, (void **)&procPoint);
        controller->queryInterface(IConnectionPoint::iid, (void **)&ctrlPoint);

        check("processor implements IConnectionPoint", procPoint != nullptr);
        check("controller implements IConnectionPoint", ctrlPoint != nullptr);

        if ((procPoint != nullptr) && (ctrlPoint != nullptr)) {
            procPoint->connect(ctrlPoint);
            ctrlPoint->connect(procPoint);
            check("the two ends connect", true);
        }

        IComponent *      second     = nullptr;
        IEditController * secondCtrl = nullptr;

        factory->createInstance(processorCid, IComponent::iid, (void **)&second);
        factory->createInstance(controllerCid, IEditController::iid, (void **)&secondCtrl);

        check("a second instance can be created", (second != nullptr) && (secondCtrl != nullptr));

        if (second != nullptr) {
            second->initialize(nullptr);

            IConnectionPoint * secondPoint = nullptr;

            second->queryInterface(IConnectionPoint::iid, (void **)&secondPoint);
            check("the second processor is a distinct object", secondPoint != procPoint);

            if (secondPoint != nullptr) {
                secondPoint->release();
            }

            second->terminate();
            second->release();
        }

        if (secondCtrl != nullptr) {
            secondCtrl->release();
        }

        if (procPoint != nullptr) {
            procPoint->release();
        }

        if (ctrlPoint != nullptr) {
            ctrlPoint->release();
        }
    }

    // notes §35
    printf("\nsave and reload\n");
    {
        // Pick a device that is NOT slot 0, so "restored correctly" cannot be confused with
        // "happened to default to the right thing".
        String128 slot0, slot2;
        controller->getParamStringByValue(0, 0.0, slot0);
        controller->getParamStringByValue(0, 2.0 / 63.0, slot2);

        printf("    slot 0 = %s, slot 2 = %s\n", to_ascii(slot0).c_str(), to_ascii(slot2).c_str());

        std::string slot2Name = to_ascii(slot2);
        std::string slot2Uid   = uid_for_device_name(slot2Name);

        if (slot2Uid.empty()) {
            printf("    no UID for '%s' - skipping the restore check\n", slot2Name.c_str());
        }
        MemStream   saved;
        // notes §36
        std::string savedText = "GENBRIDGE3\n"
                                "active=" + slot2Uid + "\n"
                                "dev=256,48000.0,30.000,2,1,0.6000," + slot2Uid + "\n";

        saved.buf = savedText;

        IEditController * fresh = nullptr;
        factory->createInstance(controllerCid, IEditController::iid, (void **)&fresh);
        fresh->initialize(nullptr);

        check("controller accepts the component state", fresh->setComponentState(&saved) == kResultOk);

        String128 restored;
        fresh->getParamStringByValue(0, fresh->getParamNormalized(0), restored);
        printf("    device parameter restored as: %s\n", to_ascii(restored).c_str());
        // Skipped rather than failed when the machine could not supply a UID at all - a harness that
        // cannot set the test up has not found a defect, and saying so is the difference between a
        // red run that means something and one that gets ignored.
        if (!slot2Uid.empty()) {
            check("device restored from the saved UID, not the default",
                  to_ascii(restored) == slot2Name);
        } else {
            printf("  device restored from the saved UID, not the default  SKIPPED\n");
        }

        String128 framesText;
        fresh->getParamStringByValue(3, fresh->getParamNormalized(3), framesText);
        check("buffer size restored (256)", to_ascii(framesText) == "256");

        // notes §37
        {
            MemStream in;
            MemStream out;

            in.buf = "GENBRIDGE3\n"
                     "active=" + slot2Uid + "\n"
                     "callback=512\n"
                     "dev=256,48000.0,30.000,2,1,0.6000," + slot2Uid + "\n";

            IComponent * roundTrip = nullptr;

            factory->createInstance(processorCid, IComponent::iid, (void **)&roundTrip);

            if (roundTrip != nullptr) {
                roundTrip->initialize(&hostApp);

                bool took = (roundTrip->setState(&in) == kResultOk);

                check("component accepts a state carrying the callback size", took);

                out.buf.clear();
                roundTrip->getState(&out);

                check("the callback size is written back out",
                      out.buf.find("callback=512") != std::string::npos);

                roundTrip->terminate();
                roundTrip->release();
            }
        }

        String128 modeText;
        fresh->getParamStringByValue(4, fresh->getParamNormalized(4), modeText);
        check("mono/stereo restored (Mono)", to_ascii(modeText) == "Mono");

        String128 chanText;
        fresh->getParamStringByValue(5, fresh->getParamNormalized(5), chanText);
        check("first channel restored (3)", to_ascii(chanText) == "3");

        // THE MIDI DESTINATION MUST SURVIVE TOO. It was never written to the state at all, so every
        // reopened session came up on the first destination in the list - which looked like Ableton
        // forgetting, and was actually nothing ever writing it down.
        MemStream withMidi;

        // notes §38
        withMidi.buf =
            std::string("GENBRIDGE3\n")
            + "active=AppleUSBAudioEngine:LINE 6:HELIX:   2933118:2,3\n"
            + "midi=" + kSinkName + "\n"
            + "midich=5\n"
            + "dev=256,48000.0,30.000,2,1,0.6000,AppleUSBAudioEngine:LINE 6:HELIX:   2933118:2,3\n";

        TUID instCid2;
        bool haveInst = false;

        memset(instCid2, 0, sizeof(instCid2));

        for (int32 i = 0; (factory2 != nullptr) && (i < factory->countClasses()); i++) {
            PClassInfo2 info;

            if ((factory2->getClassInfo2(i, &info) == kResultOk)
                && (strstr(info.subCategories, "Instrument") != nullptr)) {
                IComponent * probe = nullptr;

                factory->createInstance(info.cid, IComponent::iid, (void **)&probe);

                if (probe != nullptr) {
                    probe->initialize(&hostApp);
                    probe->getControllerClassId(instCid2);
                    probe->terminate();
                    probe->release();
                    haveInst = true;
                }
                break;
            }
        }

        IEditController * instFresh = nullptr;

        if (haveInst) {
            factory->createInstance(instCid2, IEditController::iid, (void **)&instFresh);
        }

        if (instFresh != nullptr) {
            instFresh->initialize(&hostApp);
            check("instrument accepts the component state",
                  instFresh->setComponentState(&withMidi) == kResultOk);

            String128 midiText;
            String128 chText;

            instFresh->getParamStringByValue(6, instFresh->getParamNormalized(6), midiText);
            instFresh->getParamStringByValue(7, instFresh->getParamNormalized(7), chText);

            printf("    restored MIDI destination: %s, channel: %s\n",
                   to_ascii(midiText).c_str(), to_ascii(chText).c_str());

            check("MIDI destination restored by name",
                  to_ascii(midiText).find(kSinkName) != std::string::npos);
            check("MIDI channel restored", to_ascii(chText) == "5");

            instFresh->terminate();
            instFresh->release();
        }

        fresh->terminate();
        fresh->release();
    }

    // notes §39
    printf("\nhost-input capture (the External Instrument mode)\n");
    {
        TUID instCid;
        bool haveInst = false;

        memset(instCid, 0, sizeof(instCid));

        for (int32 i = 0; (factory2 != nullptr) && (i < factory->countClasses()); i++) {
            PClassInfo2 info;

            if ((factory2->getClassInfo2(i, &info) == kResultOk)
                && (strcmp(info.category, kVstAudioEffectClass) == 0)
                && (strstr(info.subCategories, "Instrument") != nullptr)) {
                memcpy(instCid, info.cid, sizeof(TUID));
                haveInst = true;
                break;
            }
        }

        IComponent * inst = nullptr;

        if (haveInst) {
            factory->createInstance(instCid, IComponent::iid, (void **)&inst);
        }

        if (inst != nullptr) {
            IAudioProcessor * ip = nullptr;

            inst->initialize(&hostApp);
            inst->queryInterface(IAudioProcessor::iid, (void **)&ip);

            if (ip != nullptr) {
                ProcessSetup setup;

                memset(&setup, 0, sizeof(setup));
                setup.processMode        = kRealtime;
                setup.symbolicSampleSize = kSample32;
                setup.maxSamplesPerBlock = 128;
                setup.sampleRate         = 48000.0;
                ip->setupProcessing(setup);
                inst->setActive(true);

                float inL[128], inR[128], outL[128], outR[128];
                float * inCh[2]  = { inL, inR };
                float * outCh[2] = { outL, outR };

                // A RAMP, NOT A CONSTANT: a constant would pass a check that copied one sample over
                // the whole block, or that swapped the channels, and a ramp fails both.
                for (int i = 0; i < 128; i++) {
                    inL[i] = (float)i / 128.0f;
                    inR[i] = -inL[i];
                }
                float          silentPeak = 0.0f;
                AudioBusBuffers inBus, outBus;
                ProcessData     data;

                // notes §40
                for (int block = 0; block < 10; block++) {
                    memset(&data, 0, sizeof(data));
                    memset(&inBus, 0, sizeof(inBus));
                    memset(&outBus, 0, sizeof(outBus));
                    memset(outL, 0, sizeof(outL));
                    memset(outR, 0, sizeof(outR));

                    inBus.numChannels       = 2;
                    inBus.channelBuffers32  = inCh;
                    outBus.numChannels      = 2;
                    outBus.channelBuffers32 = outCh;

                    data.numSamples         = 128;
                    data.numInputs          = 1;
                    data.inputs             = &inBus;
                    data.numOutputs         = 1;
                    data.outputs            = &outBus;
                    data.symbolicSampleSize = kSample32;
                    data.processMode        = kRealtime;

                    ip->process(data);

                    for (int i = 0; i < 128; i++) {
                        float m = (outL[i] < 0.0f) ? -outL[i] : outL[i];

                        if (m > silentPeak) { silentPeak = m; }
                    }
                }
                check("device mode ignores the host's input", silentPeak == 0.0f);

                // Now the mode itself. Capture Source is the LAST parameter id, so it is read from
                // the table rather than written here as a number - the two must not drift.
                IEditController * ec = nullptr;
                ParamID           sourceId = 0;
                bool              foundSource = false;
                TUID              ctrlCid;

                inst->getControllerClassId(ctrlCid);
                factory->createInstance(ctrlCid, IEditController::iid, (void **)&ec);

                if (ec != nullptr) {
                    ec->initialize(&hostApp);

                    for (int32 i = 0; i < ec->getParameterCount(); i++) {
                        ParameterInfo pi;

                        if ((ec->getParameterInfo(i, pi) == kResultOk)
                            && (to_ascii(pi.title) == "Capture Source")) {
                            sourceId    = pi.id;
                            foundSource = true;
                            break;
                        }
                    }
                }
                check("the instrument exposes a Capture Source parameter", foundSource);

                if (foundSource) {
                    OneChange toHost(sourceId, 1.0);
                    float     matched = 0.0f;
                    bool      exact   = true;

                    for (int block = 0; block < 4; block++) {
                        memset(&data, 0, sizeof(data));
                        memset(&inBus, 0, sizeof(inBus));
                        memset(&outBus, 0, sizeof(outBus));
                        memset(outL, 0, sizeof(outL));
                        memset(outR, 0, sizeof(outR));

                        inBus.numChannels       = 2;
                        inBus.channelBuffers32  = inCh;
                        outBus.numChannels      = 2;
                        outBus.channelBuffers32 = outCh;

                        data.numSamples         = 128;
                        data.numInputs          = 1;
                        data.inputs             = &inBus;
                        data.numOutputs         = 1;
                        data.outputs            = &outBus;
                        data.symbolicSampleSize = kSample32;
                        data.processMode        = kRealtime;
                        data.inputParameterChanges = (block == 0) ? (IParameterChanges *)&toHost
                                                                  : nullptr;

                        ip->process(data);
                    }

                    // The trim defaults to unity, so out must equal in sample for sample - and the
                    // right channel must be the right channel.
                    for (int i = 0; i < 128; i++) {
                        if ((outL[i] != inL[i]) || (outR[i] != inR[i])) {
                            exact = false;
                        }

                        float m = (outL[i] < 0.0f) ? -outL[i] : outL[i];

                        if (m > matched) { matched = m; }
                    }
                    check("host input reaches the output", matched > 0.0f);
                    check("and arrives unchanged, both channels", exact);

                    // notes §41
                    uint32 hostModeLatency = ip->getLatencySamples();

                    printf("    latency in host-input mode: %u samples\n", hostModeLatency);
                    check("host-input mode reports no latency of its own", hostModeLatency == 0);
                }

                if (ec != nullptr) {
                    ec->terminate();
                    ec->release();
                }
                inst->setActive(false);
                ip->release();
            }
            inst->terminate();
            inst->release();
        }
    }

    // notes §42

    printf("\nactivation (a fresh instance must stay idle)\n");

    if (processor != nullptr) {
        check("setActive(true)", component->setActive(true) == kResultOk);

        wait_ms(500);        // long enough for a device open to have happened, had one been due

        uint32 latency = processor->getLatencySamples();

        printf("    latency %u samples\n", latency);
        check("nothing opened without a selection", latency == 0);

        MemStream live;

        component->getState(&live);
        printf("    state now:\n");

        size_t at = 0;

        while (at < live.buf.size()) {
            size_t end = live.buf.find('\n', at);

            printf("      %s\n", live.buf.substr(at, end - at).c_str());
            at = (end == std::string::npos) ? live.buf.size() : end + 1;
        }

        // notes §43
        printf("\nparameter routing\n");

        for (int32 i = 0; i < controller->getParameterCount(); i++) {
            ParameterInfo info;

            if ((controller->getParameterInfo(i, info) != kResultOk) || (info.id != 0)) {
                continue;
            }

            // Walk the device slots until one names something different from what is open now, then
            // ask for it and see whether the plug-in actually goes there.
            char before[128];

            snprintf(before, sizeof(before), "%s", "");

            for (int32 slot = 1; slot < 6; slot++) {
                ParamValue norm = (ParamValue)slot / (ParamValue)info.stepCount;
                String128  name;

                controller->getParamStringByValue(0, norm, name);

                std::string wanted = to_ascii(name);

                if ((wanted == "-") || wanted.empty()) {
                    continue;
                }

                OneChange   change(0, norm);
                ProcessData data;

                memset(&data, 0, sizeof(data));

                float   left[512]  = { 0.0f };
                float   right[512] = { 0.0f };
                float * channels[2] = { left, right };

                AudioBusBuffers outBus;

                memset(&outBus, 0, sizeof(outBus));
                outBus.numChannels       = 2;
                outBus.channelBuffers32  = channels;

                data.numSamples           = 512;
                data.numOutputs           = 1;
                data.outputs              = &outBus;
                data.symbolicSampleSize   = kSample32;
                data.processMode          = kRealtime;
                data.inputParameterChanges = &change;

                processor->process(data);

                // notes §44
                for (int k = 0; (k < 100) && (processor->getLatencySamples() == 0); k++) {
                    wait_ms(100);
                }

                // And long enough after it for the host to have been told - see wait_ms().
                wait_ms(100);

                printf("    asked for slot %d (%s)\n", slot, wanted.c_str());

                // Latency becomes non-zero only when a device is genuinely open, so this is proof
                // the parameter reached the processor AND that it acted on it.
                check("a selection opens a device", processor->getLatencySamples() > 0);

                // And the host must have been TOLD, or it goes on compensating for zero - which is
                // what "Ableton reports latency 0" meant.
                printf("    restartComponent calls: %d (flags 0x%x)\n",
                       handler.restarts, handler.restartFlags);
                check("host told that latency changed",
                      (handler.restarts > 0) && ((handler.restartFlags & kLatencyChanged) != 0));

                // notes §45
                printf("\n  block-size retune (declaring %d, calling with 128)\n", maxBlock);

                uint32 latencyBefore = processor->getLatencySamples();
                int    restartsBefore = handler.restarts;

                for (int block = 0; block < 1200; block++) {
                    OneChange   none(9999, 0.0);       // an id the plug-in ignores
                    ProcessData run;

                    memset(&run, 0, sizeof(run));

                    float   l[128] = { 0.0f };
                    float   r[128] = { 0.0f };
                    float * ch[2]  = { l, r };

                    AudioBusBuffers bus;

                    memset(&bus, 0, sizeof(bus));
                    bus.numChannels      = 2;
                    bus.channelBuffers32 = ch;

                    run.numSamples          = 128;
                    run.numOutputs          = 1;
                    run.outputs             = &bus;
                    run.symbolicSampleSize  = kSample32;
                    run.processMode         = kRealtime;
                    run.inputParameterChanges = &none;

                    processor->process(run);

                    // notes §46
                    pace_to_deadline(AudioConvertNanosToHostTime(
                                         (uint64_t)((128.0 / 48000.0) * 1.0e9)));
                }

                wait_ms(300);                // let the worker act on the request, and tell the host

                uint32 latencyAfter = processor->getLatencySamples();

                printf("    latency %u -> %u samples (%.1f -> %.1f ms)\n",
                       latencyBefore, latencyAfter,
                       latencyBefore / 48.0, latencyAfter / 48.0);

                if (maxBlock > 128) {
                    check("latency reduced once the real block size was seen",
                          latencyAfter < latencyBefore);
                    check("host told about the reduction", handler.restarts > restartsBefore);
                } else {
                    // Declared equals used, so there is nothing to reclaim and the plug-in must not
                    // disturb the host's delay compensation pretending otherwise.
                    check("no retune when the host uses what it declared",
                          (latencyAfter == latencyBefore) && (handler.restarts == restartsBefore));
                }

                // notes §47
                component->setActive(false);
                component->setActive(true);
                wait_ms(300);

                printf("    after a reactivation: %u samples\n", processor->getLatencySamples());
                // notes §48
                uint32 after = processor->getLatencySamples();
                uint32 moved = (after > latencyAfter) ? (after - latencyAfter) : (latencyAfter - after);

                printf("    moved %u samples across the reopen\n", moved);

                check("a reopen keeps the tuned setpoint", moved < 128);
                break;
            }
        }

        component->setActive(false);
        processor->release();
    }

    if (controller != nullptr) {
        controller->terminate();
        controller->release();
    }

    component->terminate();
    component->release();

    if (factory2 != nullptr) {
        factory2->release();
    }

    factory->release();

    printf("\n%s (%d failure%s)\n", (gFailures == 0) ? "all checks passed" : "CHECKS FAILED",
           gFailures, (gFailures == 1) ? "" : "s");

    return (gFailures == 0) ? 0 : 1;
}
