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

// ── Headless conformance and state checks for the plug-in ───────────────────────────────────────
//
// The companion to tools/vst3host, and deliberately not the same program: vst3host opens a window
// and shows the editor, which is the only way to check that drawing works and the only thing that
// needs a run loop. This one has no window and answers different questions - does the factory look
// the way a host expects, does the saved state survive a round trip, does activation actually open
// a device - so it can run anywhere and be diffed.
//
// It exists in the repository, rather than in a scratchpad, because the scratchpad version of it
// was written twice in a single afternoon and thrown away twice. vst3host.mm carries the same note
// for the same reason.
//
// WHAT IT PROVES, AND WHAT IT DOES NOT. Passing every check here does not mean a DAW will accept
// the plug-in. An earlier version of vst3host asked only for IPluginFactory and so never noticed
// that IPluginFactory2 was missing - which is precisely what Ableton rejected G2-Edit for.
// ~/Library/Preferences/Ableton/Live */Log.txt remains the last word on a rejection.
//
// Build: ./do-vst3host    (it builds this too)

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <dlfcn.h>
#include <unistd.h>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
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

// ---- a minimal host, enough to exercise the processor/controller connection ----
//
// Messages between the two ends are created BY THE HOST - IHostApplication::createInstance is the
// only way to get an IMessage - so a checker that passes a null context can never see them. That is
// why the latency notification could not be tested here before, and why it reached a DAW broken.

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

// One note, delivered the way a host delivers them.
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

        data.inputParameterChanges = (block == 0) ? (IParameterChanges *)&deviceChange
                                                  : (IParameterChanges *)&midiChange;

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

        usleep(2666);
    }

    return peak;
}

int main(int argc, char ** argv) {
    if (argc < 2) {
        printf("usage: vst3check <plugin.vst3> [--block N]\n");
        printf("  --block N   max samples per block to declare, as a host would (default 512)\n");
        return 2;
    }

    int32       maxBlock   = 512;
    const char * playDevice = nullptr;
    const char * playMidi   = nullptr;

    for (int i = 2; i < argc; i++) {
        if ((strcmp(argv[i], "--block") == 0) && ((i + 1) < argc)) {
            maxBlock = atoi(argv[i + 1]);
        } else if ((strcmp(argv[i], "--play") == 0) && ((i + 2) < argc)) {
            playDevice = argv[i + 1];
            playMidi   = argv[i + 2];
        }
    }

    char path[1024];

    snprintf(path, sizeof(path), "%s/Contents/MacOS/%s", argv[1], "GenBridge");

    void * lib = dlopen(path, RTLD_NOW);

    if (lib == nullptr) {
        printf("dlopen failed: %s\n", dlerror());
        return 2;
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

        // BOTH processors register under kVstAudioEffectClass - that is the VST3 category for
        // anything that makes audio, and the SUBcategory is what separates an effect from an
        // instrument. Matching on the category alone picked whichever came last, so every check
        // below silently ran against the instrument.
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

        // THE CONTROLLER'S state, not the component's: the editor size lives there, and both halves
        // of it were stubs that returned kResultOk without touching the stream, so every session
        // opened at the default size however the last one was left.
        {
            MemStream guiSaved;
            MemStream guiLoaded;

            controller->getState(&guiSaved);
            check("controller writes a GUI state", guiSaved.buf.find("editor=") != std::string::npos);

            guiLoaded.buf = "GENBRIDGEGUI1\neditor=700,748\nfuturekey=a newer build wrote this\n";
            check("controller loads a GUI state", controller->setState(&guiLoaded) == kResultOk);

            MemStream guiBack;

            controller->getState(&guiBack);
            check("editor size survives the round trip", guiBack.buf.find("editor=700,748") != std::string::npos);

            // Straight out of a project file, so it is checked rather than trusted: a size the user
            // cannot see or cannot fit on screen has no way back short of editing the project.
            MemStream absurd;

            absurd.buf = "GENBRIDGEGUI1\neditor=40,4\n";
            controller->setState(&absurd);

            MemStream afterAbsurd;

            controller->getState(&afterAbsurd);
            check("an out-of-range saved size is refused",
                  afterAbsurd.buf.find("editor=700,748") != std::string::npos);
        }

        // THE MICROPHONE BUG. A project naming a device that is not plugged in must open NOTHING.
        // The host restores its saved DEVICE PARAMETER too, and that is a slot INDEX recorded when
        // the device list had a different shape - so slot 0 arrives here exactly as a host delivers
        // it, and slot 0 on a Mac is generally a built-in or Continuity microphone.
        //
        // ASSERTED ON THE PLUG-IN'S OWN LOG, which is not where a check would rather look. The
        // obvious signals do not work: getLatencySamples() only moves when a device actually OPENS,
        // and on a developer machine every device is already claimed by something, so it reads zero
        // whether the plug-in tried the microphone or refused to. active= only changes on a
        // successful open, for the same reason. The log is the one place the plug-in records the
        // decision rather than the outcome, and the decision is what is on trial.
        //
        // ONE parameter value, repeated. A host restoring a project sends one, and it is precisely
        // the FIRST one after a state restore that must not be believed; later changes are the user
        // choosing, and are meant to be honoured.
        {
            bool hadGate = (access("/tmp/genbridge-log", F_OK) == 0);

            if (!hadGate) {
                FILE * gate = fopen("/tmp/genbridge-log", "w");

                if (gate != nullptr) {
                    fclose(gate);
                }
            }
            FILE * truncate = fopen("/tmp/genbridge.log", "w");

            if (truncate != nullptr) {
                fclose(truncate);
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

                while (fgets(line, (int)sizeof(line), readback) != nullptr) {
                    log += line;
                }
                fclose(readback);
            }

            check("an absent saved device is waited for",
                  log.find("not present - waiting for it") != std::string::npos);

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

            if (!hadGate) {
                unlink("/tmp/genbridge-log");    // left exactly as it was found
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

    // ---- two instances must not share status ----
    //
    // The status figures used to live in one process-wide struct, so with two plug-ins in a set the
    // editors read whichever processor wrote last: a panel showing a microphone cheerfully reported
    // that it was capturing a Kronos. Each processor now claims its own slot and tells its own
    // controller which one, over IConnectionPoint.
    // ---- the instrument variant ----
    //
    // Same code, registered a second time with its own identity, category and bus layout. An
    // instrument has NO audio input, which a host only tolerates because IPluginFactory2 declares
    // the subcategory - with the bare "Audio Module Class" a host assumes effect, looks for the
    // input an effect must have, and refuses to load. That is the trap G2-Edit fell into.
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

            check("no audio input bus", inst->getBusCount(kAudio, kInput) == 0);
            check("one audio output bus", inst->getBusCount(kAudio, kOutput) == 1);
            check("one event input bus", inst->getBusCount(kEvent, kInput) == 1);

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

    // ---- the round trip, on real hardware ----
    //
    // The instrument's entire claim is that the DAW can play the hardware and record it back. That
    // is testable without a person listening: send a note, capture the device, and see whether
    // anything arrives. Opt-in, because it makes a real synth make a real noise.
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

                // MEASURE, not just detect. The number wanted is how long after the note was sent
                // the audio actually arrived - USB MIDI transit, the synth's own response, the
                // capture path and our ring, end to end.
                //
                // It necessarily includes the PATCH'S ATTACK: a slow pad crosses the threshold later
                // than a piano does, and no measurement can separate the two from outside. So this
                // is "time to audible onset" and it wants a percussive sound to mean anything.
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

                // Drive the plug-in's OWN Measure control, so its detection is exercised rather
                // than the harness's - they are different code and only one of them ships.
                printf("    pressing the plug-in's Measure control\n");

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

                run_blocks(ip, ch, l, r, 900, nullptr, deviceValue, midiValue);
                usleep(300000);

                printf("    plug-in now reports %u samples\n", ip->getLatencySamples());

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

    // ---- the save/reload cycle ----
    //
    // A host saves the component's state and, on reload, hands the SAME bytes to the controller via
    // setComponentState so the panel can agree with the processor. A controller that ignores it
    // comes up on defaults - which is how two tracks saved with different devices both reopened
    // showing the first device in the list.
    printf("\nsave and reload\n");
    {
        // Pick a device that is NOT slot 0, so "restored correctly" cannot be confused with
        // "happened to default to the right thing".
        String128 slot0, slot2;
        controller->getParamStringByValue(0, 0.0, slot0);
        controller->getParamStringByValue(0, 2.0 / 63.0, slot2);

        printf("    slot 0 = %s, slot 2 = %s\n", to_ascii(slot0).c_str(), to_ascii(slot2).c_str());

        MemStream saved;
        saved.buf =
            "GENBRIDGE3\n"
            "active=AppleUSBAudioEngine:LINE 6:HELIX:   2933118:2,3\n"
            "dev=256,48000.0,30.000,2,1,0.6000,AppleUSBAudioEngine:LINE 6:HELIX:   2933118:2,3\n";

        IEditController * fresh = nullptr;
        factory->createInstance(controllerCid, IEditController::iid, (void **)&fresh);
        fresh->initialize(nullptr);

        check("controller accepts the component state", fresh->setComponentState(&saved) == kResultOk);

        String128 restored;
        fresh->getParamStringByValue(0, fresh->getParamNormalized(0), restored);
        printf("    device parameter restored as: %s\n", to_ascii(restored).c_str());
        check("device restored from the saved UID, not the default",
              to_ascii(restored).find("HELIX") != std::string::npos);

        String128 framesText;
        fresh->getParamStringByValue(3, fresh->getParamNormalized(3), framesText);
        check("buffer size restored (256)", to_ascii(framesText) == "256");

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

        withMidi.buf =
            "GENBRIDGE3\n"
            "active=AppleUSBAudioEngine:LINE 6:HELIX:   2933118:2,3\n"
            "midi=KRONOS SOUND\n"
            "midich=5\n"
            "dev=256,48000.0,30.000,2,1,0.6000,AppleUSBAudioEngine:LINE 6:HELIX:   2933118:2,3\n";

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
                  to_ascii(midiText).find("KRONOS") != std::string::npos);
            check("MIDI channel restored", to_ascii(chText) == "5");

            instFresh->terminate();
            instFresh->release();
        }

        fresh->terminate();
        fresh->release();
    }

    // ---- activation ----
    //
    // A FRESH INSTANCE MUST OPEN NOTHING. Earlier versions grabbed a device on activation so as not
    // to look broken, and on a machine whose first input is an iPhone Continuity microphone that
    // meant waking it once per instance, on the host's main thread, during load. Ableton stopped
    // starting. Latency stays at zero until something is actually chosen.

    printf("\nactivation (a fresh instance must stay idle)\n");

    if (processor != nullptr) {
        check("setActive(true)", component->setActive(true) == kResultOk);

        usleep(500000);      // long enough for a device open to have happened, had one been due

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

        // ---- drive a real parameter change, the way a host does ----
        //
        // This is the path that failed in Ableton: the editor asks the host to change a parameter,
        // the host delivers it inside process(), and the processor acts on it. None of the earlier
        // checks touched it, which is precisely why the bug reached a DAW.
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

                for (int k = 0; (k < 30); k++) {
                    usleep(100000);
                }

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

                // ---- the block-size retune ----
                //
                // maxSamplesPerBlock was declared as whatever --block said, but a host may call
                // with far less; Ableton declares 512 and uses 128. The plug-in watches for a
                // couple of seconds and shrinks the ring to suit.
                //
                // PACED AT REALTIME, deliberately. Driving process() as fast as the loop can go
                // would consume the ring far quicker than a real device fills it, the plug-in would
                // underrun, and the safety net would correctly undo the very thing being tested.
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
                    usleep(2666);            // 128 frames at 48 kHz
                }

                usleep(300000);              // let the worker act on the request

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

                // A REOPEN MUST NOT UNDO THE LESSON. Telling the host about a latency change makes
                // it reactivate the plug-in, which reopens the device; if that reset the
                // observation the two would chase each other for ever, tearing the device down
                // every couple of seconds. This is that loop, reproduced.
                component->setActive(false);
                component->setActive(true);
                usleep(300000);

                printf("    after a reactivation: %u samples\n", processor->getLatencySamples());
                check("a reopen keeps the tuned setpoint",
                      processor->getLatencySamples() == latencyAfter);
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
