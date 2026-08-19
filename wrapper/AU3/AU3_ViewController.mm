#import "AU3_ViewController.h"
#import "AU3_Wrapper.h"

#include <TargetConditionals.h>

#include "GmpiSdkCommon.h"
#include "GmpiApiEditor.h"
#include "helpers/NativeUi.h" // IDrawingClient, gmpi::drawing types
#include "Hosting/gmpi_factory.h"
#include "Hosting/controller_holder.h"
#include "helpers/IController.h"

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
typedef UIView GmpiHostPlatformView;
#else
#import <AppKit/AppKit.h>
typedef NSView GmpiHostPlatformView;
#endif

// gmpi_ui's per-platform view host (DrawingFrameMac.mm / DrawingFrameIos.mm).
void* createNativeView(void* parent, class IUnknown* parameterHost, class IUnknown* client, int width, int height);
void  gmpi_onCloseNativeView(void* ptr);

@implementation GmpiAUViewController
{
	GmpiAudioUnit* _audioUnit;
	gmpi::shared_ptr<gmpi::api::IEditor> editor;
	GmpiHostPlatformView* nativeEditorView; // owns the +1 from createNativeView's alloc
	CGSize measuredSize;
}

@synthesize audioUnit = _audioUnit;

- (AUAudioUnit*)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
												  error:(NSError**)error
{
	_audioUnit = [[GmpiAudioUnit alloc] initWithComponentDescription:desc options:0 error:error];

	// May be called off-main; view work always belongs to the main thread.
	dispatch_async(dispatch_get_main_queue(), ^{
		[self connectEditorToUnit];
	});

	return _audioUnit;
}

- (void)loadView
{
	// No nib: an empty container the editor view is added to. (NSViewController
	// throws without this; UIViewController would cope but explicit is uniform.)
	//
	// The editor is created and measured HERE, not when the audio unit arrives:
	// creation needs only the plugin factory, and the measured size must be on
	// preferredContentSize before the view-bridge delivers the view - a size
	// set after delivery does not reach the host's proxy. Wiring to the unit
	// (createNativeView + initUi) still waits for both halves in
	// connectEditorToUnit.
	gmpi::drawing::Size desiredSize{ 400.f, 300.f };

	auto& factory = gmpi::hosting::factory::getInstance();
	if (auto pluginInfo = factory.getPluginInfo(); pluginInfo)
	{
		if (auto pluginUnknown = factory.createInstance(pluginInfo->id.c_str(), gmpi::api::PluginSubtype::Editor); pluginUnknown)
			editor = pluginUnknown.as<gmpi::api::IEditor>();
	}

	if (auto drawingClient = editor.as<gmpi::api::IDrawingClient>(); drawingClient)
	{
		const gmpi::drawing::Size availableSize{ 99999.f, 99999.f };
		drawingClient->measure(&availableSize, &desiredSize);
	}

	measuredSize = CGSizeMake(desiredSize.width, desiredSize.height);
	self.view = [[[GmpiHostPlatformView alloc] initWithFrame:CGRectMake(0, 0, measuredSize.width, measuredSize.height)] autorelease];
	self.preferredContentSize = measuredSize;
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	[self connectEditorToUnit];
}

// Runs whenever unit or view arrives; wires editor to unit once both exist.
- (void)connectEditorToUnit
{
	if (!_audioUnit || !self.viewLoaded || !editor || nativeEditorView)
		return;

	auto controllerHolder = [_audioUnit gmpiController];

	// Same wiring as the AU2 Cocoa view factory (AudioUnitView.mm), minus the
	// property-64000 handshake; the editor itself was made in loadView.
	nativeEditorView = (GmpiHostPlatformView*)createNativeView(
		  (void*)self.view
		, (class IUnknown*)static_cast<gmpi::api::IEditorHost*>(controllerHolder)
		, (class IUnknown*)editor.get()
		, (int)measuredSize.width
		, (int)measuredSize.height
	);

	controllerHolder->initUi(editor.get());
}

- (void)dealloc
{
	if (nativeEditorView)
	{
		gmpi_onCloseNativeView((void*)nativeEditorView);
		[nativeEditorView removeFromSuperview];
		[nativeEditorView release];
		nativeEditorView = nil;
	}

	editor = {};

	[_audioUnit release];
	[super dealloc];
}

@end

// Referenced by wrapperAu3.mm, which each consuming appex compiles directly, so
// the linker cannot dead-strip this translation unit out of the static library.
extern "C" int gmpi_au3_forceLinkViewController(void)
{
	return (int)(intptr_t)[GmpiAUViewController class];
}
