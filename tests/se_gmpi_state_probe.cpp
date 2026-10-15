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
 *   --cc <n> <value>         on block --cc-block (default 4), deliver MIDI controller n
 *                            (channel 1) at normalized <value> as the VST3 parameter the
 *                            wrapper maps MIDI onto (Controller_VST3::getMidiControllerAssignment:
 *                            id 10000 + n). The wrapper turns it back into MIDI, the engine's
 *                            MIDI learn moves the parameter it is assigned to, and that change
 *                            must travel engine -> controller -> saved blob. With the fixture's
 *                            pitch learned to that CC, MEASURED audio Hz shows the engine
 *                            moved and the blob's pitch shows the controller heard about it.
 *   --editor                 (Linux) open the plug-in's editor in an X11 window on $DISPLAY
 *                            (an Xvfb will do), with this probe as its IPlugFrame and
 *                            IRunLoop, and pump it between blocks. MEASURED: whether the
 *                            editor received its controller pointer and built a panel
 *                            (SE_GMPI_TRACE lines), and which parameters it edits.
 *   --drag x0 y0 x1 y1       with --editor: on block --drag-block (default 16) press the
 *                            left button at (x0,y0) in the editor window and drag to (x1,y1),
 *                            as XTest input. A panel knob reaching the DAW shows as
 *                            performEdit calls; reaching the engine shows in MEASURED audio
 *                            Hz; reaching the save shows in the chunk. A drag on the
 *                            background is the negative control.
 *   --block-ms <n>           sleep this long per block (default 5 with --editor, 0 without):
 *                            the run loop needs wall-clock time for its timers.
 *   --screenshot <file.png>  with --editor: after the last block, "import -window root" to here.
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
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/gui/iplugview.h"
#include "se_gmpi_state_analysis.h"
#if defined(__linux__)
  #include <chrono>
  #include <thread>
  #include <sys/select.h>
  #include <X11/Xlib.h>
  #include <X11/Xutil.h>
  #include <X11/extensions/XTest.h>
#endif

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

/* ---- one parameter change in one block: the host's IParameterChanges ------ */

class ParamValueQueue : public IParamValueQueue
{
public:
    ParamID id = 0;
    ParamValue value = 0.0;
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE
    {
        if (iidIs(_iid, IParamValueQueue_iid) || iidIs(_iid, FUnknown_iid)) { *obj = static_cast<IParamValueQueue*>(this); return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    PROBE_REFCOUNT
    ParamID PLUGIN_API getParameterId() SMTG_OVERRIDE { return id; }
    int32 PLUGIN_API getPointCount() SMTG_OVERRIDE { return 1; }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& v) SMTG_OVERRIDE
    {
        if (index != 0) return kResultFalse;
        sampleOffset = 0;
        v = value;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32, ParamValue, int32&) SMTG_OVERRIDE { return kNotImplemented; }
};

class ParameterChanges : public IParameterChanges
{
public:
    std::vector<ParamValueQueue> queues;
    void add(ParamID id, ParamValue value)
    {
        for (auto& q : queues) if (q.id == id) { q.value = value; return; }
        ParamValueQueue q; q.id = id; q.value = value; queues.push_back(q);
    }
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE
    {
        if (iidIs(_iid, IParameterChanges_iid) || iidIs(_iid, FUnknown_iid)) { *obj = static_cast<IParameterChanges*>(this); return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    PROBE_REFCOUNT
    int32 PLUGIN_API getParameterCount() SMTG_OVERRIDE { return static_cast<int32>(queues.size()); }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) SMTG_OVERRIDE { return index >= 0 && (size_t)index < queues.size() ? &queues[(size_t)index] : nullptr; }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID&, int32&) SMTG_OVERRIDE { return nullptr; }
};

class ComponentHandler : public IComponentHandler
{
public:
    int edits = 0, restarts = 0;
    std::map<ParamID, ParamValue> lastEdit;        /* every id the plug-in edited, with its last value */
    std::vector<std::pair<ParamID, ParamValue>> pending; /* edits not yet handed to the processor */
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE
    {
        if (iidIs(_iid, IComponentHandler_iid) || iidIs(_iid, FUnknown_iid)) { *obj = static_cast<IComponentHandler*>(this); return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    PROBE_REFCOUNT
    tresult PLUGIN_API beginEdit(ParamID) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue v) SMTG_OVERRIDE
    {
        ++edits;
        lastEdit[id] = v;
        pending.push_back({id, v});
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) SMTG_OVERRIDE { ++restarts; return kResultOk; }
};

#if defined(__linux__)
/* The host side of an open editor: IPlugFrame (resize requests) and the Linux IRunLoop the
 * editor registers its X connection and its timer with. pump() is one turn of that loop. */
class PlugFrame : public IPlugFrame, public Steinberg::Linux::IRunLoop
{
public:
    struct Timer { Steinberg::Linux::ITimerHandler* handler; Steinberg::Linux::TimerInterval ms; std::chrono::steady_clock::time_point due; };
    std::vector<std::pair<Steinberg::Linux::IEventHandler*, Steinberg::Linux::FileDescriptor>> fds;
    std::vector<Timer> timers;
    int resizes = 0, fdWakeups = 0, timerTicks = 0;

    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE
    {
        if (iidIs(_iid, IPlugFrame_iid) || iidIs(_iid, FUnknown_iid)) { *obj = static_cast<IPlugFrame*>(this); return kResultOk; }
        if (iidIs(_iid, Steinberg::Linux::IRunLoop_iid)) { *obj = static_cast<Steinberg::Linux::IRunLoop*>(this); return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    PROBE_REFCOUNT
    tresult PLUGIN_API resizeView(IPlugView*, ViewRect*) SMTG_OVERRIDE { ++resizes; return kResultOk; }
    tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler* h, Steinberg::Linux::FileDescriptor fd) SMTG_OVERRIDE { fds.push_back({h, fd}); return kResultOk; }
    tresult PLUGIN_API unregisterEventHandler(Steinberg::Linux::IEventHandler* h) SMTG_OVERRIDE
    {
        for (size_t i = 0; i < fds.size(); ) { if (fds[i].first == h) fds.erase(fds.begin() + (long)i); else ++i; }
        return kResultOk;
    }
    tresult PLUGIN_API registerTimer(Steinberg::Linux::ITimerHandler* h, Steinberg::Linux::TimerInterval ms) SMTG_OVERRIDE
    {
        timers.push_back({h, ms, std::chrono::steady_clock::now() + std::chrono::milliseconds(ms)});
        return kResultOk;
    }
    tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler* h) SMTG_OVERRIDE
    {
        for (size_t i = 0; i < timers.size(); ) { if (timers[i].handler == h) timers.erase(timers.begin() + (long)i); else ++i; }
        return kResultOk;
    }
    void pump()
    {
        for (size_t i = 0; i < fds.size(); ++i)
        {
            fd_set set; FD_ZERO(&set); FD_SET(fds[i].second, &set);
            timeval tv{0, 0};
            if (select(fds[i].second + 1, &set, nullptr, nullptr, &tv) > 0) { ++fdWakeups; fds[i].first->onFDIsSet(fds[i].second); }
        }
        const auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < timers.size(); ++i)
            if (now >= timers[i].due) { timers[i].due = now + std::chrono::milliseconds(timers[i].ms); ++timerTicks; timers[i].handler->onTimer(); }
    }
};

/* XTest pointer input to the Xvfb: root coordinates equal window coordinates because the
 * editor's top-level window sits at (0,0) and no window manager runs there. */
static void fakeMotion(Display* dpy, int x, int y) { XTestFakeMotionEvent(dpy, -1, x, y, CurrentTime); XFlush(dpy); }
static void fakeButton(Display* dpy, bool down) { XTestFakeButtonEvent(dpy, 1, down ? True : False, CurrentTime); XFlush(dpy); }
#endif

int main(int argc, char** argv)
{
    const char* bundle = nullptr;
    const char* restorePath = nullptr;
    const char* savePath = nullptr;
    const char* resourcesDir = nullptr;
    int blocks = 8;
    bool wantController = true;
    int ccNumber = -1;      /* --cc */
    double ccValue = 0.0;
    int ccBlock = 4;        /* --cc-block */
    bool wantEditor = false; /* --editor */
    int drag[4] = {0, 0, 0, 0}; bool wantDrag = false; int dragBlock = 16; /* --drag, --drag-block */
    int blockMs = -1;       /* --block-ms */
    const char* screenshotPath = nullptr; /* --screenshot */

    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--restore") && i + 1 < argc) restorePath = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) savePath = argv[++i];
        else if (!strcmp(argv[i], "--resources") && i + 1 < argc) resourcesDir = argv[++i];
        else if (!strcmp(argv[i], "--process") && i + 1 < argc) blocks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-controller")) wantController = false;
        else if (!strcmp(argv[i], "--cc") && i + 2 < argc) { ccNumber = atoi(argv[++i]); ccValue = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--cc-block") && i + 1 < argc) ccBlock = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--editor")) wantEditor = true;
        else if (!strcmp(argv[i], "--drag") && i + 4 < argc) { for (int k = 0; k < 4; ++k) drag[k] = atoi(argv[++i]); wantDrag = true; }
        else if (!strcmp(argv[i], "--drag-block") && i + 1 < argc) dragBlock = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--block-ms") && i + 1 < argc) blockMs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshotPath = argv[++i];
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

    /* ---- optional editor, hosted here under X11 --------------------------- */
#if defined(__linux__)
    PlugFrame frame;
    IPlugView* view = nullptr;
    Display* dpy = nullptr;
    Window win = 0;
    int editorW = 0, editorH = 0;
    if (wantEditor)
    {
        check("--editor needs a controller (not --no-controller)", controller != nullptr);
        dpy = XOpenDisplay(nullptr);
        check("XOpenDisplay succeeds (is DISPLAY set and an X server, e.g. Xvfb, running?)", dpy != nullptr);
        if (controller && dpy)
        {
            view = controller->createView(ViewType::kEditor);
            check("controller->createView(\"editor\") returns a view", view != nullptr);
        }
        if (view)
        {
            check("the view supports X11 embedding", view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) == kResultTrue);
            ViewRect r{};
            view->getSize(&r);
            const int w = r.getWidth() > 0 ? r.getWidth() : 200, h = r.getHeight() > 0 ? r.getHeight() : 200;
            editorW = w; editorH = h;
            printf("      editor size %dx%d\n", w, h);
            win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, (unsigned)w, (unsigned)h, 0, 0, 0x202020);
            XMapWindow(dpy, win);
            XSync(dpy, False);
            view->setFrame(&frame);
            check("view->attached(X11 window) succeeds", view->attached((void*)(uintptr_t)win, kPlatformTypeX11EmbedWindowID) == kResultTrue);
            XSync(dpy, False);
            printf("      run loop: %zu fd handler(s), %zu timer(s) registered by the editor\n", frame.fds.size(), frame.timers.size());
            check("the editor registered an fd and a timer with the host run loop", !frame.fds.empty() && !frame.timers.empty());
            for (int i = 0; i < 30; ++i) { frame.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); } /* map, expose, first paint */
        }
        if (blockMs < 0) blockMs = 5;
    }
    if (blockMs < 0) blockMs = 0;
    if (wantDrag) printf("      --drag: button 1 from (%d,%d) to (%d,%d) on block %d\n", drag[0], drag[1], drag[2], drag[3], dragBlock);
#else
    if (wantEditor || wantDrag) check("--editor is implemented for Linux/X11 only", false);
#endif

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
        const ParamID ccParamId = static_cast<ParamID>(10000 + ccNumber); /* MidiControllersParameterId + channel 0 * 130 + cc */
        if (ccNumber >= 0)
            printf("      --cc: MIDI controller %d = %.3f on block %d (as VST3 parameter %u)\n", ccNumber, ccValue, ccBlock, (unsigned)ccParamId);
        int editsForwarded = 0;
        for (int b = 0; b < blocks; ++b)
        {
#if defined(__linux__)
            if (view)
            {
                /* The host's main thread between two audio blocks: the editor's run loop turns. */
                frame.pump();
                if (wantDrag && b == dragBlock && dpy)
                {
                    fakeMotion(dpy, drag[0], drag[1]); frame.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    fakeButton(dpy, true);             frame.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    for (int k = 1; k <= 8; ++k)
                    {
                        fakeMotion(dpy, drag[0] + (drag[2] - drag[0]) * k / 8, drag[1] + (drag[3] - drag[1]) * k / 8);
                        frame.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    fakeButton(dpy, false);            frame.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    frame.pump();
                }
            }
            if (blockMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(blockMs));
#endif
            /* What a DAW does with performEdit: the value comes back to the processor as an input parameter change. */
            ParameterChanges changes;
            for (auto& e : handler.pending) { changes.add(e.first, e.second); ++editsForwarded; }
            handler.pending.clear();
            if (ccNumber >= 0 && b == ccBlock) changes.add(ccParamId, ccValue);

            ProcessData data{};
            data.processMode = kRealtime;
            data.symbolicSampleSize = kSample32;
            data.numSamples = 256;
            data.numOutputs = outBuses;
            data.outputs = outBuses > 0 ? buses.data() : nullptr;
            data.inputParameterChanges = changes.queues.empty() ? nullptr : static_cast<IParameterChanges*>(&changes);
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
        if (editsForwarded) printf("      %d performEdit value(s) handed back to the processor as parameter changes\n", editsForwarded);
        freq.print(48000.0);
#if defined(__linux__)
        if (view)
        {
            for (int i = 0; i < 20; ++i) { frame.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); } /* let the panel repaint */
            printf("      run loop served %d fd wakeup(s), %d timer tick(s); %d resizeView request(s)\n", frame.fdWakeups, frame.timerTicks, frame.resizes);

            /* MEASURED editor pixels: the screen area under the editor, read back from the X server. A
             * panel that painted shows many distinct colours; an editor that never presented shows one. */
            XWindowAttributes wa{};
            const bool haveAttr = XGetWindowAttributes(dpy, win, &wa) != 0;
            printf("      host window: %s, %dx%d at (%d,%d), depth %d\n", !haveAttr ? "no attributes" : wa.map_state == IsViewable ? "viewable" : wa.map_state == IsUnmapped ? "UNMAPPED" : "unviewable", wa.width, wa.height, wa.x, wa.y, wa.depth);
            if (XImage* img = XGetImage(dpy, DefaultRootWindow(dpy), 0, 0, (unsigned)editorW, (unsigned)editorH, AllPlanes, ZPixmap))
            {
                std::map<unsigned long, int> colours;
                int nonBlack = 0;
                for (int y = 0; y < editorH; ++y)
                    for (int x = 0; x < editorW; ++x)
                    {
                        const unsigned long p = XGetPixel(img, x, y) & 0xffffff;
                        if (p) ++nonBlack;
                        ++colours[p];
                    }
                unsigned long top = 0; int topCount = -1;
                for (auto& [c, n] : colours) if (n > topCount) { top = c; topCount = n; }
                printf("MEASURED editor pixels          %d of %d non-black, %zu distinct colours, commonest #%06lx x%d\n", nonBlack, editorW * editorH, colours.size(), top, topCount);
                check("the editor put pixels on screen (more than one colour under it)", colours.size() > 1);
                XDestroyImage(img);
            }
            else
                check("XGetImage of the editor area succeeds", false);

            if (screenshotPath && dpy)
            {
                std::string cmd = "import -window root \"" + std::string(screenshotPath) + "\"";
                const int rc = system(cmd.c_str());
                printf("      screenshot: %s (rc %d)\n", screenshotPath, rc);
            }
        }
#endif
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
    if (!handler.lastEdit.empty())
    {
        printf("MEASURED performEdit ids   ");
        for (auto& [id, v] : handler.lastEdit) printf(" %u=%.4f", (unsigned)id, v);
        printf("\n");
    }
    else
        printf("MEASURED performEdit ids    none\n");

    /* ---- teardown --------------------------------------------------------- */
#if defined(__linux__)
    if (view) { view->removed(); view->setFrame(nullptr); view->release(); }
    if (dpy) { if (win) XDestroyWindow(dpy, win); XCloseDisplay(dpy); }
#endif
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
