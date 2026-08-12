#include "SEVSTGUIEditorMac.h"
#include "Controller_VST3.h"

// without including objective-C headers, we need to create an CocoaView (NSView).
// forward declare function here to return the view, using void* as return type.
void* createNativeView(void* parent, class IUnknown* parameterHost, class IUnknown* controller, int width, int height);
void gmpi_onCloseNativeView(void* ptr);
void resizeNativeView(void* view, int width, int height);
// Bounds an extent to what DrawingFrameMac.mm will actually honour. Declared here for
// the same reason as the three above: no Objective-C headers on this side.
void gmpi_clampEditorSize(int* width, int* height);

namespace wrapper
{

SEVSTGUIEditorMac::SEVSTGUIEditorMac(gmpi::hosting::pluginInfo const& info, gmpi::shared_ptr<gmpi::api::IEditor>& peditor, Controller_VST3* pcontroller, int pwidth, int pheight) :
    VST3EditorBase(info, peditor, pcontroller, pwidth, pheight)
{
}

Steinberg::tresult PLUGIN_API SEVSTGUIEditorMac::attached (void* parent, Steinberg::FIDString type)
{
    // Cocoa works in points, so no DPI factor here (unlike the Win32 editor).
    measurePreferredSize(pluginGraphics_GMPI.get(), 1.0f, width, height);


    nsView = createNativeView(parent, (class IUnknown*) static_cast<gmpi::api::IEditorHost*>(&controller->gmpiController), (class IUnknown*) pluginGraphics_GMPI.get(), width, height);
    
    initPlugin();
    if(pluginParameters_GMPI)
    {
        controller->gmpiController.initUi(pluginParameters_GMPI.get());
    }
    
	return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API SEVSTGUIEditorMac::removed ()
{
    if(pluginParameters_GMPI)
    {
        controller->gmpiController.unRegisterGui(pluginParameters_GMPI.get());
    }
    
    gmpi_onCloseNativeView(nsView);
    
	return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API SEVSTGUIEditorMac::getSize (Steinberg::ViewRect* size)
{
    *size = {0, 0, width, height};
	return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API SEVSTGUIEditorMac::onSize(Steinberg::ViewRect* newSize)
{
    //    drawingframe.reSize(newSize->left, newSize->top, newSize->right, newSize->bottom);
    resizeNativeView(nsView, newSize->right - newSize->left, newSize->bottom - newSize->top);
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API SEVSTGUIEditorMac::canResize()
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

Steinberg::tresult PLUGIN_API SEVSTGUIEditorMac::checkSizeConstraint(Steinberg::ViewRect* rect)
{
    if (pluginGraphics_GMPI)
    {
        // Bound before measuring. A resizable client measures to whatever it is offered,
        // so without this the answer to "may I use 2178 x 32672?" was an affirmative
        // kResultTrue -- the wrapper approving a size nothing behind it would honour.
        int offeredWidth  = rect->right  - rect->left;
        int offeredHeight = rect->bottom - rect->top;
        gmpi_clampEditorSize(&offeredWidth, &offeredHeight);

        const gmpi::drawing::Size availableSize{ static_cast<float>(offeredWidth), static_cast<float>(offeredHeight) };
        gmpi::drawing::Size desiredSize{ availableSize };
        pluginGraphics_GMPI->measure(&availableSize, &desiredSize);

        // ...and again after, because measure is the client's answer and a fixed-size
        // client can name any size it likes regardless of what it was offered.
        int acceptedWidth  = static_cast<int>(desiredSize.width  + 0.5f);
        int acceptedHeight = static_cast<int>(desiredSize.height + 0.5f);
        gmpi_clampEditorSize(&acceptedWidth, &acceptedHeight);

        if (acceptedWidth == rect->right - rect->left && acceptedHeight == rect->bottom - rect->top)
        {
            return Steinberg::kResultTrue;
        }

        // The view will not take the size offered. The VST3 contract is to write back
        // the nearest size it *will* take -- returning kResultFalse with the rect
        // untouched tells the host nothing, and a host that ignores the return value
        // then calls onSize with a number the view never agreed to. Same fix as the
        // Windows sibling (P4b); it was left undone here until there was a bound to
        // write back to, which is what gmpi_clampEditorSize now provides.
        //
        // Cocoa works in points, so unlike SEVSTGUIEditorWin there is no DPI factor.
        rect->right  = rect->left + acceptedWidth;
        rect->bottom = rect->top  + acceptedHeight;

        return Steinberg::kResultTrue;
    }
    return Steinberg::kResultFalse;
}
}
