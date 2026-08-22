#import <AudioUnit/AudioUnit.h>
#import <AudioUnit/AUCocoaUIView.h>
#include "GmpiSdkCommon.h"
#include "GmpiApiEditor.h"
#include "backends/DrawingFrameMac.h"
#include "Hosting/gmpi_factory.h"
#include "Hosting/controller_holder.h"
#include "backends/GmpiObjCNames.h"

extern "C"
gmpi::ReturnCode MP_GetFactory( void** returnInterface );

// This class name must match the string returned in GetProperty(kAudioUnitProperty_CocoaUI).
// It now DOES, structurally: both come from GMPI_OBJC_NAME/GMPI_OBJC_NAME_STR on the
// same base, so a per-plugin suffix cannot rename one without the other.
// it's only purpose is to instantiate an NSView (the editor).
#define GMPI_VIEW_MAKER_CLASS GMPI_OBJC_NAME(GMPI_VIEW_MAKER_VERSION_02)

@interface GMPI_VIEW_MAKER_CLASS : NSObject <AUCocoaUIBase>
@end

@implementation GMPI_VIEW_MAKER_CLASS

// AU Cocoa UI protocol version (0 is fine for simple UIs)
- (unsigned int)interfaceVersion
{
    return 0;
}

- (NSString*) description
{
    return @"GMPI AU View";
}

- (NSView *) uiViewForAudioUnit:(AudioUnit)inAudioUnit withSize:(NSSize)inPreferredSize
{
    // get the IEditorHost from the Audiounit
    gmpi::api::IUnknown* editController{};
    UInt32 size = sizeof (editController);
    if (AudioUnitGetProperty (inAudioUnit, 64000, kAudioUnitScope_Global, 0, &editController, &size) != noErr)
        return nil;

    // create the gmpi editor
    gmpi::shared_ptr<gmpi::api::IEditor> editor;
    {
        auto& factory = gmpi::hosting::factory::getInstance();
        auto pluginInfo = factory.getPluginInfo();
        
        auto pluginUnknown = factory.createInstance(pluginInfo->id.c_str(), gmpi::api::PluginSubtype::Editor);
        if (!pluginUnknown)
            return {};

        editor = pluginUnknown.as<gmpi::api::IEditor>();

        if(!editor)
            return {};
    }
    
    auto pluginGraphics_GMPI = editor.as<gmpi::api::IDrawingClient>();
    
    const gmpi::drawing::Size availableSize{ 99999.f, 99999.f };
    gmpi::drawing::Size desiredSize{ 100.f, 100.f };
    if(pluginGraphics_GMPI)
        pluginGraphics_GMPI->measure(&availableSize, &desiredSize);

    NSView* view = (NSView*) createNativeView(
          nullptr
        , (class IUnknown*) editController
        , (class IUnknown*) editor.get()
        , desiredSize.width, desiredSize.height
        );

//    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    if(editor)
    {
        auto controller = dynamic_cast<gmpi::hosting::gmpi_controller_holder*>(editController);
        controller->initUi(editor.get());
    }
    
    return view; // ARC: no autorelease needed
}

@end

int heyLinkerDontDiscardAudioUnitView_mm() // force linker to not discard this unit.
{
    return 23;
}
