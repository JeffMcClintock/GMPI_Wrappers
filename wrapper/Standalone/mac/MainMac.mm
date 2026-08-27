// The GMPI standalone app on macOS.
//
// The counterpart of windows/MainWin32.cpp and linux/MainWayland.cpp, and
// deliberately the same program - now literally so. The startup sequence, the
// menus, the tick and the teardown are ONE copy, in ../StandaloneApp.cpp; what
// is left in this file is the answers only this platform can give, in the order
// that file asks for them. CoreGraphics instead of Direct2D, CoreAudio instead
// of WASAPI, CoreMIDI instead of winmm - plus the two things below that are
// genuinely this platform's alone.
//
// TWO MENU BARS, WHICH IS NOT A MISTAKE. macOS gives every app a menu bar at
// the top of the SCREEN, and one that has none is a broken-looking app with no
// Cmd-Q and no Hide. This file installs that, minimally. The File/Options strip
// INSIDE the window is the same drawn MenuBarView the other two shells have,
// and it stays for two reasons: the window's contents are then identical on
// every platform, and the command channel's coordinate space - editorOriginY,
// which every screenshot and every synthetic click is measured against -
// therefore agrees across platforms too. A macOS-only chrome would make a
// cross-platform GUI test compare two different pictures.
//
// Two things this file owns that a plugin never does:
//
//   * the process. The NSApplication, its activation policy, its menu bar and
//     the run loop.
//   * the connection between the plugin and a soundcard, which in a DAW is the
//     host's job and here is ours.
//
// As on Windows there is no explicit timer pump: gmpi::TimerManager has a
// native source here (CFRunLoopTimer on kCFRunLoopCommonModes), so the host's
// parameter pump ticks even while a menu is tracking or a window is being
// dragged. What macOS does NOT get for free is preGraphicsRedraw -
// DrawingFrameWin drives that from its own render timer and DrawingFrameCocoa
// has no equivalent - so the app's tick calls it, exactly as the Wayland loop
// does. That is the one line of backendServices() below where this platform
// parts company with Windows, and it is the one worth checking twice: get it
// wrong and every meter and scope in the plugin quietly stops moving, with no
// error anywhere to find.

#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#import <Cocoa/Cocoa.h>

#include "AudioDriverCoreAudio.h"
#include "MidiDriverCoreMidi.h"
#include "ToplevelWindowMac.h"

#include "../StandaloneApp.h"
#include "backends/GmpiObjCNames.h"

#if GMPI_STANDALONE_COMMAND_CHANNEL
#include "FrameCapture.h"
#endif

#include "GmpiUiDrawing.h"
#include "helpers/DrawingFactory.h"

// Cmd-Q, the Dock's Quit, and "Quit" in the application menu all arrive as
// -terminate:, which calls exit() and would skip every line after the run loop -
// the command channel's stop(), the audio and MIDI threads, the frame detach.
// This delegate redirects all three onto the window's own orderly close.
#define GMPI_STANDALONE_APP_DELEGATE GMPI_OBJC_NAME(GMPI_STANDALONE_APP_DELEGATE_01)

@interface GMPI_STANDALONE_APP_DELEGATE : NSObject <NSApplicationDelegate>
{
@public
    gmpi::standalone::ToplevelWindowMac* window;
}
@end

@implementation GMPI_STANDALONE_APP_DELEGATE

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender
{
    (void)sender;

    if (window)
        window->requestClose();

    // Cancelled, not deferred: the close above ends the run loop,
    // runStandaloneApp tears everything down in order and the process exits by
    // returning from main. NSTerminateLater would leave AppKit waiting for a
    // reply that never comes.
    return NSTerminateCancel;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app
{
    (void)app;
    return YES;
}

@end

namespace
{

using namespace gmpi::standalone;

// The screen menu bar. Minimal on purpose - the app's own menus are the drawn
// strip inside the window, and duplicating them here would give the user two
// places to change the same setting from.
//
// The items are the ones macOS expects every app to have: without Hide and Quit
// at their standard shortcuts the app is not merely plain, it is broken in ways
// people notice immediately.
void installApplicationMenu(const std::string& appName)
{
    NSString* name = [NSString stringWithUTF8String:appName.c_str()];

    NSMenu* menuBar = [[NSMenu alloc] initWithTitle:@""];

    NSMenuItem* appItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    [menuBar addItem:appItem];

    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:name];

    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:name]
                       action:@selector(hide:)
                keyEquivalent:@"h"];

    NSMenuItem* hideOthers = [appMenu addItemWithTitle:@"Hide Others"
                                                action:@selector(hideOtherApplications:)
                                         keyEquivalent:@"h"];
    [hideOthers setKeyEquivalentModifierMask:(NSEventModifierFlagOption | NSEventModifierFlagCommand)];

    [appMenu addItemWithTitle:@"Show All"
                       action:@selector(unhideAllApplications:)
                keyEquivalent:@""];

    [appMenu addItem:[NSMenuItem separatorItem]];

    // -terminate: is intercepted by the app delegate above and turned into an
    // orderly window close, so this is the standard item and not a special one.
    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:name]
                       action:@selector(terminate:)
                keyEquivalent:@"q"];

    [appItem setSubmenu:appMenu];
    [NSApp setMainMenu:menuBar];

    [appMenu release];
    [appItem release];
    [menuBar release];
}

// Everything ../StandaloneApp.cpp needs to know about macOS.
//
// The methods are in the order PlatformShell declares them, which is the order
// they are called in; windows/MainWin32.cpp and linux/MainWayland.cpp carry the
// same list.
class MacShell final : public PlatformShell
{
public:
    MacShell()
    {
        // A binary launched from a shell gets no NSApplication and no connection
        // to the window server unless it asks, and this is the first thing in
        // the process that could ask. Regular rather than Accessory: this app
        // has a window and a menu bar and belongs in the Dock and in Cmd-Tab.
        //
        // The peer of the Windows shell's ProcessInit, minus the destructor half
        // - there is nothing to hand back, because the run loop's own exit is
        // what releases the connection.
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    }

    ~MacShell() override
    {
        // runStandaloneApp returns early on its fatal paths without reaching
        // closeWindow(), so the delegate teardown has to be reachable from here
        // too. It nils what it clears, so running it twice is harmless.
        clearDelegate();
    }

    bool createWindow(const std::string& title, int clientWidthPoints, int clientHeightPoints) override
    {
        // BEFORE the window, so the app has a screen menu bar from the moment it
        // has anything at all: an app that shows a window and only then acquires
        // a Cmd-Q looks broken for exactly as long as that takes.
        installApplicationMenu(title);

        if (!window_.create(title, clientWidthPoints, clientHeightPoints))
            return false;

        appDelegate_ = [[GMPI_STANDALONE_APP_DELEGATE alloc] init];
        appDelegate_->window = &window_;
        [NSApp setDelegate:appDelegate_];

        // The menu bar's font.
        //
        // A factory of this app's own, rather than the frame's the way the
        // Windows shell does it: on macOS the frame is an NSView and its
        // gmpi::cocoa::Factory is private to gmpi_ui. That costs nothing,
        // because a Cocoa TextFormat is a CTFont and its metrics - it holds no
        // reference to the factory that minted it and is not bound to a device.
        //
        // "system-ui" is CocoaGfx's name for CTFontCreateUIFontForLanguage, i.e.
        // the actual macOS UI font rather than a family that resembles it. The
        // counterpart of naming Segoe UI on Windows: the difference between a
        // menu bar that looks like the desktop it is on and one that looks like
        // Arial.
        const std::string_view menuFontFamily{ "system-ui" };
        menuFont_ = drawingFactory_.factory().createTextFormat(14.0f, std::span{ &menuFontFamily, 1 });

        return true;
    }

    void setMinimumClientSize(int widthPoints, int heightPoints) override
    {
        window_.setMinimumClientSize(widthPoints, heightPoints);
    }

    gmpi::drawing::api::ITextFormat* menuFont() override
    {
        return gmpi::drawing::AccessPtr::get(menuFont_);
    }

    bool attachClient(gmpi::api::IDrawingClient* client, gmpi::api::IUnknown* parameterHost) override
    {
        // Builds the editor frame around the layout. Both halves in one call
        // because here they cannot be separated: on this platform the frame IS
        // the view, so it cannot be created before the client it wraps. The
        // other two shells call setFallbackHost and attachClient in turn.
        return window_.attachClient(client, parameterHost);
    }

    void showAndPaint() override
    {
        // On screen and PAINTED before the audio device is touched, and this is
        // the one place in the interface that exists for a single platform.
        // Opening a device that has an input raises the microphone permission
        // prompt the first time, and that blocks this thread - which has not
        // reached the run loop yet - until the user answers. Asking for the
        // microphone from behind an empty window frame is how an app gets its
        // permission declined.
        [NSApp activateIgnoringOtherApps:YES];
        window_.paintNow();
    }

    void requestClose() override
    {
        // Deferred to a later turn of the run loop - see
        // ToplevelWindowMac::requestClose. A File>Quit arrives inside the menu's
        // action handler with the editor's own view on the stack, and tearing
        // that view down underneath itself is a crash Windows does not have.
        window_.requestClose();
    }

    void closeWindow() override
    {
        clearDelegate();

        // Tears the editor frame down, then the window. Safe here and not from a
        // menu handler - see requestClose() above.
        window_.close();
    }

    std::unique_ptr<AudioDriver> createAudioDriver() override
    {
        return std::make_unique<AudioDriverCoreAudio>();
    }

    std::unique_ptr<MidiDriver> createMidiDriver() override
    {
        return std::make_unique<MidiDriverCoreMidi>();
    }

    BackendServices backendServices() const override
    {
        // CFRunLoopTimer on kCFRunLoopCommonModes, so timers tick even while a
        // menu is tracking. But DrawingFrameCocoa has NO render timer - unlike
        // DrawingFrameWin - so nothing calls preGraphicsRedraw unless the app's
        // tick does. The second answer is the whole difference between this
        // platform and Windows, and it is what keeps the plugin's meters and
        // scopes moving.
        return { TimerSource::backendNative, RedrawClientDriver::appTick };
    }

    int runEventLoop(const std::function<void(int elapsedMs)>& onTick) override
    {
        // A Ticker rather than a tick argument, as on Windows and for the same
        // reason: gmpi::TimerManager has a native source here, so the callback
        // becomes a CFRunLoopTimer that [NSApp run] services. Registered on
        // kCFRunLoopCommonModes, so it keeps firing while a menu is tracking.
        Ticker tick(onTick);
        return window_.runEventLoop();
    }

    void reportStatus(const std::string& message) override
    {
        std::fprintf(stderr, "%s\n", message.c_str());
    }

    void showFatalAlert(const std::string& message) override
    {
        // Shown as well as written to stderr by the caller: this app is as often
        // launched from the Finder as from a terminal, and a standalone that
        // exits silently because the binary contains no plugin is
        // indistinguishable from one that crashed.
        @autoreleasepool
        {
            NSAlert* alert = [[NSAlert alloc] init];
            [alert setMessageText:@"GMPI Standalone"];
            [alert setInformativeText:[NSString stringWithUTF8String:message.c_str()]];
            [alert setAlertStyle:NSAlertStyleCritical];
            [alert runModal];
            [alert release];
        }
    }

#if GMPI_STANDALONE_COMMAND_CHANNEL
    bool framePixels(bool forceRedraw,
                     const uint8_t*& pixels, int& width, int& height, int& stride) override
    {
        return capture_.capture(forceRedraw, pixels, width, height, stride);
    }

    // BACKLOG E32 -- the position half.
    //
    // INSIDE this guard because MainWin32.cpp puts its pair inside the same
    // one and PlatformShell declares them there. That placement is WRONG for
    // what these do -- reopening where the user left the window is a shipping
    // feature, not a test affordance -- and it already breaks the build:
    // with the channel OFF, StandaloneApp.cpp calls windowPosition,
    // setWindowPosition and logicalSize unconditionally and does not compile.
    // That is true on origin/main today, before this change, and the third of
    // those is E32's already-merged SIZE half. Filed rather than fixed here:
    // moving the seam out means moving all three shells' overrides with it,
    // and only the mac one can be built on this box.
    //
    // Thin on purpose. The arithmetic and the clamp live in ToplevelWindowMac,
    // next to the only object that can see the screens, and its header says why
    // this shell answers in POINTS where the seam says pixels.
    //
    // NSWindow's own frame autosave was the obvious alternative and is not used:
    // it writes to NSUserDefaults on its own schedule, which would put the
    // position in a different file from the SIZE the portable half already
    // keeps in standalone.conf, and give two mechanisms the chance to restore
    // conflicting halves of one rectangle.
    bool windowPosition(int& xPixels, int& yPixels) const override
    {
        return window_.framePositionTopLeft(xPixels, yPixels);
    }

    bool setWindowPosition(int xPixels, int yPixels) override
    {
        return window_.setFramePositionTopLeft(xPixels, yPixels);
    }

    void logicalSize(float& width, float& height) override
    {
        // Points, which is the space pointer coordinates are in. Read from the
        // frame's own view rather than from the window, so this cannot drift
        // from what the editor was arranged at.
        window_.logicalSize(width, height);
    }

    void canvasSize(int& width, int& height) override
    {
        // The one platform where this is not a convenience: nothing here paints
        // into FrameCapture's bitmap except FrameCapture itself, so before the
        // first screenshot there are no pixels to measure and --info would
        // otherwise have nothing to report. The window's geometry is available
        // the whole time, and the same arithmetic FrameCapture sizes its bitmap
        // with lives in ToplevelWindowMac so the two cannot disagree.
        window_.canvasSize(width, height);
    }
#endif

private:
    // Idempotent, because both closeWindow() and the destructor reach it. Nil
    // first, then release: AppKit's delegate reference is unretained, so
    // releasing while NSApp still points at it leaves a dangling pointer for
    // anything AppKit does on the way out.
    void clearDelegate()
    {
        if (!appDelegate_)
            return;

        [NSApp setDelegate:nil];
        [appDelegate_ release];
        appDelegate_ = nil;
    }

    // Member order is the contract, as it is in the other two shells: window_
    // first so it outlives everything measured against it, and FrameCapture last
    // so its bitmap context goes before the view it was drawn from.
    ToplevelWindowMac              window_;
    gmpi::drawing::DrawingFactory  drawingFactory_;
    gmpi::drawing::TextFormat      menuFont_;
    GMPI_STANDALONE_APP_DELEGATE*  appDelegate_{};
#if GMPI_STANDALONE_COMMAND_CHANNEL
    FrameCapture                   capture_{ window_ };
#endif
};

} // namespace

int main(int argc, char** argv)
{

    @autoreleasepool
    {
        // Inside the pool, so the shell is destroyed before it drains - which
        // is where the app delegate and anything AppKit autoreleased on the way
        // out have to land. Constructed here rather than inside runStandaloneApp
        // so that the NSApplication and the window outlive it.
        MacShell shell;
        return gmpi::standalone::runStandaloneApp(shell, argc, argv);
    }
}
