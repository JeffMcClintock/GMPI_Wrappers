#import <AudioUnit/AudioUnit.h>
#import <AudioUnit/AUCocoaUIView.h>
#include "GmpiSdkCommon.h"
#include "GmpiApiEditor.h"
#include "backends/DrawingFrameMac.h"
#include "Hosting/gmpi_factory.h"
#include "Hosting/controller_holder.h"

extern "C"
gmpi::ReturnCode MP_GetFactory( void** returnInterface );

// This class name must match the string returned in GetProperty(kAudioUnitProperty_CocoaUI)
// it's only purpose is to instantiate an NSView (the editor).
@interface GMPI_VIEW_MAKER_VERSION_02 : NSObject <AUCocoaUIBase>
@end

@implementation GMPI_VIEW_MAKER_VERSION_02

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
    // get the IEditorHost from teh Audiounit
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

//    width = static_cast<int>(desiredSize.width);
//    height = static_cast<int>(desiredSize.height);


 //   const CGFloat defaultW = (inPreferredSize.width  > 0.0 ? inPreferredSize.width  : 480.0);
 //   const CGFloat defaultH = (inPreferredSize.height > 0.0 ? inPreferredSize.height : 320.0);
 //   NSRect frame = NSMakeRect(0, 0, defaultW, defaultH);

    //NSView* view = [[NSView alloc] initWithFrame:frame];
    //NSView* view = [[GMPI_VIEW_CLASS alloc] initWithClient:editor.get() parameterHost:editController.get() preferredSize:inPreferredSize];

    NSView* view = (NSView*) createNativeView(
          nullptr
        , (class IUnknown*) editController
        , (class IUnknown*) editor.get()
        , desiredSize.width, desiredSize.height
        );

//    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    /*
    // Optional: give a neutral background so hosts don’t show black
    view.wantsLayer = YES;
    if (view.layer)
    {
        view.layer.backgroundColor = NSColor.blueColor.CGColor;
    }
    */
    auto editorParams = editor.as<gmpi::api::IParameterObserver>();
    if(editorParams)
    {
        auto controller = dynamic_cast<gmpi::hosting::gmpi_controller_holder*>(editController); //.as<gmpi::api::IDrawingClient>();
        
        controller->registerGui(editorParams.get());
    }

    return view; // ARC: no autorelease needed
}

@end

int heyLinkerDontDiscardAudioUnitView_mm() // force linker to not discard this unit.
{
    return 23;
}
