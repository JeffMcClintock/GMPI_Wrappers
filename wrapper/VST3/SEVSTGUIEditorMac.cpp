#include "SEVSTGUIEditorMac.h"
#include "adelaycontroller.h"

// without including objective-C headers, we need to create an SynthEditCocoaView (NSView).
// forward declare function here to return the view, using void* as return type.
void* createNativeView(void* parent, class IUnknown* parameterHost, class IUnknown* controller, int width, int height);
void onCloseNativeView(void* ptr);
void resizeNativeView(void* view, int width, int height);

namespace wrapper
{

SEVSTGUIEditorMac::SEVSTGUIEditorMac(pluginInfoSem const& info, gmpi::shared_ptr<gmpi::api::IEditor>& peditor, VST3Controller* pcontroller, int pwidth, int pheight) :
    VST3EditorBase(info, peditor, pcontroller, pwidth, pheight)
{
}

Steinberg::tresult PLUGIN_API SEVSTGUIEditorMac::attached (void* parent, Steinberg::FIDString type)
{
    nsView = createNativeView(parent, (class IUnknown*) static_cast<gmpi::api::IEditorHost*>(&helper), (class IUnknown*) pluginGraphics_GMPI.get(), width, height);

    initPlugin();
    
	return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API SEVSTGUIEditorMac::removed ()
{
    onCloseNativeView(nsView);
    
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
        const gmpi::drawing::Size availableSize{ static_cast<float>(rect->right - rect->left), static_cast<float>(rect->bottom - rect->top) };
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
