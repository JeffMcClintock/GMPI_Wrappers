// ---------------------------------------------------------------------------
// A minimal VST3 host that opens a plugin editor through the VST3 3.8.0 Wayland
// path (kPlatformTypeWaylandSurfaceID).
//
//   wayland_editor_host <plugin.vst3> [milliseconds]
//
// It implements the four things the specification requires of a Wayland host:
//
//   IHostApplication  - delivered via IPluginFactory3::setHostContext, and the
//                       only route by which a plugin can reach...
//   IWaylandHost      - ...whose openWaylandConnection() hands back a wl_display
//   IWaylandFrame     - implemented BY the IPlugFrame object; supplies the
//                       xdg_surface a plugin's popups anchor to
//   Linux::IRunLoop   - because a Linux plugin may not run its own event loop
//
// SIMPLIFICATION, and it is worth being explicit about: a real host is a
// compositor for its plugins, so plugin and host each have their own
// connection. Here they share one. That is legal - nothing requires
// openWaylandConnection to mint a new connection - and it exercises every part
// of the plugin's code path, but it does mean this does not test a host that
// proxies. The host deliberately never dispatches the display itself; the
// plugin's registered fd handler does it, and our own listeners fire from
// there.
//
// The plugin's pixels land on the compositor, not in our address space, so this
// cannot check them itself. Run it under the nested-compositor harness and
// screenshot - see run_wayland_editor_test.sh.
// ---------------------------------------------------------------------------

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iwaylandframe.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"

// See the note in SEVSTGUIEditorWayland.cpp: these IIDs are in none of the
// SDK's central *iids.cpp, so every module that names them defines its own.
// The SDK's editorhost sample does exactly this in wayland/window.cpp.
namespace Steinberg {
DEF_CLASS_IID (IWaylandHost)
DEF_CLASS_IID (IWaylandFrame)
}

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
// Wayland bits the host owns
// ---------------------------------------------------------------------------
struct WaylandBits
{
    wl_display*       display{};
    wl_compositor*    compositor{};
    wl_subcompositor* subcompositor{};
    wl_shm*           shm{};
    xdg_wm_base*      wmBase{};

    wl_surface*   surface{};
    xdg_surface*  xdgSurface{};
    xdg_toplevel* toplevel{};
    bool          configured = false;

    int width = 400, height = 300;
};

WaylandBits g_wl;

void registryGlobal(void*, wl_registry* reg, uint32_t name, const char* iface, uint32_t)
{
    if (!strcmp(iface, wl_compositor_interface.name))
        g_wl.compositor = (wl_compositor*)wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_subcompositor_interface.name))
        g_wl.subcompositor = (wl_subcompositor*)wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, wl_shm_interface.name))
        g_wl.shm = (wl_shm*)wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name))
        g_wl.wmBase = (xdg_wm_base*)wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
}

void wmPing(void*, xdg_wm_base* b, uint32_t serial) { xdg_wm_base_pong(b, serial); }

void xdgSurfaceConfigure(void*, xdg_surface* s, uint32_t serial)
{
    xdg_surface_ack_configure(s, serial);
    g_wl.configured = true;
}

// Fill the host window with a colour the plugin would never draw, so a
// screenshot can tell "the plugin painted" from "the host is all you see".
bool paintHostWindow()
{
    const int stride = g_wl.width * 4;
    const int size = stride * g_wl.height;

    char name[] = "/tmp/gmpi-wl-host-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0)
        return false;
    unlink(name);
    if (ftruncate(fd, size) < 0) { close(fd); return false; }

    auto* pixels = (uint32_t*)mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) { close(fd); return false; }

    for (int i = 0; i < g_wl.width * g_wl.height; ++i)
        pixels[i] = 0xff201060;   // opaque dark purple

    auto* pool = wl_shm_create_pool(g_wl.shm, fd, size);
    auto* buffer = wl_shm_pool_create_buffer(pool, 0, g_wl.width, g_wl.height, stride,
                                             WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    wl_surface_attach(g_wl.surface, buffer, 0, 0);
    wl_surface_damage_buffer(g_wl.surface, 0, 0, g_wl.width, g_wl.height);
    wl_surface_commit(g_wl.surface);
    return true;
}

// ---------------------------------------------------------------------------
// IWaylandHost
// ---------------------------------------------------------------------------
class WaylandHost : public IWaylandHost
{
public:
    wl_display* PLUGIN_API openWaylandConnection() override { return g_wl.display; }

    tresult PLUGIN_API closeWaylandConnection(wl_display* d) override
    {
        // Shared with the host here, so there is nothing to close. A real host
        // would tear down the plugin's connection at this point.
        return (d == g_wl.display) ? kResultTrue : kInvalidArgument;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IWaylandHost)
        QUERY_INTERFACE(iid, obj, IWaylandHost::iid, IWaylandHost)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
};

WaylandHost g_waylandHost;

// ---------------------------------------------------------------------------
// IHostApplication - the plugin reaches IWaylandHost through this
// ---------------------------------------------------------------------------
class HostApp : public Vst::IHostApplication
{
public:
    tresult PLUGIN_API getName(Vst::String128 name) override
    {
        static const char16_t n[] = u"GMPI Wayland Test Host";
        memcpy(name, n, sizeof(n));
        return kResultTrue;
    }

    tresult PLUGIN_API createInstance(TUID cid, TUID _iid, void** obj) override
    {
        if (FUnknownPrivate::iidEqual(cid, IWaylandHost::iid) &&
            FUnknownPrivate::iidEqual(_iid, IWaylandHost::iid))
        {
            *obj = &g_waylandHost;
            return kResultTrue;
        }
        *obj = nullptr;
        return kNotImplemented;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, Vst::IHostApplication)
        QUERY_INTERFACE(iid, obj, Vst::IHostApplication::iid, Vst::IHostApplication)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
};

// ---------------------------------------------------------------------------
// IPlugFrame + IWaylandFrame + Linux::IRunLoop
// ---------------------------------------------------------------------------
class Frame : public IPlugFrame, public IWaylandFrame, public Linux::IRunLoop
{
public:
    struct TimerEntry { Linux::ITimerHandler* handler; uint64_t intervalMs; int64_t nextDueMs; };

    std::map<Linux::FileDescriptor, Linux::IEventHandler*> eventHandlers;
    std::vector<TimerEntry> timers;

    tresult PLUGIN_API resizeView(IPlugView*, ViewRect*) override { return kResultTrue; }

    //--- IWaylandFrame ---
    wl_surface* PLUGIN_API getWaylandSurface(wl_display*) override { return g_wl.surface; }

    int parentSurfaceQueries = 0;

    xdg_surface* PLUGIN_API getParentSurface(ViewRect& parentSize, wl_display*) override
    {
        // Counted: this is the plugin asking for something to anchor popups to.
        // Without it a context menu has no xdg_surface and cannot open at all,
        // so "did the plugin ask" is worth reporting separately from "did it
        // draw".
        ++parentSurfaceQueries;

        // Our xdg_surface IS the plugin's parent surface, so the offset is zero.
        parentSize = { 0, 0, g_wl.width, g_wl.height };
        return g_wl.xdgSurface;
    }

    xdg_toplevel* PLUGIN_API getParentToplevel(wl_display*) override { return g_wl.toplevel; }

    //--- Linux::IRunLoop ---
    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* h, Linux::FileDescriptor fd) override
    {
        if (!h || fd < 0) return kInvalidArgument;
        eventHandlers[fd] = h;
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* h) override
    {
        for (auto it = eventHandlers.begin(); it != eventHandlers.end();)
            it = (it->second == h) ? eventHandlers.erase(it) : std::next(it);
        return kResultTrue;
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* h, Linux::TimerInterval ms) override
    {
        if (!h || ms == 0) return kInvalidArgument;
        timers.push_back({ h, ms, nowMs() + int64_t(ms) });
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* h) override
    {
        std::erase_if(timers, [h](const TimerEntry& t) { return t.handler == h; });
        return kResultTrue;
    }

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

            // Deliberately no wl_display_dispatch of our own: the display is
            // shared, and two readers racing on one connection is a hang.
            poll(fds.data(), fds.size(), 5);

            for (size_t i = 0; i < fds.size(); ++i)
                if (fds[i].revents & POLLIN)
                    handlers[i]->onFDIsSet(fds[i].fd);

            const int64_t t = nowMs();
            for (auto& timer : timers)
                if (t >= timer.nextDueMs)
                {
                    timer.nextDueMs = t + int64_t(timer.intervalMs);
                    timer.handler->onTimer();
                }

            // The HOST has to commit its own surface for a plugin's subsurface
            // to appear: adding a sub-surface is a change to the PARENT's
            // state, and takes effect on the parent's next commit. The plugin
            // cannot do it - iwaylandframe.h forbids it from touching the
            // parent - so a host that never re-commits shows a blank plugin
            // area forever. Real hosts commit as part of their own rendering;
            // this stands in for that.
            wl_surface_commit(g_wl.surface);
            wl_display_flush(g_wl.display);
        }
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IPlugFrame)
        QUERY_INTERFACE(iid, obj, IPlugFrame::iid, IPlugFrame)
        QUERY_INTERFACE(iid, obj, IWaylandFrame::iid, IWaylandFrame)
        QUERY_INTERFACE(iid, obj, Linux::IRunLoop::iid, Linux::IRunLoop)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
};

} // anonymous namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <plugin.vst3> [ms]\n", argv[0]);
        return 2;
    }

    const char* pluginPath = argv[1];
    const int64_t runMs = (argc > 2) ? std::atoll(argv[2]) : 2000;

    // --- host window -------------------------------------------------------
    g_wl.display = wl_display_connect(nullptr);
    if (!g_wl.display)
    {
        std::fprintf(stderr, "SKIP: no Wayland display\n");
        return 77;
    }

    static const wl_registry_listener regListener = { registryGlobal, [](void*, wl_registry*, uint32_t) {} };
    auto* registry = wl_display_get_registry(g_wl.display);
    wl_registry_add_listener(registry, &regListener, nullptr);
    wl_display_roundtrip(g_wl.display);

    if (!g_wl.compositor || !g_wl.shm || !g_wl.wmBase || !g_wl.subcompositor)
    {
        std::fprintf(stderr, "FAIL: compositor lacks %s\n",
                     !g_wl.subcompositor ? "wl_subcompositor" : "required globals");
        return 1;
    }

    static const xdg_wm_base_listener wmListener = { wmPing };
    xdg_wm_base_add_listener(g_wl.wmBase, &wmListener, nullptr);

    g_wl.surface = wl_compositor_create_surface(g_wl.compositor);
    g_wl.xdgSurface = xdg_wm_base_get_xdg_surface(g_wl.wmBase, g_wl.surface);

    static const xdg_surface_listener surfListener = { xdgSurfaceConfigure };
    xdg_surface_add_listener(g_wl.xdgSurface, &surfListener, nullptr);

    g_wl.toplevel = xdg_surface_get_toplevel(g_wl.xdgSurface);
    xdg_toplevel_set_title(g_wl.toplevel, "GMPI Wayland Test Host");
    wl_surface_commit(g_wl.surface);

    wl_display_roundtrip(g_wl.display);
    if (!g_wl.configured)
    {
        std::fprintf(stderr, "FAIL: host window never configured\n");
        return 1;
    }
    if (!paintHostWindow())
    {
        std::fprintf(stderr, "FAIL: could not paint host window\n");
        return 1;
    }
    wl_display_roundtrip(g_wl.display);
    std::printf("host window mapped: %dx%d\n", g_wl.width, g_wl.height);
    std::fflush(stdout);

    // Sit here, mapped and plugin-free, long enough for the harness to capture
    // a "before" screenshot. The check is a diff of before against after, which
    // is the only way to tell what the plugin drew that does not depend on
    // where the compositor put the window or on how much of it the view covers
    // - an earlier check derived the window box from the host's own colour, and
    // reported a perfectly working 400x200 analyser as having drawn nothing
    // because it had covered the part being measured.
    {
        const int64_t settleEnd = nowMs() + 2000;
        while (nowMs() < settleEnd)
        {
            wl_display_roundtrip(g_wl.display);
            usleep(50 * 1000);
        }
    }

    // --- plugin ------------------------------------------------------------
    std::string error;
    auto module = VST3::Hosting::Module::create(pluginPath, error);
    if (!module)
    {
        std::fprintf(stderr, "FAIL: could not load module: %s\n", error.c_str());
        return 1;
    }

    static HostApp hostApp;

    // Must happen before anything creates a view: this is how IWaylandHost
    // becomes reachable at all.
    auto factory = module->getFactory();
    if (auto f3 = FUnknownPtr<IPluginFactory3>(factory.get()))
        f3->setHostContext(&hostApp);
    else
    {
        std::fprintf(stderr, "FAIL: plugin factory is not an IPluginFactory3\n");
        return 1;
    }

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
    if (!provider || !provider->getController())
    {
        std::fprintf(stderr, "FAIL: could not instantiate plugin\n");
        return 1;
    }

    IPtr<IPlugView> view = owned(provider->getController()->createView(Vst::ViewType::kEditor));
    if (!view)
    {
        std::printf("SKIP: plugin has no editor\n");
        return 77;
    }

    if (view->isPlatformTypeSupported(kPlatformTypeWaylandSurfaceID) != kResultTrue)
    {
        std::fprintf(stderr, "FAIL: view does not support WaylandSurfaceID "
                             "(fell back to the X11 editor?)\n");
        return 1;
    }

    ViewRect size{};
    view->getSize(&size);
    std::printf("view size: %dx%d\n", size.right - size.left, size.bottom - size.top);

    Frame frame;
    view->setFrame(&frame);

    if (view->attached(g_wl.surface, kPlatformTypeWaylandSurfaceID) != kResultTrue)
    {
        std::fprintf(stderr, "FAIL: attached() failed\n");
        return 1;
    }
    std::printf("attached: ok (%zu fd handlers, %zu timers)\n",
                frame.eventHandlers.size(), frame.timers.size());
    std::printf("IWaylandFrame::getParentSurface queries: %d\n", frame.parentSurfaceQueries);

    if (frame.eventHandlers.empty() || frame.timers.empty())
    {
        std::fprintf(stderr, "FAIL: plugin registered nothing with the run loop - "
                             "it cannot receive events\n");
        return 1;
    }

    frame.run(runMs);

    view->removed();
    view->setFrame(nullptr);
    view = nullptr;
    provider = nullptr;

    if (!frame.eventHandlers.empty() || !frame.timers.empty())
    {
        std::fprintf(stderr, "FAIL: removed() left %zu handlers and %zu timers registered\n",
                     frame.eventHandlers.size(), frame.timers.size());
        return 1;
    }

    wl_display_roundtrip(g_wl.display);
    std::printf("PASS\n");
    return 0;
}
