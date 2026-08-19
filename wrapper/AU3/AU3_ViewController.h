#pragma once

/*
  The app extension's principal class: creates the GmpiAudioUnit on request and
  hosts the plugin's GMPI editor in its view.

  One class serves both platforms because AUViewController already is the
  platform split - NSViewController on macOS, UIViewController on iOS - and
  gmpi_ui's createNativeView() presents the same C signature over NSView
  (DrawingFrameMac.mm) and UIView (DrawingFrameIos.mm).

  The consuming plugin names this class in its appex Info.plist:
      NSExtensionPrincipalClass = GmpiAUViewController
  (see appex-Info.plist.in beside this file), and compiles wrapperAu3.mm into
  the appex so the linker keeps these classes.
*/

#import <CoreAudioKit/CoreAudioKit.h>

@class GmpiAudioUnit;

@interface GmpiAUViewController : AUViewController <AUAudioUnitFactory>

@property (nonatomic, readonly) GmpiAudioUnit* audioUnit;

@end
