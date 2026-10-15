/* se_gmpi_state_probe -- which of a SynthEdit-exported plug-in's parameters
 * survive a VST3 save, measured in a bare host with no DAW and no window.
 *
 * Stage 0 of TideSynth/docs/state-management-consolidation-plan.md. The claim
 * under test, made by reading the code: an SE export on the GMPI VST3 wrapper
 * saves only the DAW-facing floats that plugin.gmpi.xml declares, and loses
 * every other parameter in parameters.se.xml (text, private, MIDI learn). The
 * production se_vst3 wrapper saves them all. This probe runs the same bundle
 * layout through either wrapper and reports, parameter by parameter, what the
 * saved chunk contains -- so the two can be quoted side by side.
 *
 *   se_gmpi_state_probe <bundle.vst3 | module> [options]
 *
 *   --restore <preset.xml>   feed this chunk to setComponentState and setState
 *                            before saving (int32-length-prefixed, as both
 *                            wrappers' readSizedChunk expects). Without it the
 *                            plug-in saves its bundled defaults.
 *   --save <out.xml>         write the saved chunk (prefix stripped) here.
 *   --process <N>            run N blocks of audio between restore and save
 *                            (default 8; 0 skips activation entirely). The
 *                            output's fundamental is estimated from rising
 *                            zero crossings over the last half of the blocks
 *                            (MEASURED audio Hz): with a fixture whose pitch
 *                            is not a DAW parameter, that frequency is the
 *                            one number that proves a restored preset reached
 *                            the ENGINE and not just a store.
 *   --no-controller          never create the edit controller. CONTROL: on the
 *                            GMPI wrapper getState then falls back to the
 *                            processor's own store (Processor_VST3::getState).
 *   --resources <dir>        the Resources folder holding parameters.se.xml,
 *                            when it is not <bundle>/Contents/Resources.
 *
 * Output: PASS/FAIL lines for the probe's own mechanics, MEASURED audio Hz (the output's
 * fundamental from zero crossings, so a restore can be seen reaching the ENGINE when the
 * fixture's pitch is not a DAW parameter), then the MEASURED lines of the shared reader,
 * se_gmpi_state_analysis.h, which the CLAP twin (se_gmpi_clap_state_probe.cpp) also prints.
 * Exit code is 0 unless a mechanics check failed; the measurement is a report.
 *
 * Build (headers only; nothing from the VST3 SDK is compiled):
 *   linux:  g++ -std=c++17 -O1 -I <VST3_SDK> se_gmpi_state_probe.cpp -o se_gmpi_state_probe -ldl
 *   win32:  cl /std:c++17 /EHsc /I <VST3_SDK> se_gmpi_state_probe.cpp /link ole32.lib
 *
 * Shape and host stubs follow TideSynth/tests/e80_vst3_feedback_probe.cpp. */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
  #include <windows.h>
#elif defined(__linux__)
  #include <dlfcn.h>
  #include <sys/stat.h>
#endif

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "se_gmpi_state_analysis.h"

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace se_state_analysis;

static int failures = 0;

static void check(const char* what, bool ok)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

/* Host objects live in main()'s frame and outlive the plug-in; refcounts are no-ops. */
#define PROBE_REFCOUNT                                          \
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1; }      \
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1; }

static bool iidIs(const TUID a, const TUID b) { return FUnknownPrivate::iidEqual(a, b); }

/* ---- IBStream over memory ------------------------------------------------ */

class MemStream : public IBStream
{
public:
    std::string buf;
    int64 pos = 0;

    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE
    {
        if (iidIs(_iid, IBStream_iid) || iidIs(_iid, FUnknown_iid)) { *obj = static_cast<IBStream*>(this); return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    PROBE_REFCOUNT

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) SMTG_OVERRIDE
    {
        const int64 left = (int64)buf.size() - pos;
        int64 n = numBytes < left ? (int64)numBytes : left;
        if (n < 0) n = 0;
        memcpy(buffer, buf.data() + pos, (size_t)n);
        pos += n;
        if (numBytesRead) *numBytesRead = (int32)n;
        return kResultOk;
    }
    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) SMTG_OVERRIDE
    {
        if (pos != (int64)buf.size()) buf.resize((size_t)pos);
        buf.append((const char*)buffer, (size_t)numBytes);
        pos = (int64)buf.size();
        if (numBytesWritten) *numBytesWritten = numBytes;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 p, int32 mode, int64* result) SMTG_OVERRIDE
    {
        if (mode == kIBSeekSet) pos = p; else if (mode == kIBSeekCur) pos += p; else pos = (int64)buf.size() + p;
        if (pos < 0) pos = 0;
        if (pos > (int64)buf.size()) pos = (int64)buf.size();
        if (result) *result = pos;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* p) SMTG_OVERRIDE { if (p) *p = pos; return kResultOk; }
};

/* ---- IHostApplication: the GMPI wrapper's connect() allocates an IMessage -- */

class AttrList : public IAttributeList
{
    std::map<std::string, int64> ints;
    std::map<std::string, std::string> bins;
public:
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE
    {
        if (iidIs(_iid, IAttributeList_iid) || iidIs(_iid, FUnknown_iid)) { *obj = static_cast<IAttributeList*>(this); return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    PROBE_REFCOUNT
    tresult PLUGIN_API setInt(AttrID id, int64 value) SMTG_OVERRIDE { ints[id] = value; return kResultOk; }
    tresult PLUGIN_API getInt(AttrID id, int64& value) SMTG_OVERRIDE
    {
        auto it = ints.find(id); if (it == ints.end()) return kResultFalse; value = it->second; return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID, double) SMTG_OVERRIDE { return kNotImplemented; }
    tresult PLUGIN_API getFloat(AttrID, double&) SMTG_OVERRIDE { return kNotImplemented; }
    tresult PLUGIN_API setString(AttrID, const TChar*) SMTG_OVERRIDE { return kNotImplemented; }
    tresult PLUGIN_API getString(AttrID, TChar*, uint32) SMTG_OVERRIDE { return kNotImplemented; }
    tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32 size) SMTG_OVERRIDE
    {
        bins[id].assign((const char*)data, size); return kResultOk;
    }
    tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32& size) SMTG_OVERRIDE
    {
        auto it = bins.find(id); if (it == bins.end()) return kResultFalse;
        data = it->second.data(); size = (uint32)it->second.size(); return kResultOk;
    }
};

class HostMessage : public IMessage
{
    std::string id;
    AttrList attrs;
    int32 rc = 1;
public:
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE
    {
        if (iidIs(_iid, IMessage_iid) || iidIs(_iid, FUnknown_iid)) { *obj = static_cast<IMessage*>(this); return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++rc; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { if (--rc == 0) { delete this; return 0; } return rc; }
    FIDString PLUGIN_API getMessageID() SMTG_OVERRIDE { return id.c_str(); }
    void PLUGIN_API setMessageID(FIDString mid) SMTG_OVERRIDE { id = mid ? mid : ""; }
    IAttributeList* PLUGIN_API getAttributes() SMTG_OVERRIDE { return &attrs; }
};

static int messagesAllocated = 0;

class HostApp : public IHostApplication
{
public:
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE
    {
        if (iidIs(_iid, IHostApplication_iid) || iidIs(_iid, FUnknown_iid)) { *obj = static_cast<IHostApplication*>(this); return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    PROBE_REFCOUNT
    tresult PLUGIN_API getName(String128 name) SMTG_OVERRIDE
    {
        static const char16_t n[] = u"se-gmpi-state-probe";
        memcpy(name, n, sizeof(n));
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID _iid, void** obj) SMTG_OVERRIDE
    {
        if (iidIs(cid, IMessage_iid) && iidIs(_iid, IMessage_iid)) { ++messagesAllocated; *obj = static_cast<IMessage*>(new HostMessage()); return kResultOk; }
        if (iidIs(cid, IAttributeList_iid) && iidIs(_iid, IAttributeList_iid)) { *obj = static_cast<IAttributeList*>(new AttrList()); return kResultOk; }
        *obj = nullptr;
        return kResultFalse;
    }
};

class ComponentHandler : public IComponentHandler
{
public:
    int edits = 0, restarts = 0;
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE
    {
        if (iidIs(_iid, IComponentHandler_iid) || iidIs(_iid, FUnknown_iid)) { *obj = static_cast<IComponentHandler*>(this); return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    PROBE_REFCOUNT
    tresult PLUGIN_API beginEdit(ParamID) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) SMTG_OVERRIDE { ++edits; return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) SMTG_OVERRIDE { ++restarts; return kResultOk; }
};

int main(int argc, char** argv)
{
    const char* bundle = nullptr;
    const char* restorePath = nullptr;
    const char* savePath = nullptr;
    const char* resourcesDir = nullptr;
    int blocks = 8;
    bool wantController = true;

    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--restore") && i + 1 < argc) restorePath = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) savePath = argv[++i];
        else if (!strcmp(argv[i], "--resources") && i + 1 < argc) resourcesDir = argv[++i];
        else if (!strcmp(argv[i], "--process") && i + 1 < argc) blocks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-controller")) wantController = false;
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        else bundle = argv[i];
    }
    if (!bundle)
    {
        fprintf(stderr, "usage: se_gmpi_state_probe <bundle.vst3|module> [--restore preset.xml] [--save out.xml] [--process N] [--no-controller] [--resources dir]\n");
        return 2;
    }

    printf("se_gmpi_state_probe: %s\n", bundle);

    /* ---- locate the module and the Resources folder ----------------------- */
    std::string dir(bundle);
    while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    std::string leaf = dir;
    {
        const size_t slash = leaf.find_last_of("/\\");
        if (slash != std::string::npos) leaf = leaf.substr(slash + 1);
        const size_t dot = leaf.find_last_of('.');
        if (dot != std::string::npos) leaf = leaf.substr(0, dot);
    }
    std::string modulePath(bundle);
    std::string resources = resourcesDir ? resourcesDir : dir + "/Contents/Resources";

    typedef IPluginFactory* (PLUGIN_API *GetFactoryProc)();
    GetFactoryProc getFactory = nullptr;

#if defined(_WIN32)
    {
        DWORD a = GetFileAttributesA(bundle);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
            modulePath = dir + "\\Contents\\x86_64-win\\" + leaf + ".vst3";
    }
    printf("      module -> %s\n", modulePath.c_str());
    HMODULE lib = LoadLibraryA(modulePath.c_str());
    check("the plug-in binary loads", lib != nullptr);
    if (!lib) { fprintf(stderr, "  LoadLibrary failed, GetLastError=%lu\n", GetLastError()); return 1; }
    if (auto initDll = (bool (*)())GetProcAddress(lib, "InitDll")) initDll();
    auto exitDll = (bool (*)())GetProcAddress(lib, "ExitDll");
    getFactory = (GetFactoryProc)GetProcAddress(lib, "GetPluginFactory");
#elif defined(__linux__)
    {
        struct stat st;
        if (stat(bundle, &st) == 0 && S_ISDIR(st.st_mode))
        {
#if defined(__x86_64__)
            const char* arch = "x86_64-linux";
#elif defined(__aarch64__)
            const char* arch = "aarch64-linux";
#else
            const char* arch = "unknown-linux";
#endif
            modulePath = dir + "/Contents/" + arch + "/" + leaf + ".so";
        }
    }
    printf("      module -> %s\n", modulePath.c_str());
    void* lib = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL);
    check("the plug-in binary loads", lib != nullptr);
    if (!lib) { fprintf(stderr, "  dlopen failed: %s\n", dlerror()); return 1; }
    auto moduleEntry = (bool (*)(void*))dlsym(lib, "ModuleEntry");
    auto moduleExit = (bool (*)())dlsym(lib, "ModuleExit");
    check("ModuleEntry and ModuleExit are exported", moduleEntry && moduleExit);
    if (moduleEntry) check("ModuleEntry(handle) returns true", moduleEntry(lib));
    getFactory = (GetFactoryProc)dlsym(lib, "GetPluginFactory");
#else
    fprintf(stderr, "this probe runs on linux and win32\n");
    return 2;
#endif

    check("GetPluginFactory is exported", getFactory != nullptr);
    if (!getFactory) return 1;
    IPluginFactory* factory = getFactory();
    check("GetPluginFactory returns a factory", factory != nullptr);
    if (!factory) return 1;

    TUID componentCid;
    memset(componentCid, 0, sizeof componentCid);
    bool haveCid = false;
    const int32 classCount = factory->countClasses();
    for (int32 i = 0; i < classCount && !haveCid; ++i)
    {
        PClassInfo ci;
        if (factory->getClassInfo(i, &ci) != kResultOk) continue;
        printf("      class [%d] %-24s %s\n", (int)i, ci.name, ci.category);
        if (!strcmp(ci.category, kVstAudioEffectClass)) { memcpy(componentCid, ci.cid, sizeof(TUID)); haveCid = true; }
    }
    check("an Audio Module Class is advertised", haveCid);
    if (!haveCid) return 1;

    HostApp hostApp;
    IComponent* component = nullptr;
    check("createInstance(IComponent) succeeds",
          factory->createInstance(componentCid, IComponent_iid, (void**)&component) == kResultOk && component);
    if (!component) return 1;
    check("component->initialize succeeds", component->initialize(static_cast<IHostApplication*>(&hostApp)) == kResultOk);

    IAudioProcessor* processor = nullptr;
    check("the component offers IAudioProcessor",
          component->queryInterface(IAudioProcessor_iid, (void**)&processor) == kResultOk && processor);
    if (!processor) return 1;

    /* ---- the restore chunk, int32-length-prefixed ------------------------- */
    MemStream stateStream;
    std::string restoreXml;
    if (restorePath)
    {
        check("the restore preset reads", slurp(restorePath, restoreXml) && !restoreXml.empty());
        const int32 chunkSize = (int32)restoreXml.size();
        stateStream.buf.assign((const char*)&chunkSize, sizeof chunkSize);
        stateStream.buf.append(restoreXml);
        printf("      restore chunk: %zu bytes from %s\n", restoreXml.size(), restorePath);
    }
    else
        printf("      no restore: the plug-in saves its bundled defaults\n");

    /* ---- controller, connection, restore ---------------------------------- */
    IEditController* controller = nullptr;
    IConnectionPoint* cpComponent = nullptr;
    IConnectionPoint* cpController = nullptr;
    ComponentHandler handler;
    if (wantController)
    {
        TUID controllerCid;
        memset(controllerCid, 0, sizeof controllerCid);
        check("the component names a controller class", component->getControllerClassId(controllerCid) == kResultOk);
        check("createInstance(IEditController) succeeds",
              factory->createInstance(controllerCid, IEditController_iid, (void**)&controller) == kResultOk && controller);
        if (controller)
        {
            check("controller->initialize succeeds", controller->initialize(static_cast<IHostApplication*>(&hostApp)) == kResultOk);
            controller->setComponentHandler(&handler);
            if (component->queryInterface(IConnectionPoint_iid, (void**)&cpComponent) != kResultOk) cpComponent = nullptr;
            if (controller->queryInterface(IConnectionPoint_iid, (void**)&cpController) != kResultOk) cpController = nullptr;
            check("both ends offer IConnectionPoint", cpComponent && cpController);
            if (cpComponent && cpController)
            {
                check("component->connect(controller)", cpComponent->connect(cpController) == kResultOk);
                check("controller->connect(component)", cpController->connect(cpComponent) == kResultOk);
            }
            if (restorePath)
            {
                stateStream.pos = 0;
                check("controller->setComponentState accepts the preset", controller->setComponentState(&stateStream) == kResultTrue);
            }
        }
    }
    else
        printf("      --no-controller: the processor saves from its own store\n");

    if (restorePath)
    {
        stateStream.pos = 0;
        check("component->setState accepts the preset", component->setState(&stateStream) == kResultTrue);
    }

    /* ---- optional audio, so the engine has been built and run ------------- */
    if (blocks > 0)
    {
        const int32 outBuses = component->getBusCount(kAudio, kOutput);
        for (int32 i = 0; i < outBuses; ++i) component->activateBus(kAudio, kOutput, i, true);
        ProcessSetup setup{};
        setup.processMode = kRealtime;
        setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = 256;
        setup.sampleRate = 48000.0;
        check("setupProcessing succeeds", processor->setupProcessing(setup) == kResultOk);
        check("setActive(true) succeeds", component->setActive(true) == kResultOk);
        processor->setProcessing(true);

        const int32 chans = 2;
        std::vector<std::vector<float>> store((size_t)(outBuses * chans), std::vector<float>(256, 0.0f));
        std::vector<std::vector<float*>> ptrs((size_t)outBuses, std::vector<float*>((size_t)chans, nullptr));
        std::vector<AudioBusBuffers> buses((size_t)(outBuses > 0 ? outBuses : 1));
        for (int32 b = 0; b < outBuses; ++b)
        {
            for (int32 c = 0; c < chans; ++c) ptrs[(size_t)b][(size_t)c] = store[(size_t)(b * chans + c)].data();
            buses[(size_t)b].numChannels = chans;
            buses[(size_t)b].silenceFlags = 0;
            buses[(size_t)b].channelBuffers32 = ptrs[(size_t)b].data();
        }
        double peak = 0.0;
        int processFailures = 0;
        FrequencyEstimate freq; /* over the second half of the blocks, channel 0 */
        for (int b = 0; b < blocks; ++b)
        {
            ProcessData data{};
            data.processMode = kRealtime;
            data.symbolicSampleSize = kSample32;
            data.numSamples = 256;
            data.numOutputs = outBuses;
            data.outputs = outBuses > 0 ? buses.data() : nullptr;
            if (processor->process(data) != kResultOk) ++processFailures;
            if (outBuses > 0)
            {
                for (int32 c = 0; c < chans; ++c)
                    for (int32 i = 0; i < 256; ++i) { const double a = fabs((double)ptrs[0][(size_t)c][i]); if (a > peak) peak = a; }
                if (b >= blocks / 2)
                    freq.feed(ptrs[0][0], 256);
            }
        }
        check("no block returned an error", processFailures == 0);
        printf("      %d blocks: audio out buses %d, peak %.4f (%.1f dBFS)\n", blocks, (int)outBuses, peak, peak > 0 ? 20.0 * log10(peak) : -1000.0);
        freq.print(48000.0);
        processor->setProcessing(false);
        component->setActive(false);
    }

    /* ---- save ------------------------------------------------------------- */
    std::string chunk;
    {
        MemStream out;
        check("component->getState succeeds", component->getState(&out) == kResultTrue);
        const char* body = out.buf.data();
        size_t bodyLen = out.buf.size();
        if (bodyLen >= sizeof(int32))
        {
            int32 chunkSize = 0;
            memcpy(&chunkSize, body, sizeof chunkSize);
            if (chunkSize >= 0 && (size_t)chunkSize + sizeof(int32) <= bodyLen) { body += sizeof(int32); bodyLen = (size_t)chunkSize; }
        }
        chunk.assign(body, bodyLen);
        if (controller)
        {
            MemStream cout_;
            controller->getState(&cout_);
            printf("      controller->getState wrote %zu bytes\n", cout_.buf.size());
        }
        if (savePath)
        {
            if (FILE* f = fopen(savePath, "wb")) { fwrite(chunk.data(), 1, chunk.size(), f); fclose(f); printf("      saved chunk to %s\n", savePath); }
            else check("the --save path is writable", false);
        }
    }

    /* ---- the measurement: the shared reader (se_gmpi_state_analysis.h) ----- */
    report(chunk, resources, restoreXml);
    printf("      host allocated %d IMessage(s); performEdit %d, restartComponent %d\n", messagesAllocated, handler.edits, handler.restarts);

    /* ---- teardown --------------------------------------------------------- */
    if (cpComponent && cpController) { cpComponent->disconnect(cpController); cpController->disconnect(cpComponent); }
    if (cpComponent) cpComponent->release();
    if (cpController) cpController->release();
    if (controller) { controller->terminate(); controller->release(); }
    processor->release();
    component->terminate();
    component->release();
#if defined(_WIN32)
    if (exitDll) exitDll();
    FreeLibrary(lib);
#elif defined(__linux__)
    if (moduleExit) moduleExit();
    dlclose(lib);
#endif

    printf("\n%s\n", failures ? "SOME PROBE CHECKS FAILED" : "all probe checks passed");
    return failures ? 1 : 0;
}
