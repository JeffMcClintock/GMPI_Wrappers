// The containing app for an AUv3 app extension, iOS — and, since TideSynth
// BACKLOG M9, a HOST for it rather than a shelf to carry it on.
//
// WHY THE APP HOSTS ITS OWN EXTENSION. An iOS AUv3 cannot ship as a bare
// .appex; it must be delivered inside a container app installed from the App
// Store. So the app exists whether or not it does anything, and the only
// question is whether it is useful. Apple's own Xcode AUv3 template hosts the
// extension; most iOS instruments ship standalone-plus-AUv3 from one binary.
// Until now this app was a single text label, which reads to a user as broken.
//
// It also supplies the AU host we have no other way to get: there is NO AUv3
// host in the iOS simulator (GarageBand and AUM are device-only App Store
// builds) and no host harness in any of these repos. So an iOS AU could be
// installed, launched and REGISTERED — all verified — and never once
// INSTANTIATED, which is the same content-blindness that let an empty rack
// ship past `auval` for two days.
//
// OUT-OF-PROCESS BY DEFAULT, IN-PROCESS BEHIND A DEVELOPER TOGGLE, and that is
// a ruling rather than a preference. Real hosts load third-party AUv3
// out-of-process, so a container that loads in-process exercises a path no
// shipping host uses. Concretely it would not reproduce the failure class this
// host exists to catch: the extension process is sandboxed with its OWN
// container — that is why `/tmp` is not `/tmp` and `$HOME` points into
// ~/Library/Containers/… — whereas in-process the plugin inherits the APP's
// sandbox. A resource-resolution bug can therefore pass in-process and still
// fail in GarageBand. In-process earns its place in development (working
// breakpoints, ordinary stderr, no extension-lifecycle weirdness); it simply
// must not be the default.
//
// Compiled into each <Plugin>_AU3App target by gmpi_plugin.cmake's AU3 arm.
//
// NOTE: manual retain/release. This file is not compiled with ARC.

#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include <string>
#include <vector>

#include "../../Standalone/mcp/WavWriter.h"

// ---------------------------------------------------------------------------
// Launch arguments. `xcrun simctl launch <dev> <bundle-id> <args…>` passes
// these through as argv, which is what makes the whole thing scriptable — the
// point of M9, since no other iOS host can be driven from a test run.
//
//   --gmpi-in-process        load in-process (developer toggle; see above)
//   --gmpi-preset <name>     a file in the app's Documents/, applied as the
//                            GMPIPRESET key of fullState
//   --gmpi-render <seconds>  render offline to Documents/render.wav, then stop.
//                            Deliberately NOT "play for N seconds and record":
//                            an offline pull through the render block is
//                            deterministic and does not depend on the
//                            simulator's audio device existing.
// ---------------------------------------------------------------------------
namespace
{

struct Options
{
	bool inProcess = false;
	std::string presetName;
	double renderSeconds = 0.0;
};

Options parseOptions()
{
	Options o;
	NSArray<NSString*>* args = [[NSProcessInfo processInfo] arguments];
	for (NSUInteger i = 1; i < args.count; ++i)
	{
		NSString* a = args[i];
		if ([a isEqualToString:@"--gmpi-in-process"])
			o.inProcess = true;
		else if ([a isEqualToString:@"--gmpi-preset"] && i + 1 < args.count)
			o.presetName = [args[++i] UTF8String];
		else if ([a isEqualToString:@"--gmpi-render"] && i + 1 < args.count)
			o.renderSeconds = [args[++i] doubleValue];
	}
	return o;
}

NSString* documentsPath(NSString* leaf)
{
	NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(
		NSDocumentDirectory, NSUserDomainMask, YES);
	if (!dirs.count)
		return nil;
	return [dirs[0] stringByAppendingPathComponent:leaf];
}

/// OUR OWN extension's component description, read out of the appex we ship.
///
/// Not hard-coded, and not "the first aumu in the registry": the registry holds
/// every AUv3 installed on the device, so picking from it would silently host
/// somebody else's plugin the moment a second one is installed — and the
/// resulting measurement would look perfectly fine. Reading PlugIns/*.appex's
/// own Info.plist is exact and needs no per-plugin configuration.
bool ownComponentDescription(AudioComponentDescription& out, NSString** nameOut)
{
	NSURL* plugins = [[NSBundle mainBundle] builtInPlugInsURL];
	if (!plugins)
		return false;

	NSArray<NSURL*>* entries =
		[[NSFileManager defaultManager] contentsOfDirectoryAtURL:plugins
		                             includingPropertiesForKeys:nil
		                                                options:0
		                                                  error:nil];
	for (NSURL* url in entries)
	{
		if (![url.pathExtension isEqualToString:@"appex"])
			continue;

		NSBundle* appex = [NSBundle bundleWithURL:url];
		NSDictionary* ext = [appex objectForInfoDictionaryKey:@"NSExtension"];
		NSDictionary* attrs = ext[@"NSExtensionAttributes"];
		NSArray* comps = attrs[@"AudioComponents"];
		if (![comps isKindOfClass:[NSArray class]] || !comps.count)
			continue;

		NSDictionary* c = comps[0];
		NSString* type = c[@"type"], *sub = c[@"subtype"], *manu = c[@"manufacturer"];
		if (type.length != 4 || sub.length != 4 || manu.length != 4)
			continue;

		auto fourcc = [](NSString* s) -> OSType {
			const char* p = [s UTF8String];
			return (OSType)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
		};

		memset(&out, 0, sizeof(out));
		out.componentType         = fourcc(type);
		out.componentSubType      = fourcc(sub);
		out.componentManufacturer = fourcc(manu);
		if (nameOut)
			*nameOut = c[@"name"] ?: url.lastPathComponent;
		return true;
	}
	return false;
}

/// Pull `seconds` of audio straight through the AU's render block.
///
/// The render block rather than AVAudioEngine's manual-rendering mode: this is
/// the same pull a host performs, it works identically for an out-of-process
/// unit, and it does not require an audio device — so it runs on a simulator
/// with no output configured, which is exactly where this needs to run.
///
/// A host must read back mData rather than the pointer it passed in: the
/// wrapper may hand back its own storage, and reading our scratch would
/// silently measure zeroes.
bool renderOffline(AUAudioUnit* au, double seconds, double rate, int channels,
                   std::vector<std::vector<float>>& out, OSStatus& status)
{
	const AUAudioFrameCount block = 512;
	const int total = (int)(seconds * rate);

	out.assign((size_t)channels, std::vector<float>((size_t)total, 0.0f));

	const size_t ablBytes = offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * channels;
	std::vector<uint8_t> ablStore(ablBytes, 0);
	AudioBufferList* abl = (AudioBufferList*)ablStore.data();
	abl->mNumberBuffers = (UInt32)channels;

	std::vector<std::vector<float>> scratch((size_t)channels, std::vector<float>(block, 0.0f));

	AURenderBlock render = au.renderBlock;
	if (!render)
	{
		status = kAudioUnitErr_Uninitialized;
		return false;
	}

	AudioTimeStamp ts{};
	ts.mFlags = kAudioTimeStampSampleTimeValid;
	ts.mSampleTime = 0.0;

	int done = 0;
	while (done < total)
	{
		const AUAudioFrameCount frames =
			(AUAudioFrameCount)std::min<int>((int)block, total - done);

		for (int c = 0; c < channels; ++c)
		{
			std::fill(scratch[(size_t)c].begin(), scratch[(size_t)c].end(), 0.0f);
			abl->mBuffers[c].mNumberChannels = 1;
			abl->mBuffers[c].mDataByteSize   = frames * sizeof(float);
			abl->mBuffers[c].mData           = scratch[(size_t)c].data();
		}

		AudioUnitRenderActionFlags flags = 0;
		status = render(&flags, &ts, frames, 0, abl, nil);
		if (status != noErr)
			return false;

		for (int c = 0; c < channels; ++c)
		{
			const float* src = (const float*)abl->mBuffers[c].mData;
			if (src)
				memcpy(out[(size_t)c].data() + done, src, frames * sizeof(float));
		}

		done += frames;
		ts.mSampleTime += frames;
	}

	status = noErr;
	return true;
}

} // namespace

@interface GmpiAU3HostAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, retain) UIWindow* window;
@property (nonatomic, retain) UILabel* status;
@property (nonatomic, retain) AVAudioEngine* engine;
@property (nonatomic, retain) AVAudioUnit* unit;
@end

@implementation GmpiAU3HostAppDelegate

- (void)say:(NSString*)text
{
	// Both channels on purpose: the label is for a person holding the device,
	// and stdout is for the test run that launched this with --gmpi-render and
	// will never see a screen.
	NSLog(@"GMPI-HOST: %@", text);
	if (self.status)
		self.status.text = text;
}

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
	const Options opts = parseOptions();

	NSString* appName = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
	if (!appName)
		appName = @"This app";

	self.window = [[[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds] autorelease];

	UIViewController* vc = [[[UIViewController alloc] init] autorelease];
	vc.view.backgroundColor = [UIColor systemBackgroundColor];

	UILabel* label = [[[UILabel alloc] init] autorelease];
	label.numberOfLines = 0;
	label.textAlignment = NSTextAlignmentCenter;
	label.translatesAutoresizingMaskIntoConstraints = NO;
	label.text = [NSString stringWithFormat:@"%@\n\nstarting…", appName];
	[vc.view addSubview:label];
	self.status = label;

	UILayoutGuide* margins = vc.view.layoutMarginsGuide;
	[NSLayoutConstraint activateConstraints:@[
		[label.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor constant:20],
		[label.trailingAnchor constraintEqualToAnchor:margins.trailingAnchor constant:-20],
		[label.centerYAnchor constraintEqualToAnchor:vc.view.centerYAnchor],
	]];

	self.window.rootViewController = vc;
	[self.window makeKeyAndVisible];

	// Instantiation is asynchronous, and out-of-process it involves launching
	// another process — so the UI is up first and the result lands in the
	// label. A host that blocks its first runloop turn on an XPC round trip is
	// a host that looks hung on a slow device.
	[self hostWithOptions:opts];

	return YES;
}

- (void)hostWithOptions:(Options)opts
{
	AudioComponentDescription desc{};
	NSString* name = nil;
	if (!ownComponentDescription(desc, &name))
	{
		[self say:@"No AudioUnit extension found in PlugIns/. The app was built "
		           "without its .appex, or the assembly step did not run."];
		return;
	}

	// THE IN-PROCESS TOGGLE CANNOT EXIST HERE, and this is measured rather
	// than argued. M9's ruling asked for "in-process behind a developer
	// toggle" and flagged the preconditions as unverified. They are not
	// merely unmet -- the option is not offered at all:
	//
	//   error: 'kAudioComponentInstantiation_LoadInProcess' is unavailable:
	//          not available on iOS
	//
	// iOS loads every AUv3 out of process, full stop. That is the ruling's own
	// DEFAULT and the reason it gave for it (real hosts load out-of-process,
	// and the extension's separate sandbox is the failure class worth
	// reproducing), so nothing is lost here except an escape hatch that was
	// never available. --gmpi-in-process is still ACCEPTED and reported,
	// rather than ignored: a flag that silently does nothing is worse than
	// one that says it cannot.
	const AudioComponentInstantiationOptions loadOptions =
		kAudioComponentInstantiation_LoadOutOfProcess;

	NSString* mode = @"out-of-process";
	if (opts.inProcess)
		mode = @"out-of-process (--gmpi-in-process ignored: iOS has no in-process load)";
	[self say:[NSString stringWithFormat:@"instantiating %@ (%@)…", name, mode]];

	Options captured = opts;
	[AVAudioUnit instantiateWithComponentDescription:desc
	                                         options:loadOptions
	                               completionHandler:^(AVAudioUnit* avUnit, NSError* error)
	{
		if (!avUnit)
		{
			[self say:[NSString stringWithFormat:@"instantiation FAILED: %@",
			           error.localizedDescription ?: @"(no error given)"]];
			return;
		}

		self.unit = avUnit;
		[self applyPresetIfAny:captured to:avUnit.AUAudioUnit];

		if (captured.renderSeconds > 0.0)
			[self renderAndExit:captured unit:avUnit.AUAudioUnit];
		else
			[self startLivePlayback:avUnit mode:mode];
	}];
}

- (void)applyPresetIfAny:(Options)opts to:(AUAudioUnit*)au
{
	if (opts.presetName.empty())
		return;

	NSString* path = documentsPath(@(opts.presetName.c_str()));
	NSString* xml = path ? [NSString stringWithContentsOfFile:path
	                                                 encoding:NSUTF8StringEncoding
	                                                    error:nil]
	                     : nil;
	if (!xml.length)
	{
		[self say:[NSString stringWithFormat:@"preset '%s' not found in Documents/",
		           opts.presetName.c_str()]];
		return;
	}

	// GMPIPRESET is the same key the AU3 wrapper's fullState carries and the
	// same bytes the VST3 chunk holds, so one fixture drives every format.
	au.fullState = @{ @"GMPIPRESET" : xml };
	NSLog(@"GMPI-HOST: preset applied, %lu bytes", (unsigned long)xml.length);
}

- (void)renderAndExit:(Options)opts unit:(AUAudioUnit*)au
{
	const double rate = 48000.0;
	const int channels = 2;

	NSError* err = nil;
	AVAudioFormat* fmt = [[[AVAudioFormat alloc] initStandardFormatWithSampleRate:rate
	                                                                     channels:(AVAudioChannelCount)channels] autorelease];
	if (au.outputBusses.count)
		[au.outputBusses[0] setFormat:fmt error:&err];

	au.maximumFramesToRender = 512;
	if (![au allocateRenderResourcesAndReturnError:&err])
	{
		[self say:[NSString stringWithFormat:@"allocateRenderResources FAILED: %@",
		           err.localizedDescription ?: @"(no error given)"]];
		return;
	}

	std::vector<std::vector<float>> audio;
	OSStatus st = noErr;
	if (!renderOffline(au, opts.renderSeconds, rate, channels, audio, st))
	{
		[self say:[NSString stringWithFormat:@"render FAILED, OSStatus %d", (int)st]];
		return;
	}

	const auto stats = gmpi::standalone::mcp::measure(audio);
	NSString* out = documentsPath(@"render.wav");
	std::string writeError;
	const bool wrote = gmpi::standalone::mcp::writeWav(
		[out UTF8String], audio, (int)rate,
		gmpi::standalone::mcp::WavFormat::Int16, writeError);
	if (!wrote)
		NSLog(@"GMPI-HOST: writeWav failed: %s", writeError.c_str());

	// Printed in one line so a test run can grep it, and stating peak/rms so
	// "the file exists" and "the plugin made a sound" are separable — a
	// silent render writes a perfectly valid WAV.
	NSLog(@"GMPI-HOST: render %@ %.2fs %d ch @%.0f Hz peak=%.4f rms=%.4f -> %@",
	      wrote ? @"OK" : @"WRITE-FAILED", opts.renderSeconds, channels, rate,
	      stats.peak, stats.rms, out);

	[self say:[NSString stringWithFormat:@"rendered %.1fs, peak %.3f\n%@",
	           opts.renderSeconds, stats.peak, out]];
}

- (void)startLivePlayback:(AVAudioUnit*)avUnit mode:(NSString*)mode
{
	NSError* err = nil;
	AVAudioSession* session = [AVAudioSession sharedInstance];
	[session setCategory:AVAudioSessionCategoryPlayback error:&err];
	[session setActive:YES error:&err];

	self.engine = [[[AVAudioEngine alloc] init] autorelease];
	[self.engine attachNode:avUnit];
	[self.engine connect:avUnit to:self.engine.mainMixerNode format:nil];

	if (![self.engine startAndReturnError:&err])
	{
		[self say:[NSString stringWithFormat:@"engine start FAILED: %@",
		           err.localizedDescription ?: @"(no error given)"]];
		return;
	}

	[self say:[NSString stringWithFormat:
		@"%@ is running, hosted %@.\n\nIt is also registered as an Audio Unit, "
		 "so it appears in any AUv3 host on this device.",
		avUnit.name.length ? avUnit.name : @"The Audio Unit", mode]];
}

@end

int main(int argc, char* argv[])
{
	@autoreleasepool
	{
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([GmpiAU3HostAppDelegate class]));
	}
}
