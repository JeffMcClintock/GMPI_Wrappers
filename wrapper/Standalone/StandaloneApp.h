#pragma once

// The standalone app's startup sequence, written once.
//
// Everything between "there is a process" and "the process is over" lives in
// runStandaloneApp() below, and all three platforms run the SAME one. What
// differs - and it is much less than it looks - each shell supplies through the
// interface above it: create a window, mint the menu bar's font, attach the
// client, make a driver, run a loop, say something to a developer.
//
// This exists because the three mains had drifted. Windows silently ignored
// whether the command channel had started and never said where it had
// published; Windows said nothing when the audio device would not open; the
// teardown ran there in a different order than on macOS. None of those were
// decisions - they were three copies of the same fifteen lines, and a fix
// landing in two of them. There is one copy now, so there is nothing left to
// miss.
//
// The entry point itself is NOT here, and cannot be: a main() inside a static
// archive is never pulled in, which is why this directory's CMakeLists.txt
// hands the three Main files to the EXECUTABLE and this file to the library.
// Each shell's main() constructs its PlatformShell and calls runStandaloneApp.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "AudioMidiDevices.h"
#include "CommandChannel.h"

#include "GmpiApiCommon.h"
#include "GmpiUiDrawing.h"
#include "helpers/NativeUi.h"
#include "helpers/Timer.h"

namespace gmpi
{
namespace standalone
{

// A TimerClient that runs one callback at the app's tick rate.
//
// Portable, and here rather than in a shell, because it was byte-identical in
// windows/MainWin32.cpp and mac/MainMac.mm - the two platforms where
// gmpi::TimerManager has a native source and the event loop therefore has no
// tick of its own to offer. The Wayland loop takes a tick callback instead and
// never constructs one of these.
class Ticker : public gmpi::TimerClient
{
public:
    // The same interval the Wayland loop is given, so the app ticks at one rate
    // on every platform. 16ms rather than 16.66: a slower timer misses frames
    // and a faster one buys nothing.
    static constexpr int kIntervalMs = 16;

    explicit Ticker(std::function<void(int elapsedMs)> tick) : tick_(std::move(tick))
    {
        startTimer(kIntervalMs);
    }

    ~Ticker() override
    {
        stopTimer();
    }

    bool onTimer() override
    {
        // The NOMINAL interval, not a measured one. Only the Wayland shell -
        // which does not use this class - has an elapsed time worth reporting,
        // because only there does anything consume it (TimerManager::pump).
        tick_(kIntervalMs);
        return true;
    }

private:
    std::function<void(int elapsedMs)> tick_;
};

// ---------------------------------------------------------------------------
// What a platform has to supply. Nothing else.
//
// Implemented by exactly one class per shell, defined in that shell's Main file
// and compiled into the EXECUTABLE - so this library never links a window.
//
// The methods are listed in the order runStandaloneApp calls them, which is
// also the order in which each may safely assume the ones above it have run.
// The three shells declare their overrides in this same order, so that reading
// windows/MainWin32.cpp beside linux/MainWayland.cpp and mac/MainMac.mm shows
// one program with three bodies rather than three programs.
// ---------------------------------------------------------------------------
class PlatformShell
{
public:
    // Out of line (StandaloneApp.cpp) deliberately: it pins the vtable to one
    // translation unit, which matters here because the implementations live in
    // the executable and this class lives in the static library.
    virtual ~PlatformShell();

    // --- the window -------------------------------------------------------

    // `title` is the plugin's own name. `clientWidth/Height` are DIPs (points
    // on macOS): the editor's measured size plus the menu bar's strip.
    //
    // A shell must have its menu-bar font ready by the time this returns, and
    // where the font comes from differs enough to be worth stating: Windows
    // mints one from the frame's own DirectWrite factory, so it can only be
    // made AFTER the window; Wayland has to hand the font to the frame before
    // the surface exists, so it is made BEFORE. Hence one call that owns both,
    // and a plain accessor below.
    //
    // False is fatal to the app. A shell with something specific to say - the
    // Wayland one, which can fail for the very particular reason that there is
    // no display - reports it through lastError(); the caller supplies a
    // generic message when that is empty.
    virtual bool createWindow(const std::string& title,
                              int clientWidthDips, int clientHeightDips) = 0;

    // Why the last createWindow() or attachClient() returned false. Empty means
    // "nothing more specific than the caller's own message", which is the
    // ordinary case and therefore the default.
    virtual std::string lastError() const { return {}; }

    // A floor the user cannot drag the window under. On Wayland only the
    // compositor can actually refuse the drag, which is why this is phrased as
    // a request rather than as a clamp.
    virtual void setMinimumClientSize(int widthDips, int heightDips) = 0;

    // The menu bar's font. Valid once createWindow() has returned; owned by the
    // shell, borrowed by MenuBarView for the life of the app.
    virtual gmpi::drawing::api::ITextFormat* menuFont() = 0;

    // Puts the app's root client in the window, and gives the frame the
    // fallback host that answers the plugin's queryInterface for IEditorHost
    // during setHost - without which the first knob drag dereferences null.
    //
    // One call rather than two because on macOS they cannot be separated: there
    // the frame does not exist until it has a client to be built around, so
    // createNativeView takes both at once.
    virtual bool attachClient(gmpi::api::IDrawingClient* client,
                              gmpi::api::IUnknown* parameterHost) = 0;

    // On screen and PAINTED, before the audio device is opened.
    //
    // A real phase of startup rather than a hook, and every platform is asked:
    // macOS answers with [NSApp activateIgnoringOtherApps:] plus a synchronous
    // paint, because opening a device that has an input raises the microphone
    // permission prompt the first time and that BLOCKS this thread - which has
    // not reached the run loop yet - until the user answers. Asking for the
    // microphone from behind an empty window frame is how an app gets its
    // prompt declined. Windows and Wayland answer "the loop does it" (see
    // ToplevelWindow::runEventLoop's ShowWindow, and the first Wayland
    // configure), so their override is empty and says so.
    virtual void showAndPaint() = 0;

    // Ask for the app to shut. What File>Quit calls, and what the tick calls
    // when a termination signal has arrived. Must only END THE LOOP: every
    // shell's implementation is posted or deferred rather than immediate,
    // because the caller is typically a menu action running from a showAsync()
    // completion with the editor's own view on the stack. The teardown belongs
    // to runStandaloneApp, not to a menu.
    virtual void requestClose() = 0;

    // Drops the editor's connection to the window, and the window with it.
    //
    // Called AFTER the command channel and both device threads have stopped, so
    // nothing can still be reaching for the client this releases. The layout
    // and its pages are destroyed by the caller immediately afterwards.
    virtual void closeWindow() = 0;

    // --- devices ----------------------------------------------------------
    // Constructed here because only a shell knows whether "audio" means WASAPI,
    // PipeWire or CoreAudio; owned by StandaloneHost from the moment they are
    // handed over.

    virtual std::unique_ptr<AudioDriver> createAudioDriver() = 0;

    // Only called when the plugin has a MIDI input pin.
    virtual std::unique_ptr<MidiDriver> createMidiDriver() = 0;

    // No defaultAudioDeviceId() here. What an unconfigured app opens is asked
    // of the driver this just made - AudioDriver::defaultDeviceId - because a
    // shell answering for its driver is a second place to get one string right.

    // --- the loop ---------------------------------------------------------

    // What drives gmpi::TimerManager on this platform.
    enum class TimerSource
    {
        // The backend has a native one (SetTimer, CFRunLoopTimer on
        // kCFRunLoopCommonModes), so timers tick on their own - even while a
        // menu is tracking or a window is being dragged.
        backendNative,

        // Nothing but this app's loop can drive it, so the tick pumps it.
        appTick
    };

    // What calls preGraphicsRedraw on the attached client, which is what lets
    // the visible page service its DSP->GUI queue.
    enum class RedrawClientDriver
    {
        // The frame's own render timer - DrawingFrameWin::preGraphicsRedraw.
        // The app's tick must then leave it alone: calling it as well would
        // service the queue twice per frame.
        frameRenderTimer,

        // The frame has none - DrawingFrameCocoa and the Wayland frame both -
        // so the tick is the only thing that can.
        appTick
    };

    // What gmpi_ui's backend on this platform already drives for itself, and
    // therefore what the app's tick must NOT do a second time.
    //
    // Facts about the backend, not choices, and asked as one question so that a
    // shell written next year is asked BOTH of them. No default constructor and
    // no default answers, deliberately: the two are independent - Windows,
    // macOS and Wayland occupy three different combinations of them - so a
    // shell that could supply one and inherit the other would be inheriting a
    // guess dressed as an answer.
    //
    // Two enums rather than two bools for the same reason. A pair of adjacent
    // bools transposes silently, and this is code a human writes by hand once
    // per platform and then nobody reads again for a year.
    //
    // GET redrawClient WRONG ON A PLATFORM WITHOUT A RENDER TIMER AND EVERY
    // METER AND SCOPE SILENTLY FREEZES: nothing services the DSP->GUI queue,
    // the GUI simply stops updating, and there is no error anywhere to find.
    // macOS is the platform that is easy to get wrong, because it shares a
    // native timer source with Windows but not a render timer.
    struct BackendServices
    {
        constexpr BackendServices(TimerSource timers, RedrawClientDriver redraw)
            : timerSource(timers), redrawClient(redraw) {}

        TimerSource        timerSource;
        RedrawClientDriver redrawClient;
    };
    virtual BackendServices backendServices() const = 0;

    // Runs until the window closes; returns the process exit code.
    //
    // `onTick` must be called about every Ticker::kIntervalMs with the elapsed
    // time. On the two platforms with a native timer source that means wrapping
    // it in a Ticker; on Wayland it is the loop's own tick argument, unchanged.
    virtual int runEventLoop(const std::function<void(int elapsedMs)>& onTick) = 0;

    // --- diagnostics ------------------------------------------------------

    // Something a developer wants and the app does not stop for: where the
    // command channel published, why the audio device would not open.
    //
    // A virtual rather than an fprintf because "stderr" is not a thing a
    // GUI-subsystem Windows app reliably has - which is exactly how that shell
    // came to print neither of those. Every shell must put this SOMEWHERE
    // findable.
    virtual void reportStatus(const std::string& message) = 0;

    // Something the app cannot start without. The caller returns 1 immediately
    // afterwards, so this is the last thing the user sees: it has to be visible
    // with no terminal open.
    //
    // NOT virtual, because all three shells opened it by doing exactly what
    // reportStatus does and the fatal message must reach the developer's log
    // whether or not the platform can also raise an alert. That half is here;
    // the alert is the hook below.
    void reportFatal(const std::string& message);

    // The platform's own way of saying it to a user who has no terminal open:
    // a message box on Windows, an NSAlert on macOS. Must BLOCK until the user
    // has seen it - the process exits as soon as it returns.
    //
    // The default is to add nothing, and Wayland takes it: every path that
    // reaches this runs before there is a window, and the two dialogs available
    // before then - a libdecor message box, a portal - need the very display
    // connection whose absence is the usual reason for being here. So there the
    // stderr line above is the whole report, and there is no override.
    virtual void showFatalAlert(const std::string& message);

    // --- the command channel ----------------------------------------------
    // Behind the switch for the same reason the shells' FrameCapture members
    // are: with the channel off, mcp/ and the capture code are not compiled at
    // all, so there is nothing left for these to call.
#if GMPI_STANDALONE_COMMAND_CHANNEL

    // Semantics of mcp::AppContext::framePixels. BGRX8888 on every platform.
    virtual bool framePixels(bool forceRedraw,
                             const uint8_t*& pixels, int& width, int& height, int& stride) = 0;

    // The window in DIPs - the space pointer coordinates are in, which under
    // fractional scaling is not the pixel size above.
    virtual void logicalSize(float& width, float& height) = 0;

#endif
};

// The whole app. Returns the process exit code; reports its own fatal errors
// through the shell before returning 1.
int runStandaloneApp(PlatformShell& shell);

} // namespace standalone
} // namespace gmpi
