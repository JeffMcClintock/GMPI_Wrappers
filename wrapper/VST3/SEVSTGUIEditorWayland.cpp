#include "SEVSTGUIEditorWayland.h"
#include "Controller_VST3.h"

#include <algorithm>
#include "MyVstPluginFactory.h"

#include "pluginterfaces/vst/ivsthostapplication.h"

#include "helpers/CpuTextEngine.h"
#include "helpers/DecodeImage.h"
#include "helpers/FontProvider.h"

// These two IIDs are declared in iwaylandframe.h and are NOT in any of the
// SDK's central *iids.cpp - not coreiids, commoniids, vstinitiids or baseiids -
// so linking those, as a plugin normally does for every other interface, does
// not supply them. Each module defines its own instead; the SDK does the same
// in vstgui4/plugin-bindings/vst3editor.cpp (plugin side) and in the editorhost
// sample's wayland/window.cpp (host side).
//
// Miss it and the module still LINKS - a shared object may carry undefined
// symbols - then fails to dlopen in the host with nothing in our build to say
// why. `nm -DC --undefined-only` on the built .so is how to catch it.
//
// Corollary: if this wrapper is ever built into a module that also compiles
// vst3editor.cpp, these become duplicate definitions. Delete one, do not
// silence the linker.
namespace Steinberg {
DEF_CLASS_IID (IWaylandHost)
DEF_CLASS_IID (IWaylandFrame)
}

using namespace Steinberg;

namespace wrapper
{
namespace
{

constexpr Steinberg::uint64 kTimerIntervalMs = 16;

// One text engine for the whole module - it caches rasterised glyphs, and
// several plugin windows in one DAW should share that cache. Identical to the
// X11 editor's wiring; the renderer underneath is the same CPU backend.
void wireTextStack(gmpi::cpugfx::Factory& factory)
{
    static gmpi::drawing::CpuTextEngine textEngine{ gmpi::drawing::findFont };
    static bool once = []
    {
        textEngine.imageDecoder = gmpi::drawing::decodeImageMemory;
        return true;
    }();
    (void)once;

    factory.textEngine   = &textEngine;
    factory.imageDecoder = gmpi::drawing::decodeImageFile;
}

// Menus draw their own labels - X11 and Wayland each supply a grabbing window
// and nothing else - so the frame needs a text format of its own. Built once
// per module, after the text engine is in place.
gmpi::drawing::api::ITextFormat* menuFont(gmpi::cpugfx::Factory& factory)
{
    static gmpi::drawing::TextFormat format = [&factory]
    {
        gmpi::drawing::Factory facade;
        *gmpi::drawing::AccessPtr::put(facade) = &factory;
        const std::string_view family{ "sans-serif" };
        return facade.createTextFormat(14.0f, std::span{ &family, 1 });
    }();
    return gmpi::drawing::AccessPtr::get(format);
}

// IWaylandHost is a singleton created through IHostApplication. Returns null on
// any host that predates 3.8.0, or that ships 3.8.0 headers without Wayland
// support - both are normal, and both mean "use X11".
IPtr<IWaylandHost> createWaylandHost()
{
    auto* context = MyVstPluginFactory::getHostContext();
    if (!context)
        return {};

    Vst::IHostApplication* hostApp{};
    if (context->queryInterface(Vst::IHostApplication::iid, reinterpret_cast<void**>(&hostApp)) != kResultOk || !hostApp)
        return {};

    // createInstance takes TUID by value (char[16] -> char*), so the FUIDs have
    // to be copied into writable buffers first.
    TUID cid, iid;
    IWaylandHost::iid.toTUID(cid);
    IWaylandHost::iid.toTUID(iid);

    IWaylandHost* host{};
    hostApp->createInstance(cid, iid, reinterpret_cast<void**>(&host));
    hostApp->release();

    return host ? owned(host) : IPtr<IWaylandHost>{};
}

} // anonymous namespace

bool SEVSTGUIEditorWayland::hostSupportsWayland()
{
    return createWaylandHost() != nullptr;
}

SEVSTGUIEditorWayland::SEVSTGUIEditorWayland(gmpi::hosting::pluginInfo const& info,
                                             gmpi::shared_ptr<gmpi::api::IEditor>& peditor,
                                             wrapper::Controller_VST3* pcontroller,
                                             int pwidth, int pheight)
    : VST3EditorBase(info, peditor, pcontroller, pwidth, pheight)
{
    wireTextStack(drawingframe.drawingFactory());
    drawingframe.setMenuFont(menuFont(drawingframe.drawingFactory()));

    // Before any setHost call - the plugin resolves IEditorHost during setHost,
    // and the frame can only forward that once it knows where to.
    drawingframe.setFallbackHost(static_cast<gmpi::api::IEditorHost*>(&pcontroller->gmpiController));

    if (pluginParameters_GMPI)
        pluginParameters_GMPI->setHost(static_cast<gmpi::api::IDrawingHost*>(&drawingframe));

    if (auto drawingClient = peditor.as<gmpi::api::IDrawingClient>(); drawingClient)
    {
        // Shared with the Windows and macOS editors - see VST3EditorBase.h. It
        // offers an unbounded size and ignores an echo of it, which is how a
        // resizable client says "anything"; only a real preference moves the
        // default.
        measurePreferredSize(drawingClient.get(), Dpi, width, height);
    }
}

SEVSTGUIEditorWayland::~SEVSTGUIEditorWayland()
{
    if (pluginParameters_GMPI)
        controller->gmpiController.unRegisterGui(pluginParameters_GMPI.get());

    // The connection is the host's to close, not ours. Connection::adopt left
    // owned_ false so its destructor will not call wl_display_disconnect - but
    // the destructor still runs first, so the close must happen after it, and
    // the frame must be gone before either. Sequencing is handled in removed();
    // this is the belt-and-braces path for a view destroyed without it.
    if (connectionOpen && waylandHost && display)
    {
        waylandHost->closeWaylandConnection(display);
        connectionOpen = false;
        display = {};
    }
}

tresult PLUGIN_API SEVSTGUIEditorWayland::queryInterface(const TUID iid, void** obj)
{
    QUERY_INTERFACE(iid, obj, Linux::IEventHandler::iid, Linux::IEventHandler)
    QUERY_INTERFACE(iid, obj, Linux::ITimerHandler::iid, Linux::ITimerHandler)
    return VST3EditorBase::queryInterface(iid, obj);
}

tresult PLUGIN_API SEVSTGUIEditorWayland::isPlatformTypeSupported(FIDString type)
{
    if (type && std::strcmp(type, kPlatformTypeWaylandSurfaceID) == 0)
        return kResultTrue;

    return kResultFalse;
}

tresult PLUGIN_API SEVSTGUIEditorWayland::setFrame(IPlugFrame* frame)
{
    runLoop = {};
    waylandFrame = {};

    if (frame)
    {
        Linux::IRunLoop* loop{};
        if (frame->queryInterface(Linux::IRunLoop::iid, reinterpret_cast<void**>(&loop)) == kResultOk && loop)
            runLoop = owned(loop);

        // IWaylandFrame is implemented BY the IPlugFrame object. Optional: a
        // host may embed us and still offer no popup parent, in which case
        // menus have nothing to anchor to and we simply do not open any.
        IWaylandFrame* wf{};
        if (frame->queryInterface(IWaylandFrame::iid, reinterpret_cast<void**>(&wf)) == kResultOk && wf)
            waylandFrame = owned(wf);
    }

    return kResultTrue;
}

void SEVSTGUIEditorWayland::resolvePopupParent()
{
    if (!waylandFrame || !display)
        return;

    // Dialogs of ours must be children of the host's toplevel, or a modal
    // question can fall behind the DAW window - which the user experiences as a
    // hang, with no way to answer it.
    drawingframe.setParentToplevel(waylandFrame->getParentToplevel(display));

    ViewRect parentSize{};
    if (auto* xdg = waylandFrame->getParentSurface(parentSize, display))
    {
        drawingframe.setPopupParent(
            xdg,
            { static_cast<float>(parentSize.left),  static_cast<float>(parentSize.top),
              static_cast<float>(parentSize.right), static_cast<float>(parentSize.bottom) });
    }
}

tresult PLUGIN_API SEVSTGUIEditorWayland::attached(void* parent, FIDString type)
{
    if (isPlatformTypeSupported(type) != kResultTrue || !parent)
        return kResultFalse;

    waylandHost = createWaylandHost();
    if (!waylandHost)
        return kResultFalse;

    display = waylandHost->openWaylandConnection();
    if (!display)
        return kResultFalse;
    connectionOpen = true;

    // adopt, not open: this display belongs to the host and must never be
    // wl_display_disconnect'd - closeWaylandConnection is its counterpart.
    if (!connection.adopt(display))
        return kResultFalse;

    if (pluginGraphics_GMPI)
    {
        drawingframe.attachClient(pluginGraphics_GMPI.get());

        if (!drawingframe.create(static_cast<wl_surface*>(parent), width, height))
            return kResultFalse;
    }

    resolvePopupParent();

    if (pluginParameters_GMPI)
    {
        pluginParameters_GMPI->initialize();
        controller->gmpiController.initUi(pluginParameters_GMPI.get());
    }

    initPlugin();

    if (runLoop)
    {
        runLoop->registerEventHandler(static_cast<Linux::IEventHandler*>(this),
                                      drawingframe.displayFd());
        runLoop->registerTimer(static_cast<Linux::ITimerHandler*>(this), kTimerIntervalMs);
        registeredWithRunLoop = true;
    }

    return kResultTrue;
}

tresult PLUGIN_API SEVSTGUIEditorWayland::removed()
{
    // Unregister first: the run loop holds the connection fd, and closing the
    // connection while it is still registered leaves the host polling a
    // descriptor that may already have been reused.
    if (registeredWithRunLoop && runLoop)
    {
        runLoop->unregisterTimer(static_cast<Linux::ITimerHandler*>(this));
        runLoop->unregisterEventHandler(static_cast<Linux::IEventHandler*>(this));
    }
    registeredWithRunLoop = false;

    if (pluginParameters_GMPI)
        controller->gmpiController.unRegisterGui(pluginParameters_GMPI.get());

    drawingframe.detachClient();

    if (connectionOpen && waylandHost && display)
    {
        waylandHost->closeWaylandConnection(display);
        connectionOpen = false;
        display = {};
    }

    return kResultTrue;
}

void PLUGIN_API SEVSTGUIEditorWayland::onFDIsSet(Linux::FileDescriptor)
{
    drawingframe.dispatch();
}

void PLUGIN_API SEVSTGUIEditorWayland::onTimer()
{
    drawingframe.tick();
}

tresult PLUGIN_API SEVSTGUIEditorWayland::getSize(ViewRect* size)
{
    *size = { 0, 0, width, height };
    return kResultTrue;
}

tresult PLUGIN_API SEVSTGUIEditorWayland::onSize(ViewRect* newSize)
{
    width  = newSize->right - newSize->left;
    height = newSize->bottom - newSize->top;
    drawingframe.resize(width, height);

    // The parent moved relative to us, or we to it; popup anchoring is
    // expressed in that offset, so it has to be re-read.
    resolvePopupParent();
    return kResultTrue;
}

tresult PLUGIN_API SEVSTGUIEditorWayland::canResize()
{
    if (pluginGraphics_GMPI)
    {
        const gmpi::drawing::Size availableSize1{ 0.0f, 0.0f };
        const gmpi::drawing::Size availableSize2{ 10000.0f, 10000.0f };
        gmpi::drawing::Size desiredSize1{ availableSize1 };
        gmpi::drawing::Size desiredSize2{ availableSize1 };
        pluginGraphics_GMPI->measure(&availableSize1, &desiredSize1);
        pluginGraphics_GMPI->measure(&availableSize2, &desiredSize2);

        if (desiredSize1.width != desiredSize2.width || desiredSize1.height != desiredSize2.height)
            return kResultTrue;
    }
    return kResultFalse;
}

tresult PLUGIN_API SEVSTGUIEditorWayland::checkSizeConstraint(ViewRect* rect)
{
    if (pluginGraphics_GMPI)
    {
        const gmpi::drawing::Size availableSize{ static_cast<float>(rect->right - rect->left) / Dpi,
                                                 static_cast<float>(rect->bottom - rect->top) / Dpi };
        gmpi::drawing::Size desiredSize{ availableSize };
        pluginGraphics_GMPI->measure(&availableSize, &desiredSize);

        if (availableSize.width == desiredSize.width && availableSize.height == desiredSize.height)
            return kResultTrue;
    }
    return kResultFalse;
}

}
