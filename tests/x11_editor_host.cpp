// ---------------------------------------------------------------------------
// A minimal VST3 host that actually OPENS a plugin editor on Linux.
//
// The Steinberg validator never calls IPlugView::attached, so it cannot tell a
// working editor from one that returns kResultTrue and draws nothing. This does
// the real thing: loads the bundle, creates the view, embeds it in an X11
// window, drives the host-side run loop the specification requires, and dumps
// the window's pixels.
//
// Usage:
//     x11_editor_host <plugin.vst3> [output.ppm] [milliseconds]
//
// Exit status is 0 only if the editor attached AND painted something other than
// a single flat colour - "it didn't crash" is not evidence that it drew.
// ---------------------------------------------------------------------------

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <poll.h>
#include <time.h>

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

using namespace Steinberg;

namespace
{

int64_t nowMs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// ---------------------------------------------------------------------------
// The host side of the Linux contract: a run loop the plugin registers its file
// descriptors and timers with. Everything a real DAW must provide, minimally.
// ---------------------------------------------------------------------------
class RunLoop : public Linux::IRunLoop, public IPlugFrame
{
public:
    struct TimerEntry { Linux::ITimerHandler* handler; uint64_t intervalMs; int64_t nextDueMs; };

    std::map<Linux::FileDescriptor, Linux::IEventHandler*> eventHandlers;
    std::vector<TimerEntry> timers;

    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* handler, Linux::FileDescriptor fd) override
    {
        if (!handler || fd < 0)
            return kInvalidArgument;
        eventHandlers[fd] = handler;
        return kResultTrue;
    }

    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* handler) override
    {
        for (auto it = eventHandlers.begin(); it != eventHandlers.end();)
            it = (it->second == handler) ? eventHandlers.erase(it) : std::next(it);
        return kResultTrue;
    }

    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* handler, Linux::TimerInterval ms) override
    {
        if (!handler || ms == 0)
            return kInvalidArgument;
        timers.push_back({ handler, ms, nowMs() + int64_t(ms) });
        return kResultTrue;
    }

    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* handler) override
    {
        std::erase_if(timers, [handler](const TimerEntry& t) { return t.handler == handler; });
        return kResultTrue;
    }

    tresult PLUGIN_API resizeView(IPlugView*, ViewRect*) override { return kResultTrue; }

    void run(int64_t durationMs)
    {
        const int64_t end = nowMs() + durationMs;

        while (nowMs() < end)
        {
            std::vector<pollfd> fds;
            std::vector<Linux::IEventHandler*> handlers;
            for (auto& [fd, h] : eventHandlers)
            {
                fds.push_back(pollfd{ fd, POLLIN, 0 });
                handlers.push_back(h);
            }

            poll(fds.data(), fds.size(), 5);

            for (size_t i = 0; i < fds.size(); ++i)
                if (fds[i].revents & POLLIN)
                    handlers[i]->onFDIsSet(fds[i].fd);

            const int64_t t = nowMs();
            for (auto& timer : timers)
            {
                if (t >= timer.nextDueMs)
                {
                    timer.nextDueMs = t + int64_t(timer.intervalMs);
                    timer.handler->onTimer();
                }
            }
        }
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, Linux::IRunLoop)
        QUERY_INTERFACE(iid, obj, Linux::IRunLoop::iid, Linux::IRunLoop)
        QUERY_INTERFACE(iid, obj, IPlugFrame::iid, IPlugFrame)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
};

// Count distinct pixel values. A working editor produces many; a view that
// attached but painted nothing produces exactly one.
size_t writePpmAndCountColours(Display* dpy, Window win, int w, int h, const char* path)
{
    XImage* img = XGetImage(dpy, win, 0, 0, unsigned(w), unsigned(h), AllPlanes, ZPixmap);
    if (!img)
        return 0;

    std::vector<unsigned char> rgb(size_t(w) * h * 3);
    std::map<unsigned long, int> histogram;

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const unsigned long p = XGetPixel(img, x, y);
            ++histogram[p];
            const size_t o = (size_t(y) * w + x) * 3;
            rgb[o + 0] = (p >> 16) & 0xff;
            rgb[o + 1] = (p >> 8) & 0xff;
            rgb[o + 2] = p & 0xff;
        }
    }

    if (path)
    {
        if (FILE* f = std::fopen(path, "wb"))
        {
            std::fprintf(f, "P6\n%d %d\n255\n", w, h);
            std::fwrite(rgb.data(), 1, rgb.size(), f);
            std::fclose(f);
        }
    }

    XDestroyImage(img);
    return histogram.size();
}

} // anonymous namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <plugin.vst3> [out.ppm] [ms]\n", argv[0]);
        return 2;
    }

    const char* pluginPath = argv[1];
    const char* outPath = (argc > 2) ? argv[2] : nullptr;
    const int64_t runMs = (argc > 3) ? std::atoll(argv[3]) : 800;

    std::string error;
    auto module = VST3::Hosting::Module::create(pluginPath, error);
    if (!module)
    {
        std::fprintf(stderr, "FAIL: could not load module: %s\n", error.c_str());
        return 1;
    }

    auto factory = module->getFactory();
    IPtr<Vst::PlugProvider> provider;
    for (auto& classInfo : factory.classInfos())
    {
        if (classInfo.category() == kVstAudioEffectClass)
        {
            provider = owned(new Vst::PlugProvider(factory, classInfo, true));
            if (provider->initialize())
                break;
            provider = nullptr;
        }
    }

    if (!provider)
    {
        std::fprintf(stderr, "FAIL: no audio effect class could be instantiated\n");
        return 1;
    }

    auto controller = provider->getController();
    if (!controller)
    {
        std::fprintf(stderr, "FAIL: no edit controller\n");
        return 1;
    }

    IPtr<IPlugView> view = owned(controller->createView(Vst::ViewType::kEditor));
    if (!view)
    {
        // Not a failure. Plenty of plugins ship no editor at all (GMPI's own
        // Gain example is one), and reporting those as broken would train
        // everyone to ignore this test.
        std::printf("SKIP: plugin has no editor (createView returned null)\n");
        return 77;
    }

    if (view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue)
    {
        std::fprintf(stderr, "FAIL: view does not support X11EmbedWindowID\n");
        return 1;
    }

    ViewRect size{};
    view->getSize(&size);
    const int w = (std::max)(1, size.right - size.left);
    const int h = (std::max)(1, size.bottom - size.top);
    std::printf("view size: %dx%d\n", w, h);

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy)
    {
        std::fprintf(stderr, "FAIL: no X display\n");
        return 1;
    }

    const int screen = DefaultScreen(dpy);
    Window parent = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                                        unsigned(w), unsigned(h), 0,
                                        BlackPixel(dpy, screen), BlackPixel(dpy, screen));
    XSelectInput(dpy, parent, ExposureMask | StructureNotifyMask);
    XMapWindow(dpy, parent);
    XFlush(dpy);

    RunLoop runLoop;
    // setFrame BEFORE attached: that is where the plugin gets the run loop, and
    // a plugin that only looks for it in attached() would be broken.
    view->setFrame(&runLoop);

    if (view->attached(reinterpret_cast<void*>(parent), kPlatformTypeX11EmbedWindowID) != kResultTrue)
    {
        std::fprintf(stderr, "FAIL: attached() failed\n");
        return 1;
    }
    std::printf("attached: ok (%zu fd handlers, %zu timers registered)\n",
                runLoop.eventHandlers.size(), runLoop.timers.size());

    // Two captures with the loop running in between, so a driver script
    // (xdotool) can inject clicks and the caller can diff before against after.
    // Painting proves the renderer; only a diff proves input.
    runLoop.run(300);
    std::string beforePath;
    if (outPath)
    {
        beforePath = std::string(outPath) + ".before.ppm";
        writePpmAndCountColours(dpy, parent, w, h, beforePath.c_str());
        std::printf("captured before: %s\n", beforePath.c_str());
        std::fflush(stdout);
    }

    runLoop.run(runMs);

    const size_t colours = writePpmAndCountColours(dpy, parent, w, h, outPath);
    std::printf("distinct colours in window: %zu\n", colours);

    view->removed();
    view->setFrame(nullptr);
    view = nullptr;
    provider = nullptr;

    XDestroyWindow(dpy, parent);
    XCloseDisplay(dpy);

    if (runLoop.eventHandlers.size() != 0 || runLoop.timers.size() != 0)
    {
        std::fprintf(stderr, "FAIL: removed() left %zu handlers and %zu timers registered\n",
                     runLoop.eventHandlers.size(), runLoop.timers.size());
        return 1;
    }

    if (colours < 2)
    {
        std::fprintf(stderr, "FAIL: window is a single flat colour - the editor drew nothing\n");
        return 1;
    }

    std::printf("PASS\n");
    return 0;
}
