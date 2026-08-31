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

int main(int argc, char ** argv) {
    if (argc < 2) {
        printf("usage: vst3check <plugin.vst3> [--block N]\n");
        printf("  --block N   max samples per block to declare, as a host would (default 512)\n");
        return 2;
    }

    int32 maxBlock = 512;

    for (int i = 2; (i + 1) < argc; i++) {
        if (strcmp(argv[i], "--block") == 0) {
            maxBlock = atoi(argv[i + 1]);
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
    check("registers two classes", factory->countClasses() == 2);

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

        if (strcmp(info.category, kVstAudioEffectClass) == 0) {
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
