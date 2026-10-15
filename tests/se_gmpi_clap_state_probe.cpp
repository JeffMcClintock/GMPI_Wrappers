/* se_gmpi_clap_state_probe -- the CLAP twin of se_gmpi_state_probe.cpp: which of a SynthEdit
 * export's parameters survive a CLAP state save, in a bare host with no DAW and no window.
 *
 * Stage 3 of TideSynth/docs/state-management-consolidation-plan.md moved the export's panel
 * controller (MpController) into the plug-in's own <Controller/>, so every GMPI wrapper gets
 * the engine preset for free. This probe is how that claim is measured on a second wrapper,
 * with the same reader as the VST3 probe (se_gmpi_state_analysis.h) so the two are quoted
 * side by side. Hosting shape follows TideSynth/tests/e79_clap_headless_probe.c.
 *
 *   se_gmpi_clap_state_probe <plugin.clap> [options]
 *
 *   --restore <preset.xml>   state->load this chunk (raw XML; the CLAP wrapper reads the
 *                            whole stream) before saving.
 *   --save <out.xml>         write the saved chunk here.
 *   --process <N>            run N blocks of audio between restore and save (default 8;
 *                            0 skips activation).
 *   --resources <dir>        the Resources folder holding parameters.se.xml, when it is
 *                            not <bundle dir>/Contents/Resources.
 *   --editor                 open the plug-in's clap.gui in an X11 window on $DISPLAY (an Xvfb
 *                            will do). This probe is the host's clap.timer-support and
 *                            clap.posix-fd-support, pumped between blocks. A parameter the
 *                            plug-in reports (CLAP_EVENT_PARAM_VALUE on out_events) is handed
 *                            back on the next block's in_events, as a DAW does.
 *   --drag x0 y0 x1 y1       with --editor: XTest left-button drag on block --drag-block
 *                            (default 16). MEASURED performEdit ids / audio Hz / the chunk show
 *                            how far a panel knob reaches; a drag on the background is the control.
 *   --block-ms <n>           sleep per block (default 5 with --editor, else 0).
 *   --screenshot <file.png>  with --editor: "import -window root" after the last block.
 *
 * Build (linux): g++ -std=c++17 -O1 -I <clap-include> se_gmpi_clap_state_probe.cpp -o se_gmpi_clap_state_probe -ldl -lX11 -lXtst
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <sys/stat.h>
#include <clap/clap.h>
#include "se_gmpi_state_analysis.h"
#include <chrono>
#include <map>
#include <thread>
#include <sys/select.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

using namespace se_state_analysis;

static int failures = 0;
static void check(const char* what, bool ok)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

/* ---- the smallest host a plugin will accept, plus the run loop an editor needs ------------ */
static int restartRequested = 0;
static int flushRequested = 0;
static void host_noop(const clap_host_t*) {}
static void host_request_restart(const clap_host_t*) { restartRequested = 1; }

/* clap.timer-support and clap.posix-fd-support: what the X11 editor registers with. pump() is
 * one turn of the host run loop: readable fds, then due timers. */
struct HostTimer { clap_id id; uint32_t ms; std::chrono::steady_clock::time_point due; };
static std::vector<HostTimer> hostTimers;
static std::vector<int> hostFds;
static clap_id nextTimerId = 1;
static int fdWakeups = 0, timerTicks = 0;
static bool host_register_timer(const clap_host_t*, uint32_t period_ms, clap_id* timer_id)
{
    *timer_id = nextTimerId++;
    hostTimers.push_back({ *timer_id, period_ms, std::chrono::steady_clock::now() + std::chrono::milliseconds(period_ms) });
    return true;
}
static bool host_unregister_timer(const clap_host_t*, clap_id timer_id)
{
    for (size_t i = 0; i < hostTimers.size(); ++i) if (hostTimers[i].id == timer_id) { hostTimers.erase(hostTimers.begin() + (long)i); return true; }
    return false;
}
static bool host_register_fd(const clap_host_t*, int fd, clap_posix_fd_flags_t) { hostFds.push_back(fd); return true; }
static bool host_modify_fd(const clap_host_t*, int, clap_posix_fd_flags_t) { return true; }
static bool host_unregister_fd(const clap_host_t*, int fd)
{
    for (size_t i = 0; i < hostFds.size(); ++i) if (hostFds[i] == fd) { hostFds.erase(hostFds.begin() + (long)i); return true; }
    return false;
}
static const clap_host_timer_support_t hostTimerSupport = { host_register_timer, host_unregister_timer };
static const clap_host_posix_fd_support_t hostPosixFdSupport = { host_register_fd, host_modify_fd, host_unregister_fd };

/* clap.params on the host side: the wrapper asks for a flush while inactive. */
static void host_params_rescan(const clap_host_t*, clap_param_rescan_flags) {}
static void host_params_clear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
static void host_params_request_flush(const clap_host_t*) { flushRequested = 1; }
static const clap_host_params_t hostParams = { host_params_rescan, host_params_clear, host_params_request_flush };

static const void* host_get_extension(const clap_host_t*, const char* id)
{
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &hostTimerSupport;
    if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &hostPosixFdSupport;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &hostParams;
    return nullptr;
}

static clap_host_t host = {
    CLAP_VERSION_INIT, nullptr, "se-gmpi-clap-state-probe", "SynthEdit", "", "1.0",
    host_get_extension, host_request_restart, host_noop, host_noop,
};

static void pumpRunLoop(const clap_plugin_t* plug)
{
    auto* fdExt = static_cast<const clap_plugin_posix_fd_support_t*>(plug->get_extension(plug, CLAP_EXT_POSIX_FD_SUPPORT));
    auto* timerExt = static_cast<const clap_plugin_timer_support_t*>(plug->get_extension(plug, CLAP_EXT_TIMER_SUPPORT));
    for (size_t i = 0; i < hostFds.size(); ++i)
    {
        fd_set set; FD_ZERO(&set); FD_SET(hostFds[i], &set);
        timeval tv{ 0, 0 };
        if (select(hostFds[i] + 1, &set, nullptr, nullptr, &tv) > 0 && fdExt) { ++fdWakeups; fdExt->on_fd(plug, hostFds[i], CLAP_POSIX_FD_READ); }
    }
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < hostTimers.size(); ++i)
        if (now >= hostTimers[i].due && timerExt) { hostTimers[i].due = now + std::chrono::milliseconds(hostTimers[i].ms); ++timerTicks; timerExt->on_timer(plug, hostTimers[i].id); }
}

static void fakeMotion(Display* dpy, int x, int y) { XTestFakeMotionEvent(dpy, -1, x, y, CurrentTime); XFlush(dpy); }
static void fakeButton(Display* dpy, bool down) { XTestFakeButtonEvent(dpy, 1, down ? True : False, CurrentTime); XFlush(dpy); }

/* ---- streams ----------------------------------------------------------------------------- */
struct Sink { std::string bytes; };
static int64_t sink_write(const clap_ostream_t* s, const void* buffer, uint64_t size)
{
    static_cast<Sink*>(s->ctx)->bytes.append(static_cast<const char*>(buffer), static_cast<size_t>(size));
    return static_cast<int64_t>(size);
}
struct Source { const std::string* buf; size_t pos = 0; };
static int64_t source_read(const clap_istream_t* s, void* buffer, uint64_t size)
{
    auto* src = static_cast<Source*>(s->ctx);
    const size_t left = src->buf->size() - src->pos;
    const size_t n = size < left ? static_cast<size_t>(size) : left;
    memcpy(buffer, src->buf->data() + src->pos, n);
    src->pos += n;
    return static_cast<int64_t>(n);
}

/* ---- event lists: parameter values in, whatever the plug-in reports out ------------------- */
static std::vector<clap_event_param_value_t> inEvents;
static uint32_t in_size(const clap_input_events_t*) { return static_cast<uint32_t>(inEvents.size()); }
static const clap_event_header_t* in_get(const clap_input_events_t*, uint32_t i) { return i < inEvents.size() ? &inEvents[i].header : nullptr; }

static std::map<clap_id, double> lastReported;                 /* every param the plug-in reported, with its last value */
static std::vector<std::pair<clap_id, double>> pendingReported; /* not yet handed back to the plug-in */
static int reportedCount = 0;
static bool out_try_push(const clap_output_events_t*, const clap_event_header_t* h)
{
    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
    {
        const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
        ++reportedCount;
        lastReported[e->param_id] = e->value;
        pendingReported.push_back({ e->param_id, e->value });
    }
    return true;
}

int main(int argc, char** argv)
{
    const char* bundle = nullptr;
    const char* restorePath = nullptr;
    const char* savePath = nullptr;
    const char* resourcesDir = nullptr;
    int blocks = 8;
    bool wantEditor = false;
    int drag[4] = { 0, 0, 0, 0 }; bool wantDrag = false; int dragBlock = 16;
    int blockMs = -1;
    const char* screenshotPath = nullptr;
    const uint32_t blockSize = 256;
    const double sampleRate = 48000.0;

    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--restore") && i + 1 < argc) restorePath = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) savePath = argv[++i];
        else if (!strcmp(argv[i], "--resources") && i + 1 < argc) resourcesDir = argv[++i];
        else if (!strcmp(argv[i], "--process") && i + 1 < argc) blocks = atoi(argv[++i]);
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
        fprintf(stderr, "usage: se_gmpi_clap_state_probe <plugin.clap> [--restore preset.xml] [--save out.xml] [--process N] [--resources dir]\n");
        return 2;
    }
    printf("se_gmpi_clap_state_probe: %s\n", bundle);

    /* On Linux a .clap is the shared object itself. Its Resources sit beside it the way the
     * VST3 bundle lays them out: <dir>/Contents/Resources, with the binary in
     * <dir>/Contents/x86_64-linux/<name>.clap - which is also accepted as the argument. */
    std::string modulePath(bundle);
    std::string dir(bundle);
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    {
        struct stat st;
        if (stat(bundle, &st) == 0 && S_ISDIR(st.st_mode))
        {
            std::string leaf = dir.substr(dir.find_last_of('/') + 1);
            if (const auto dot = leaf.find_last_of('.'); dot != std::string::npos) leaf = leaf.substr(0, dot);
            modulePath = dir + "/Contents/x86_64-linux/" + leaf + ".clap";
        }
        else
        {
            /* <dir>/Contents/x86_64-linux/<name>.clap -> <dir> */
            const auto a = dir.rfind("/Contents/");
            if (a != std::string::npos) dir = dir.substr(0, a);
        }
    }
    const std::string resources = resourcesDir ? resourcesDir : dir + "/Contents/Resources";
    printf("      module -> %s\n      resources -> %s\n", modulePath.c_str(), resources.c_str());

    void* lib = dlopen(modulePath.c_str(), RTLD_LOCAL | RTLD_NOW);
    check("the plug-in binary loads", lib != nullptr);
    if (!lib) { fprintf(stderr, "  dlerror: %s\n", dlerror()); return 1; }

    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
    check("clap_entry is exported", entry != nullptr);
    if (!entry) return 1;
    check("clap_entry->init succeeds", entry->init(modulePath.c_str()));

    auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    check("the plugin factory is offered", factory != nullptr);
    if (!factory) return 1;
    const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
    check("descriptor 0 exists", desc != nullptr);
    if (!desc) return 1;
    printf("      plugin: %s (%s)\n", desc->name, desc->id);

    const clap_plugin_t* plug = factory->create_plugin(factory, &host, desc->id);
    check("create_plugin returns an instance", plug != nullptr);
    if (!plug) return 1;
    check("plugin->init succeeds", plug->init(plug));

    auto* state = static_cast<const clap_plugin_state_t*>(plug->get_extension(plug, CLAP_EXT_STATE));
    check("the plugin offers clap.state", state != nullptr);
    if (!state) return 1;

    std::string restoreXml;
    if (restorePath)
    {
        check("the restore preset reads", slurp(restorePath, restoreXml) && !restoreXml.empty());
        Source src{ &restoreXml };
        clap_istream_t is{ &src, source_read };
        check("state->load accepts the preset", state->load(plug, &is));
        printf("      restore chunk: %zu bytes from %s\n", restoreXml.size(), restorePath);
    }
    else
        printf("      no restore: the plug-in saves its bundled defaults\n");

    /* ---- optional editor, hosted here under X11 --------------------------------------------- */
    Display* dpy = nullptr;
    Window win = 0;
    const clap_plugin_gui_t* gui = nullptr;
    int editorW = 0, editorH = 0;
    if (wantEditor)
    {
        dpy = XOpenDisplay(nullptr);
        check("XOpenDisplay succeeds (is DISPLAY set and an X server, e.g. Xvfb, running?)", dpy != nullptr);
        gui = static_cast<const clap_plugin_gui_t*>(plug->get_extension(plug, CLAP_EXT_GUI));
        check("the plugin offers clap.gui", gui != nullptr);
        if (gui && dpy)
        {
            check("clap.gui supports embedded X11", gui->is_api_supported(plug, CLAP_WINDOW_API_X11, false));
            check("gui->create(X11) succeeds", gui->create(plug, CLAP_WINDOW_API_X11, false));
            uint32_t w = 0, h = 0;
            gui->get_size(plug, &w, &h);
            editorW = w > 0 ? (int)w : 200; editorH = h > 0 ? (int)h : 200;
            printf("      editor size %dx%d\n", editorW, editorH);
            win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, (unsigned)editorW, (unsigned)editorH, 0, 0, 0x202020);
            XMapWindow(dpy, win);
            XSync(dpy, False);
            clap_window_t cw{};
            cw.api = CLAP_WINDOW_API_X11;
            cw.x11 = static_cast<unsigned long>(win);
            check("gui->set_parent(X11 window) succeeds", gui->set_parent(plug, &cw));
            gui->show(plug);
            XSync(dpy, False);
            printf("      run loop: %zu fd(s), %zu timer(s) registered by the plug-in\n", hostFds.size(), hostTimers.size());
            check("the editor registered an fd and a timer with the host run loop", !hostFds.empty() && !hostTimers.empty());
            for (int i = 0; i < 30; ++i) { pumpRunLoop(plug); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        }
        if (blockMs < 0) blockMs = 5;
    }
    if (blockMs < 0) blockMs = 0;
    if (wantDrag) printf("      --drag: button 1 from (%d,%d) to (%d,%d) on block %d\n", drag[0], drag[1], drag[2], drag[3], dragBlock);

    FrequencyEstimate freq;
    if (blocks > 0)
    {
        check("activate succeeds", plug->activate(plug, sampleRate, 1, blockSize));
        check("start_processing succeeds", plug->start_processing(plug));
        if (restartRequested)
        {
            restartRequested = 0;
            plug->stop_processing(plug);
            plug->deactivate(plug);
            check("re-activate after request_restart", plug->activate(plug, sampleRate, 1, blockSize));
            check("re-start_processing after request_restart", plug->start_processing(plug));
        }

        uint32_t channels = 2;
        if (auto* ports = static_cast<const clap_plugin_audio_ports_t*>(plug->get_extension(plug, CLAP_EXT_AUDIO_PORTS)); ports)
        {
            clap_audio_port_info_t info{};
            if (ports->count(plug, false) > 0 && ports->get(plug, 0, false, &info))
                channels = info.channel_count;
        }
        std::vector<std::vector<float>> store(channels, std::vector<float>(blockSize, 0.0f));
        std::vector<float*> chans(channels);
        for (uint32_t c = 0; c < channels; ++c) chans[c] = store[c].data();
        clap_audio_buffer_t outBus{};
        outBus.data32 = chans.data();
        outBus.channel_count = channels;

        clap_input_events_t inEv{ nullptr, in_size, in_get };
        clap_output_events_t outEv{ nullptr, out_try_push };

        double peak = 0.0;
        int processFailures = 0;
        int64_t frames = 0;
        int handedBack = 0;
        for (int b = 0; b < blocks; ++b)
        {
            if (gui)
            {
                pumpRunLoop(plug);
                if (wantDrag && b == dragBlock && dpy)
                {
                    fakeMotion(dpy, drag[0], drag[1]); pumpRunLoop(plug); std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    fakeButton(dpy, true);             pumpRunLoop(plug); std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    for (int k = 1; k <= 8; ++k)
                    {
                        fakeMotion(dpy, drag[0] + (drag[2] - drag[0]) * k / 8, drag[1] + (drag[3] - drag[1]) * k / 8);
                        pumpRunLoop(plug); std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    fakeButton(dpy, false);            pumpRunLoop(plug); std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    pumpRunLoop(plug);
                }
            }
            if (blockMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(blockMs));

            /* What a DAW does with a reported value: it comes back as an input parameter event. */
            inEvents.clear();
            for (auto& e : pendingReported)
            {
                clap_event_param_value_t ev{};
                ev.header.size = sizeof(ev);
                ev.header.time = 0;
                ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                ev.header.type = CLAP_EVENT_PARAM_VALUE;
                ev.param_id = e.first;
                ev.value = e.second;
                ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1;
                inEvents.push_back(ev);
                ++handedBack;
            }
            pendingReported.clear();

            for (auto& ch : store) memset(ch.data(), 0, ch.size() * sizeof(float));
            clap_process_t p{};
            p.steady_time = frames;
            p.frames_count = blockSize;
            p.audio_outputs = &outBus;
            p.audio_outputs_count = 1;
            p.in_events = &inEv;
            p.out_events = &outEv;
            if (plug->process(plug, &p) == CLAP_PROCESS_ERROR) ++processFailures;
            for (uint32_t c = 0; c < channels; ++c)
                for (uint32_t i = 0; i < blockSize; ++i) { const double a = fabs((double)chans[c][i]); if (a > peak) peak = a; }
            if (b >= blocks / 2) freq.feed(chans[0], static_cast<int>(blockSize));
            frames += blockSize;
        }
        check("no block returned CLAP_PROCESS_ERROR", processFailures == 0);
        printf("      %d blocks: audio out channels %u, peak %.4f (%.1f dBFS)\n", blocks, channels, peak, peak > 0 ? 20.0 * log10(peak) : -1000.0);
        if (handedBack) printf("      %d reported value(s) handed back to the plug-in as parameter events\n", handedBack);
        freq.print(sampleRate);
        if (gui)
        {
            for (int i = 0; i < 20; ++i) { pumpRunLoop(plug); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            printf("      run loop served %d fd wakeup(s), %d timer tick(s)\n", fdWakeups, timerTicks);
            if (XImage* img = XGetImage(dpy, DefaultRootWindow(dpy), 0, 0, (unsigned)editorW, (unsigned)editorH, AllPlanes, ZPixmap))
            {
                std::map<unsigned long, int> colours;
                int nonBlack = 0;
                for (int y = 0; y < editorH; ++y)
                    for (int x = 0; x < editorW; ++x) { const unsigned long px = XGetPixel(img, x, y) & 0xffffff; if (px) ++nonBlack; ++colours[px]; }
                printf("MEASURED editor pixels          %d of %d non-black, %zu distinct colours\n", nonBlack, editorW * editorH, colours.size());
                check("the editor put pixels on screen (more than one colour under it)", colours.size() > 1);
                XDestroyImage(img);
            }
            if (screenshotPath)
            {
                const std::string cmd = "import -window root \"" + std::string(screenshotPath) + "\"";
                printf("      screenshot: %s (rc %d)\n", screenshotPath, system(cmd.c_str()));
            }
        }

        plug->stop_processing(plug);
        plug->deactivate(plug);
    }

    Sink sink;
    clap_ostream_t os{ &sink, sink_write };
    check("state->save succeeds", state->save(plug, &os));
    std::string chunk = sink.bytes;
    if (const auto nul = chunk.find('\0'); nul != std::string::npos) chunk.resize(nul); /* the CLAP wrapper writes a terminating NUL */
    if (savePath)
    {
        if (FILE* f = fopen(savePath, "wb")) { fwrite(chunk.data(), 1, chunk.size(), f); fclose(f); printf("      saved chunk to %s\n", savePath); }
        else check("the --save path is writable", false);
    }

    report(chunk, resources, restoreXml);
    if (!lastReported.empty())
    {
        printf("MEASURED performEdit ids   ");
        for (auto& [id, v] : lastReported) printf(" %u=%.4f", (unsigned)id, v);
        printf("   (%d CLAP_EVENT_PARAM_VALUE out events)\n", reportedCount);
    }
    else
        printf("MEASURED performEdit ids    none\n");

    if (gui)
    {
        gui->hide(plug);
        gui->destroy(plug);
    }
    if (dpy) { if (win) XDestroyWindow(dpy, win); XCloseDisplay(dpy); }

    plug->destroy(plug);
    entry->deinit();
    dlclose(lib);

    printf("\n%s\n", failures ? "SOME PROBE CHECKS FAILED" : "all probe checks passed");
    return failures ? 1 : 0;
}
