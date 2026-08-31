#include "SEVSTGUIEditorLinux.h"
#include "Controller_VST3.h"

#include <algorithm>

#include "helpers/CpuTextEngine.h"
#include "helpers/Timer.h"
#include "helpers/DecodeImage.h"
#include "helpers/FontProvider.h"

using namespace Steinberg;

namespace wrapper
{
namespace
{

// The host's run loop granularity for us. 16ms keeps meters and automation
// moving at roughly display rate without pinning a core; the timer only
// repaints when something actually invalidated.
constexpr Steinberg::uint64 kTimerIntervalMs = 16;

// The CPU backend contains no font, shaping or image-decode code, so every host
// of it must supply them. One engine for the whole module: it caches rasterised
// glyphs, and several plugin windows in one DAW should share that cache.
void wireTextStack(gmpi::cpugfx::Factory& factory)
{
    static gmpi::drawing::CpuTextEngine textEngine{ gmpi::drawing::findFont };
    static bool once = [&]
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

} // anonymous namespace

SEVSTGUIEditorLinux::SEVSTGUIEditorLinux(gmpi::hosting::pluginInfo const& info,
                                         gmpi::shared_ptr<gmpi::api::IEditor>& peditor,
                                         wrapper::Controller_VST3* pcontroller,
                                         int pwidth, int pheight)
    : VST3EditorBase(info, peditor, pcontroller, pwidth, pheight)
{
    wireTextStack(drawingframe.drawingFactory());
    drawingframe.setMenuFont(menuFont(drawingframe.drawingFactory()));

    // Before any setHost call: the plugin resolves IEditorHost during setHost,
    // and the frame can only forward that once it knows where to.
    drawingframe.setFallbackHost(static_cast<gmpi::api::IEditorHost*>(&pcontroller->gmpiController));

    if (pluginParameters_GMPI)
    {
        pluginParameters_GMPI->setHost(static_cast<gmpi::api::IDrawingHost*>(&drawingframe));
    }

    if (auto drawingClient = peditor.as<gmpi::api::IDrawingClient>(); drawingClient)
    {
        // Shared with the Windows and macOS editors - see VST3EditorBase.h. It
        // offers an unbounded size and ignores an echo of it, which is how a
        // resizable client says "anything"; only a real preference moves the
        // default.
        measurePreferredSize(drawingClient.get(), Dpi, width, height);
    }
}

SEVSTGUIEditorLinux::~SEVSTGUIEditorLinux()
{
    if (pluginParameters_GMPI)
    {
        controller->gmpiController.unRegisterGui(pluginParameters_GMPI.get());
    }
}

tresult PLUGIN_API SEVSTGUIEditorLinux::queryInterface(const TUID iid, void** obj)
{
    QUERY_INTERFACE(iid, obj, Linux::IEventHandler::iid, Linux::IEventHandler)
    QUERY_INTERFACE(iid, obj, Linux::ITimerHandler::iid, Linux::ITimerHandler)
    return VST3EditorBase::queryInterface(iid, obj);
}

tresult PLUGIN_API SEVSTGUIEditorLinux::isPlatformTypeSupported(FIDString type)
{
    // Only X11. Claiming otherwise gets us handed a window id we cannot use,
    // which is worse than the host reporting no editor.
    if (type && std::strcmp(type, kPlatformTypeX11EmbedWindowID) == 0)
        return kResultTrue;

    return kResultFalse;
}

tresult PLUGIN_API SEVSTGUIEditorLinux::setFrame(IPlugFrame* frame)
{
    // This is the only route to the host's run loop, and it arrives BEFORE
    // attached(). A host that supplies no IRunLoop leaves the editor static -
    // it will draw once and never respond, which is why the failure is worth
    // distinguishing from success rather than silently ignoring.
    runLoop = {};

    if (frame)
    {
        Linux::IRunLoop* loop{};
        if (frame->queryInterface(Linux::IRunLoop::iid, reinterpret_cast<void**>(&loop)) == kResultOk && loop)
        {
            runLoop = owned(loop);
        }
    }

    return kResultTrue;
}

tresult PLUGIN_API SEVSTGUIEditorLinux::attached(void* parent, FIDString type)
{
    if (isPlatformTypeSupported(type) != kResultTrue)
        return kResultFalse;

    if (pluginGraphics_GMPI)
    {
        drawingframe.attachClient(pluginGraphics_GMPI.get());

        // VST3 coordinates on Linux are physical pixels, so the view size is
        // handed straight through with no DPI conversion.
        if (!drawingframe.open(reinterpret_cast<uintptr_t>(parent), width, height))
            return kResultFalse;
    }

    if (pluginParameters_GMPI)
    {
        pluginParameters_GMPI->initialize();
        controller->gmpiController.initUi(pluginParameters_GMPI.get());
    }

    initPlugin();

    if (runLoop && drawingframe.isOpen())
    {
        runLoop->registerEventHandler(static_cast<Linux::IEventHandler*>(this),
                                      drawingframe.connectionFd());
        runLoop->registerTimer(static_cast<Linux::ITimerHandler*>(this), kTimerIntervalMs);
        registeredWithRunLoop = true;
    }

    return kResultTrue;
}

tresult PLUGIN_API SEVSTGUIEditorLinux::removed()
{
    // Unregister BEFORE closing: the run loop holds our fd, and closing the X
    // connection first leaves it polling a descriptor that may already have been
    // reused by something else in the host process.
    if (registeredWithRunLoop && runLoop)
    {
        runLoop->unregisterTimer(static_cast<Linux::ITimerHandler*>(this));
        runLoop->unregisterEventHandler(static_cast<Linux::IEventHandler*>(this));
    }
    registeredWithRunLoop = false;

    if (pluginParameters_GMPI)
    {
        controller->gmpiController.unRegisterGui(pluginParameters_GMPI.get());
    }

    drawingframe.close();

    return kResultTrue;
}

void PLUGIN_API SEVSTGUIEditorLinux::onFDIsSet(Linux::FileDescriptor /*fd*/)
{
    drawingframe.processEvents();
}

void PLUGIN_API SEVSTGUIEditorLinux::onTimer()
{
    // TIDE BACKLOG E74 -- and this is the ONLY place the process gets a
    // UI-thread tick on Linux, which is why it is here rather than anywhere
    // more obvious.
    //
    // gmpi::TimerManager has a native timer source on Windows (SetTimer) and
    // macOS (CFRunLoopTimer) and NONE on Linux (gmpi_ui/helpers/Timer.cpp), so
    // there it must be pumped. The standalone pumps it from its own loop; a
    // plug-in has no loop of its own and nothing pumped it at all. So every
    // gmpi::TimerClient in a hosted Linux plug-in never ran -- including
    // Controller_VST3::onTimer, which is the ONE caller of
    // message_que_dsp_to_ui.pollMessage(). The processor's whole DSP->GUI
    // channel was therefore dead: measured in REAPER 7.43 on TIDE, 2,700
    // parameter updates shipped by the processor and ZERO delivered to the
    // editor, against one-for-one in the standalone control on the same build.
    //
    // The no-argument pump() measures its own elapsed time, so several open
    // editors pumping the one process-wide manager cannot make it run fast.
    gmpi::TimerManager::instance()->pump();

    // Two jobs. processEvents() because some hosts register our fd but poll it
    // lazily, and onTimer() to flush invalidations that came from automation
    // rather than from input.
    drawingframe.processEvents();
    drawingframe.onTimer();
}

tresult PLUGIN_API SEVSTGUIEditorLinux::getSize(ViewRect* size)
{
    *size = { 0, 0, width, height };
    return kResultTrue;
}

tresult PLUGIN_API SEVSTGUIEditorLinux::onSize(ViewRect* newSize)
{
    width  = newSize->right - newSize->left;
    height = newSize->bottom - newSize->top;
    drawingframe.reSize(width, height);
    return kResultTrue;
}

tresult PLUGIN_API SEVSTGUIEditorLinux::canResize()
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

tresult PLUGIN_API SEVSTGUIEditorLinux::checkSizeConstraint(ViewRect* rect)
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
