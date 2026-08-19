// The containing app for an AUv3 app extension, iOS - the UIKit sibling of
// mac/HostAppMain.mm, with the same one job: carrying the .appex in PlugIns/
// so the system registers it. Opening the app once is what makes the plugin
// appear in AUv3 hosts (GarageBand, AUM, Logic for iPad), so the single
// screen says exactly that.
//
// Compiled into each <Plugin>_AU3App target by gmpi_plugin.cmake's AU3 arm.

#import <UIKit/UIKit.h>

@interface GmpiAU3HostAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, retain) UIWindow* window;
@end

@implementation GmpiAU3HostAppDelegate

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
	NSString* appName = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
	if (!appName)
		appName = @"This app";

	self.window = [[[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds] autorelease];

	UIViewController* vc = [[[UIViewController alloc] init] autorelease];
	vc.view.backgroundColor = [UIColor systemBackgroundColor];

	UILabel* label = [[[UILabel alloc] init] autorelease];
	label.text = [NSString stringWithFormat:
		@"%@ contains an Audio Unit extension.\n\n"
		@"Now that it has been opened once, the plugin is registered: it "
		@"appears in any AUv3 host (GarageBand, AUM, Logic, ...) under its "
		@"own name.",
		appName];
	label.numberOfLines = 0;
	label.textAlignment = NSTextAlignmentCenter;
	label.translatesAutoresizingMaskIntoConstraints = NO;
	[vc.view addSubview:label];

	UILayoutGuide* margins = vc.view.layoutMarginsGuide;
	[NSLayoutConstraint activateConstraints:@[
		[label.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor constant:20],
		[label.trailingAnchor constraintEqualToAnchor:margins.trailingAnchor constant:-20],
		[label.centerYAnchor constraintEqualToAnchor:vc.view.centerYAnchor],
	]];

	self.window.rootViewController = vc;
	[self.window makeKeyAndVisible];

	return YES;
}

@end

int main(int argc, char* argv[])
{
	@autoreleasepool
	{
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([GmpiAU3HostAppDelegate class]));
	}
}
