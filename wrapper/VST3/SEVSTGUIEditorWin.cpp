#include "SEVSTGUIEditorWin.h"
#include "Controller_VST3.h"

namespace wrapper
{
// TODO !!! pass IUnknown to constructor, then QueryInterface for IDrawingClient
SEVSTGUIEditorWin::SEVSTGUIEditorWin(gmpi::hosting::pluginInfo const& info, gmpi::shared_ptr<gmpi::api::IEditor>& peditor, wrapper::Controller_VST3* pcontroller, int pwidth, int pheight) :
	VST3EditorBase(info, peditor, pcontroller, pwidth, pheight)
{
    drawingframe.setFallbackHost(static_cast<gmpi::api::IEditorHost*>(&pcontroller->gmpiController));

    if (pluginParameters_GMPI)
    {
        pluginParameters_GMPI->setHost(static_cast<gmpi::api::IDrawingHost*>(&drawingframe));
    }

    // DPI of system. only a GUESS at this point of DPI we will be using. (until we know DAW window handle).
    {
        HDC hdc = ::GetDC(NULL);
        Dpi = GetDeviceCaps(hdc, LOGPIXELSX) / 96.f;
        ::ReleaseDC(NULL, hdc);
    }

    if (auto drawingClient = peditor.as<gmpi::api::IDrawingClient>(); drawingClient)
    {
        gmpi::drawing::Size desiredSize{ 100.f, 100.f };
        gmpi::drawing::Size availableSize{ 99999.f, 99999.f };
        drawingClient->measure(&availableSize, &desiredSize);

        width = static_cast<int>(Dpi * desiredSize.width);
        height = static_cast<int>(Dpi * desiredSize.height);
    }
}

SEVSTGUIEditorWin::~SEVSTGUIEditorWin()
{
//    controller->gmpiController.unRegisterGui(&helper);
    if (pluginParameters_GMPI)
    {
        controller->gmpiController.unRegisterGui(pluginParameters_GMPI.get());
    }
}

Steinberg::tresult PLUGIN_API SEVSTGUIEditorWin::attached (void* parent, Steinberg::FIDString type)
{
    // now that we know which monitor we're on, update Dpi.
    Dpi = GetDpiForWindow((HWND) parent) / 96.f;

    if (pluginGraphics_GMPI)
    {
        drawingframe.attachClient(pluginGraphics_GMPI.get());

        const gmpi::drawing::SizeL overrideSize{ width, height };
        drawingframe.open(parent, &overrideSize);

//        controller->gmpiController.initUi(&helper);
    }

    if (pluginParameters_GMPI)
    {
        pluginParameters_GMPI->initialize();

        controller->gmpiController.initUi(pluginParameters_GMPI.get());
    }

    initPlugin();

	return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API SEVSTGUIEditorWin::removed ()
{
//    onCloseNativeView(nsView);
    if (pluginParameters_GMPI)
    {
        controller->gmpiController.unRegisterGui(pluginParameters_GMPI.get());
    }

	return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API SEVSTGUIEditorWin::getSize (Steinberg::ViewRect* size)
{
    *size = {0, 0, width, height};
	return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API SEVSTGUIEditorWin::onSize(Steinberg::ViewRect* newSize)
{
    drawingframe.reSize(newSize->left, newSize->top, newSize->right, newSize->bottom);
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API SEVSTGUIEditorWin::canResize()
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
        {
            return Steinberg::kResultTrue;
        }
    }
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API SEVSTGUIEditorWin::checkSizeConstraint(Steinberg::ViewRect* rect)
{
    if (pluginGraphics_GMPI)
    {
		const gmpi::drawing::Size availableSize{ static_cast<float>(rect->right - rect->left) / Dpi, static_cast<float>(rect->bottom - rect->top) / Dpi };
        gmpi::drawing::Size desiredSize{ availableSize };
		pluginGraphics_GMPI->measure(&availableSize, &desiredSize);

        if (availableSize.width == desiredSize.width && availableSize.height == desiredSize.height)
        {
            return Steinberg::kResultTrue;
        }
    }
    return Steinberg::kResultFalse;
}
}