// The GMPI standalone app on macOS.
//
// The counterpart of windows/MainWin32.cpp and linux/MainWayland.cpp, and
// deliberately the same program: the same AppLayout, the same drawn
// MenuBarView, the same SettingsPane. What differs is only what has to -
// CoreGraphics instead of Direct2D, CoreAudio instead of WASAPI, CoreMIDI
// instead of winmm - and each of those enters through the seam the portable
// half already had.
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
// dragged. What macOS does NOT get for free is preGraphicsRedraw - DrawingFrameWin
// drives that from its own render timer and DrawingFrameCocoa has no equivalent -
// so the ticker below calls it, exactly as the Wayland loop does.

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <functional>
#include <span>
#include <string>

#import <Cocoa/Cocoa.h>

#include "AudioDriverCoreAudio.h"
#include "MidiDriverCoreMidi.h"
#include "ToplevelWindowMac.h"

#include "../AppLayout.h"
#include "../CommandChannel.h"
#include "../MenuBarView.h"
#include "../SettingsPane.h"
#include "../StandaloneHost.h"
#include "../StandaloneSettings.h"

#if GMPI_STANDALONE_COMMAND_CHANNEL
#include "FrameCapture.h"
#include "../mcp/CommandDispatcher.h"
#include "../mcp/IpcServer.h"
#endif

#include "GmpiUiDrawing.h"
#include "helpers/DrawingFactory.h"
#include "helpers/Timer.h"

// Cmd-Q, the Dock's Quit, and "Quit" in the application menu all arrive as
// -terminate:, which calls exit() and would skip every line after the run loop -
// the command channel's stop(), the audio and MIDI threads, the frame detach.
// This delegate redirects all three onto the window's own orderly close.
#define GMPI_STANDALONE_APP_DELEGATE GMPI_STANDALONE_APP_DELEGATE_01

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

    // Cancelled, not deferred: the close above ends the run loop, main() tears
    // everything down in order and the process exits by returning from main.
    // NSTerminateLater would leave AppKit waiting for a reply that never comes.
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

// Anything the user must be told when there is no window to tell them in.
// Written to stderr as well as shown: this app is as often launched from a
// terminal as from the Finder, and a standalone that exits silently because the
// binary contains no plugin is indistinguishable from one that crashed.
void fatal(const std::string& message)
{
    std::fprintf(stderr, "%s\n", message.c_str());

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

// A TimerClient that runs one callback. gmpi::TimerManager is the app's only
// periodic source, and three things need a tick: the command queue, the
// settings page's deferred apply (see SettingsPane::pumpDeferred, which exists
// because re-opening an audio device inside input dispatch would join the
// driver's threads with a click still on the stack), and the layout's
// preGraphicsRedraw.
class Ticker : public gmpi::TimerClient
{
public:
    explicit Ticker(std::function<void()> tick) : tick_(std::move(tick))
    {
        startTimer(16);
    }

    ~Ticker() override
    {
        stopTimer();
    }

    bool onTimer() override
    {
        tick_();
        return true;
    }

private:
    std::function<void()> tick_;
};

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

// SIGTERM/SIGINT must run the normal shutdown, not the default instant kill.
// Ctrl-C in the terminal this was launched from is the ordinary case, and the
// audio device wants closing before the process goes.
//
// A handler may only touch lock-free atomics; the ticker notices the flag and
// closes the window from safe context.
std::atomic<bool> terminationRequested{ false };

void onTerminationSignal(int)
{
    terminationRequested = true;
}

} // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    using namespace gmpi::standalone;

    @autoreleasepool
    {

    // A binary launched from a shell gets no NSApplication and no connection to
    // the window server unless it asks. Regular rather than Accessory: this app
    // has a window and a menu bar and belongs in the Dock and in Cmd-Tab.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    StandaloneHost host;
    if (!host.init())
    {
        fatal("This binary contains no GMPI plugin (MP_GetFactory returned nothing).");
        return 1;
    }

    Settings settings(host.pluginName());
    settings.load();

    installApplicationMenu(host.pluginName());

    // Size the window from the editor, plus the strip the menu bar occupies.
    float editorWidth = 0.0f;
    float editorHeight = 0.0f;
    host.getEditorSize(editorWidth, editorHeight);

    ToplevelWindowMac window;

    if (!window.create(host.pluginName(),
                       static_cast<int>(std::ceil(editorWidth)),
                       static_cast<int>(std::ceil(editorHeight + MenuBarView::kHeight))))
    {
        fatal("Could not create the application window.");
        return 1;
    }

    // A window narrower than the menu bar's titles is not useful.
    window.setMinimumClientSize(320, static_cast<int>(MenuBarView::kHeight) + 80);

    auto* appDelegate = [[GMPI_STANDALONE_APP_DELEGATE alloc] init];
    appDelegate->window = &window;
    [NSApp setDelegate:appDelegate];

    // The menu bar's font.
    //
    // A factory of this app's own, rather than the frame's the way the Windows
    // shell does it: on macOS the frame is an NSView and its
    // gmpi::cocoa::Factory is private to gmpi_ui. That costs nothing, because a
    // Cocoa TextFormat is a CTFont and its metrics - it holds no reference to
    // the factory that minted it and is not bound to a device.
    //
    // "system-ui" is CocoaGfx's name for CTFontCreateUIFontForLanguage, i.e.
    // the actual macOS UI font rather than a family that resembles it. The
    // counterpart of naming Segoe UI on Windows: the difference between a menu
    // bar that looks like the desktop it is on and one that looks like Arial.
    gmpi::drawing::DrawingFactory drawingFactory;

    const std::string_view menuFontFamily{ "system-ui" };
    auto menuFont = drawingFactory.factory().createTextFormat(14.0f, std::span{ &menuFontFamily, 1 });

    host.setAudioDriver(std::make_unique<AudioDriverCoreAudio>());
    if (host.wantsMidiInput())
        host.setMidiDriver(std::make_unique<MidiDriverCoreMidi>());

    // --- the window's contents ----------------------------------------------
    // Refcounted objects, so each is owned by a shared_ptr rather than by the
    // stack: the frame and the layout keep BORROWED pointers (attaching does
    // not addRef), and the layout's children outlive nothing.

    gmpi::shared_ptr<AppLayout> layout(new AppLayout());
    gmpi::shared_ptr<MenuBarView> menuBar(new MenuBarView());
    gmpi::shared_ptr<SettingsPane> settingsPane(new SettingsPane(host, settings));

    menuBar->setFont(gmpi::drawing::AccessPtr::get(menuFont));

    layout->setMenuBarHeight(MenuBarView::kHeight);
    layout->setMenuBar(static_cast<gmpi::api::IDrawingClient*>(menuBar.get()));

    const int pageEditor   = layout->addPage(host.editorDrawingClient());
    const int pageSettings = layout->addPage(static_cast<gmpi::api::IDrawingClient*>(settingsPane.get()));

    // --- menus --------------------------------------------------------------
    // The same two menus as the other two shells, which are in turn modelled on
    // JUCE's standalone: a File menu that quits and an Options menu that reaches
    // the device settings.
    {
        std::vector<MenuBarView::Menu> menus;

        menus.push_back({ "File", {
            { "Quit", [&window] { window.requestClose(); } },
        } });

        menus.push_back({ "Options", {
            {
                "Audio/MIDI Settings",
                [&]
                {
                    // Re-read the device lists on the way in: keyboards and
                    // interfaces come and go while the app runs.
                    settingsPane->reload();
                    layout->showPage(pageSettings);
                },
                {},
                [&] { return layout->currentPage() == pageSettings; }
            },
            {
                "Plugin Editor",
                [&] { layout->showPage(pageEditor); },
                [&host] { return host.editorDrawingClient() != nullptr; },
                [&] { return layout->currentPage() == pageEditor; }
            },
            {},   // separator
            { "Quit", [&window] { window.requestClose(); } },
        } });

        menuBar->setMenus(std::move(menus));
    }

    // Builds the editor frame around the layout. The macOS counterpart of
    // attachClient + setFallbackHost: on this platform the frame IS the view,
    // so it cannot be created before the client it wraps.
    if (!window.attachClient(static_cast<gmpi::api::IDrawingClient*>(layout.get()),
                             host.parameterHost()))
    {
        fatal("Could not create the editor view.");
        return 1;
    }

    // AFTER attaching: the plugin's editor is initialised against a host it now
    // has, and initUi pushes every current parameter value into it.
    host.onEditorAttached();

    [NSApp activateIgnoringOtherApps:YES];

    // On screen and PAINTED before the audio device is touched. Opening a
    // device that has an input raises the microphone permission prompt the
    // first time, and that blocks this thread - which has not reached the run
    // loop yet - until the user answers. Asking for the microphone from behind
    // an empty window frame is how an app gets its permission declined.
    window.paintNow();

    // --- devices ------------------------------------------------------------
    {
        const auto deviceId = settings.getString(
            Settings::keyAudioDevice, AudioDriverCoreAudio::defaultDeviceId());
        const int sampleRate   = settings.getInt(Settings::keySampleRate, 48000);
        const int bufferFrames = settings.getInt(Settings::keyBufferFrames, 512);

        if (!host.startAudio(deviceId, sampleRate, bufferFrames))
        {
            // Not fatal, and not an alert either. A standalone that refuses to
            // open its window because the soundcard is busy has removed the only
            // UI that could pick a different one - so open ON the settings page
            // instead of the plugin's editor, where the failure is named next to
            // the device list that fixes it.
            std::fprintf(stderr, "Audio: %s\n", host.lastError().c_str());

            settingsPane->reload();
            layout->showPage(pageSettings);
        }

        if (host.wantsMidiInput())
        {
            // An empty list means "connect everything readable", which is what
            // makes a fresh install play the moment a keyboard is plugged in.
            // Only a user who has actually visited the settings page gets the
            // narrower list.
            const auto midiInputs = settings.getBool(Settings::keyMidiInputsSet, false)
                                  ? settings.getStringList(Settings::keyMidiInputs)
                                  : std::vector<std::string>{};

            host.startMidi(midiInputs);
        }
    }

#if GMPI_STANDALONE_COMMAND_CHANNEL
    // --- command channel ----------------------------------------------------
    // A unix socket naming this process, so a test harness or an MCP server can
    // drive the very plugin instance the user is looking at. Failing to open it
    // is not fatal: someone launched this app to make a sound with, and a
    // missing debug channel must never be the reason it will not start.
    FrameCapture frameCapture(window);
    gmpi::standalone::mcp::IpcServer ipcServer;
    gmpi::standalone::mcp::AppContext ipcContext;
    {
        ipcContext.host = &host;

        // Pointer coordinates are window-relative, matching the screenshot, so
        // "find the knob in the PNG, then click it" needs no arithmetic. This is
        // what a caller adds to convert a plugin-relative one.
        ipcContext.editorOriginY = MenuBarView::kHeight;

        ipcContext.framePixels = [&frameCapture](bool forceRedraw,
                                                 const uint8_t*& pixels, int& w, int& h, int& stride)
        {
            return frameCapture.capture(forceRedraw, pixels, w, h, stride);
        };

        ipcContext.logicalSize = [&window](float& w, float& h)
        {
            // Points, which is the space pointer coordinates are in. Read from
            // the frame's own view rather than from the window, so this cannot
            // drift from what the editor was arranged at.
            window.logicalSize(w, h);
        };

        // Input enters at the layout, which is what the frame has attached and
        // therefore exactly where a real mouse arrives - so the menu bar, the
        // page switch and the plugin's own widgets all see synthetic events on
        // the same path, with the same capture bookkeeping.
        ipcContext.inputClient = [&layout]() -> gmpi::api::IInputClient*
        {
            gmpi::api::IInputClient* client{};
            layout->queryInterface(&gmpi::api::IInputClient::guid,
                                   reinterpret_cast<void**>(&client));

            // queryInterface addRefs. The layout outlives every command, so the
            // reference is dropped here rather than making each caller own one.
            if (client)
                client->release();

            return client;
        };

        const bool started = ipcServer.start(
            [&ipcContext](const std::string& line)
            {
                return gmpi::standalone::mcp::dispatchCommand(ipcContext, line);
            });

        // Printed rather than silent: it is how you find the socket to point
        // socat at, and its absence is the first thing to check when a client
        // reports no running apps.
        if (started)
            std::fprintf(stderr, "command channel: %s\n", ipcServer.channelName().c_str());
        else
            std::fprintf(stderr, "command channel: unavailable (no writable runtime directory).\n");
    }
#endif

    std::signal(SIGTERM, onTerminationSignal);
    std::signal(SIGINT,  onTerminationSignal);

    // The app's one periodic job list.
    //
    //  * commands from the socket. This is the ONLY point at which they run -
    //    the listener thread never touches the plugin, it just parks here until
    //    we get to it. Drained BEFORE anything else, so a --set-param is queued
    //    for the processor in time for this tick rather than the next.
    //  * preGraphicsRedraw, which lets the visible page service its DSP->GUI
    //    queue once per frame rather than being polled from the audio thread's
    //    side. On Windows the frame's own render timer does this; the Cocoa
    //    frame has no equivalent, so it is here.
    //  * device changes the user asked for while we were inside input dispatch.
    //    Re-opening an audio device there would mean joining the driver's
    //    threads with a click still on the stack.
    Ticker tick([&]
    {
        if (terminationRequested)
        {
            window.requestClose();
            return;
        }

#if GMPI_STANDALONE_COMMAND_CHANNEL
        ipcServer.mainThreadQueue().drain();
#endif

        layout->preGraphicsRedraw();
        settingsPane->pumpDeferred();
    });

    const int exitCode = window.runEventLoop();

#if GMPI_STANDALONE_COMMAND_CHANNEL
    // FIRST, and on this thread: stop() refuses further model access before it
    // joins its threads, so no command can still be reaching for the host, the
    // editor or the drivers that the next lines tear down.
    ipcServer.stop();
#endif

    // Stop the audio and MIDI threads before anything they touch goes away.
    // close() on either driver returns only once no callback can still be
    // running.
    host.stopMidi();
    host.stopAudio();

    [NSApp setDelegate:nil];
    [appDelegate release];

    // Tears the editor frame down, then the window. Safe here and not from a
    // menu handler - see ToplevelWindowMac::requestClose.
    window.close();

    return exitCode;

    } // @autoreleasepool
}
