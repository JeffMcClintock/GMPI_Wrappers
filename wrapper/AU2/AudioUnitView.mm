#import <AudioUnit/AudioUnit.h>
#import <AudioUnit/AUCocoaUIView.h>
#import <Cocoa/Cocoa.h>
// #include "GmpiSdkCommon.h"
// #include "GmpiApiEditor.h"
// #import "CocoaGfx.h"
// #include "DrawingFrameCommon.h"
// #include "DrawingFrameMac.h"

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

// todo, make a controller class that inherits from gmpi::api::IEditorHost (paramHost), add it to instrument base, have AudioUnitGetProperty return a pointer to it's IUnknown interface.
// Then create the GMPI Editor (client) by calling the factory directly and pass these to:
// NSView* native = [[GMPI_VIEW_CLASS alloc] initWithClient:client parameterHost:paramHost preferredSize:inPreferredSize];

- (NSView *) uiViewForAudioUnit:(AudioUnit)inAudioUnit withSize:(NSSize)inPreferredSize
{
    // get the IEditorHost from teh Audiounit
    gmpi::api::IUnknown editController{};
    UInt32 size = sizeof (editController);
    if (AudioUnitGetProperty (inAudioUnit, 64000, kAudioUnitScope_Global, 0, &editController, &size) != noErr)
        return nil;

    // create the gmpi editor
    gmpi::shared_ptr<gmpi::api::IEditor> editor;
    {
        gmpi::shared_ptr<gmpi::api::IUnknown> factoryBase;
	    auto r = MP_GetFactory(factoryBase.put_void());

	    gmpi::shared_ptr<gmpi::api::IPluginFactory> factory;
	    auto r2 = factoryBase->queryInterface(&gmpi::api::IPluginFactory::guid, factory.put_void());

	    if (!factory || r != gmpi::ReturnCode::Ok)
	    {
		    return {};
	    }

	    gmpi::shared_ptr<gmpi::api::IUnknown> pluginUnknown;
	    r2 = factory->createInstance(info.id.c_str(), gmpi::api::PluginSubtype::Editor, pluginUnknown.put_void());
	    if (!pluginUnknown || r != gmpi::ReturnCode::Ok)
	    {
		    return {};
	    }

	    editor = pluginUnknown.as<gmpi::api::IEditor>();

	    if(!editor)
	    {
		    return {};
	    }
    }

    const CGFloat defaultW = (inPreferredSize.width  > 0.0 ? inPreferredSize.width  : 480.0);
    const CGFloat defaultH = (inPreferredSize.height > 0.0 ? inPreferredSize.height : 320.0);
    NSRect frame = NSMakeRect(0, 0, defaultW, defaultH);

    //NSView* view = [[NSView alloc] initWithFrame:frame];
    NSView* view = [[GMPI_VIEW_CLASS alloc] initWithClient:editor.get() parameterHost:editController.get() preferredSize:inPreferredSize];

    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    /*
    // Optional: give a neutral background so hosts don’t show black
    view.wantsLayer = YES;
    if (view.layer)
    {
        view.layer.backgroundColor = NSColor.blueColor.CGColor;
    }
    */

    return view; // ARC: no autorelease needed
}

@end

int shittyFunction() // force linker to not discard this unit.
{
    return 23;
}
