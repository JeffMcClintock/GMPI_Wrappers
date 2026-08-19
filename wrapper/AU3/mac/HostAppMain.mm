// The containing app for an AUv3 app extension, macOS.
//
// An .appex cannot exist on its own: the system discovers it inside an app's
// PlugIns folder, and registration happens when LaunchServices first sees the
// app (or via `pluginkit -a` during development). This app is that container,
// and nothing more - one window explaining itself, so a user who double-clicks
// it learns why it exists instead of watching nothing happen.
//
// Compiled into each <Plugin>_AU3App target by gmpi_plugin.cmake's AU3 arm,
// the way Standalone/mac/MainMac.mm is compiled into each standalone app.

#import <Cocoa/Cocoa.h>

@interface GmpiAU3HostAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation GmpiAU3HostAppDelegate
{
	NSWindow* window;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
	NSString* appName = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
	if (!appName)
		appName = @"This app";

	window = [[NSWindow alloc]
		initWithContentRect:NSMakeRect(0, 0, 480, 140)
				  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
					backing:NSBackingStoreBuffered
					  defer:NO];
	window.title = appName;

	NSTextField* label = [NSTextField wrappingLabelWithString:
		[NSString stringWithFormat:
			@"%@ contains an Audio Unit extension.\n\n"
			@"Now that it has been opened once, the plugin is registered with "
			@"macOS: it appears in any AUv3 host (Logic Pro, GarageBand, ...) "
			@"under its own name. This window can be closed.",
			appName]];
	label.frame = NSInsetRect([window.contentView bounds], 20, 20);
	label.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	[window.contentView addSubview:label];

	[window center];
	[window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
	return YES;
}

@end

int main(int argc, const char* argv[])
{
	@autoreleasepool
	{
		NSApplication* app = [NSApplication sharedApplication];
		GmpiAU3HostAppDelegate* delegate = [[GmpiAU3HostAppDelegate alloc] init];
		app.delegate = delegate;
		[app run];
	}
	return 0;
}
