#include "StandaloneApp.h"

#include <atomic>
#include <cmath>
#include <csignal>
#include <string>
#include <vector>

#include "AppLayout.h"
#include "MenuBarView.h"
#include "SettingsPane.h"
#include "StandaloneHost.h"
#include "StandaloneSettings.h"

#include "mcp/CommandChannelHost.h"

namespace gmpi
{
namespace standalone
{

PlatformShell::~PlatformShell() = default;

void PlatformShell::reportFatal(const std::string& message)
{
    reportStatus(message);
    showFatalAlert(message);
}

void PlatformShell::showFatalAlert(const std::string&)
{
}

namespace
{

// SIGTERM/SIGINT must run the normal shutdown, not the default instant kill.
// The session manager sends SIGTERM at logout and some compositors crash when a
// client vanishes without disconnecting - the workaround roundtrip lives in
// ~Connection, which a default-handled signal never reaches. Ctrl-C in the
// terminal the app was launched from is the ordinary case on both unixes.
//
// Windows raises neither at a GUI app, and MSVC documents SIGINT as unsupported
// there, so the handlers are not installed on that platform at all. Its real
// analogue is session end, and it lives where it belongs -
// WM_QUERYENDSESSION/WM_ENDSESSION in windows/ToplevelWindow.cpp, which routes
// to the same requestClose() the tick below calls. The FLAG and the tick's
// check of it stay portable: one tick, one shape, on all three.
//
// A handler may only touch lock-free atomics; the tick notices the flag and
// closes the window from safe context.
std::atomic<bool> terminationRequested{ false };

#if !defined(_WIN32)
void onTerminationSignal(int)
{
    terminationRequested = true;
}
#endif

} // namespace

int runStandaloneApp(PlatformShell& shell)
{
    StandaloneHost host;
    if (!host.init())
    {
        shell.reportFatal("This binary contains no GMPI plugin (MP_GetFactory returned nothing).");
        return 1;
    }

    Settings settings(host.pluginName());
    settings.load();

    // Size the window from the editor, plus the strip the menu bar occupies.
    float editorWidth = 0.0f;
    float editorHeight = 0.0f;
    host.getEditorSize(editorWidth, editorHeight);

    if (!shell.createWindow(host.pluginName(),
                            static_cast<int>(std::ceil(editorWidth)),
                            static_cast<int>(std::ceil(editorHeight + MenuBarView::kHeight))))
    {
        const auto why = shell.lastError();
        shell.reportFatal(why.empty() ? "Could not create the application window." : why);
        return 1;
    }

    // A window narrower than the menu bar's titles is not useful.
    shell.setMinimumClientSize(320, static_cast<int>(MenuBarView::kHeight) + 80);

    host.setAudioDriver(shell.createAudioDriver());
    if (host.wantsMidiInput())
        host.setMidiDriver(shell.createMidiDriver());

    // --- the window's contents ----------------------------------------------
    // Refcounted objects, so each is owned by a shared_ptr rather than by this
    // stack frame.
    //
    // All three shells used to claim here that the frame and the layout kept
    // BORROWED pointers because "attachClient does not addRef", and that was
    // wrong twice over: AppLayout::attach assigns every child into a
    // gmpi::shared_ptr, and so does the frame on two platforms of three
    // (DxDrawingFrameBase::attachClient and DrawingFrameCommon both
    // queryInterface into shared_ptr members - only the Wayland frame really
    // does borrow, from a raw client_).
    //
    // It matters now, because the shell owns the frame and the shell outlives
    // this function. What makes that safe in either direction is that
    // closeWindow() is the LAST teardown step and is called explicitly rather
    // than left to the shell's destructor: it drops the frame's references -
    // or clears Wayland's borrowed pointer - while the layout is still alive,
    // and these shared_ptrs are released immediately afterwards as this frame
    // unwinds.
    gmpi::shared_ptr<AppLayout> layout(new AppLayout());
    gmpi::shared_ptr<MenuBarView> menuBar(new MenuBarView());
    gmpi::shared_ptr<SettingsPane> settingsPane(new SettingsPane(host, settings));

    menuBar->setFont(shell.menuFont());

    layout->setMenuBarHeight(MenuBarView::kHeight);
    layout->setMenuBar(static_cast<gmpi::api::IDrawingClient*>(menuBar.get()));

    const int pageEditor   = layout->addPage(host.editorDrawingClient());
    const int pageSettings = layout->addPage(static_cast<gmpi::api::IDrawingClient*>(settingsPane.get()));

    // The way OUT of the settings page. A menu opens it; its own Close button
    // dismisses it, the way a settings screen anywhere else does. Only offered
    // when there is a plugin editor to go back to.
    if (host.editorDrawingClient())
        settingsPane->setOnClose([&] { layout->showPage(pageEditor); });

    // --- menus ---------------------------------------------------------------
    // Modelled on JUCE's standalone shell, which is what anyone reaching for
    // this has used before: a File menu that quits and an Options menu that
    // reaches the device settings.
    {
        std::vector<MenuBarView::Menu> menus;

        menus.push_back({ "File", {
            { "Quit", [&shell] { shell.requestClose(); } },
        } });

        menus.push_back({ "Options", {
            {
                // No "Plugin Editor" item beside it. Two items that switch
                // between two pages is a radio pair, and nobody looks in a menu
                // for the way out of a settings screen - they look for a button
                // on the screen itself, which is where it now is.
                "Audio/MIDI Settings...",
                [&]
                {
                    // Re-read the device lists on the way in: keyboards and
                    // endpoints come and go while the app runs.
                    settingsPane->reload();
                    layout->showPage(pageSettings);
                },
                {},
                [&] { return layout->currentPage() == pageSettings; }
            },
            {},   // separator
            { "Quit", [&shell] { shell.requestClose(); } },
        } });

        menuBar->setMenus(std::move(menus));
    }

    if (!shell.attachClient(static_cast<gmpi::api::IDrawingClient*>(layout.get()),
                            host.parameterHost()))
    {
        const auto why = shell.lastError();
        shell.reportFatal(why.empty() ? "Could not create the editor view." : why);
        return 1;
    }

    // AFTER attaching: the plugin's editor is initialised against a host it now
    // has, and initUi pushes every current parameter value into it.
    host.onEditorAttached();

    // BEFORE the device is opened, and macOS is why - see
    // PlatformShell::showAndPaint.
    shell.showAndPaint();

    // --- devices -------------------------------------------------------------
    {
        // The driver names its own sentinel, so the fallback here always agrees
        // with what open() will test for. Null only if a shell chose to build
        // without audio at all, which startAudio below reports.
        const auto* audio = host.audioDriver();

        const auto deviceId = settings.getString(
            Settings::keyAudioDevice, audio ? audio->defaultDeviceId() : "");
        const int sampleRate   = settings.getInt(Settings::keySampleRate, 48000);
        const int bufferFrames = settings.getInt(Settings::keyBufferFrames, 512);

        if (!host.startAudio(deviceId, sampleRate, bufferFrames))
        {
            // Not fatal, and not a message box either. A standalone that refuses
            // to open its window because the soundcard is busy has removed the
            // only UI that could pick a different one - so say why for whoever
            // is reading a log, then open ON the settings page instead of the
            // plugin's editor, where the failure is named next to the device
            // list that fixes it.
            shell.reportStatus("Audio: " + host.lastError());

            settingsPane->reload();
            layout->showPage(pageSettings);
        }

        if (host.wantsMidiInput())
        {
            // The flag and the list are resolved together, in one place all
            // three shells share. With nothing saved, every readable input is
            // connected, so a fresh install plays the moment a keyboard is
            // plugged in. Only actually ticking or unticking an input saves a
            // list - opening the settings page and closing it again does not -
            // and from then on that list is honoured exactly, including an empty
            // one, which means no MIDI input at all rather than all of them.
            if (!host.startMidi(MidiInputSelection::saved(
                    settings.getBool(Settings::keyMidiInputsSet, false),
                    settings.getStringList(Settings::keyMidiInputs))))
            {
                // Said, but not acted on - no page switch, unlike the audio
                // failure above. What is left is an app that still runs and
                // still makes a sound, just without the keyboard.
                //
                // By MidiOpenTally's rule this fires only when something was
                // asked for and none of it arrived: a saved selection whose
                // devices have been unplugged or are held by another program,
                // or - with nothing saved, where the request is "everything
                // readable" - every readable input on the machine refusing at
                // once. Nothing saved and nothing plugged in is a success and
                // stays quiet, on all three platforms, whether or not the
                // platform lists a software port of its own.
                shell.reportStatus("MIDI: " + host.midiError());
            }
        }
    }

    // --- command channel -----------------------------------------------------
    // An IPC endpoint naming this process, so a test harness or an MCP server
    // can drive the very plugin instance the user is looking at. A named pipe on
    // Windows, a unix socket on both the others; none of that is visible here,
    // and neither is whether this build has a channel at all - see
    // mcp/CommandChannelHost.h.
    mcp::CommandChannelHost commandChannel;
    commandChannel.start(host, *layout, shell);

#if !defined(_WIN32)
    std::signal(SIGTERM, onTerminationSignal);
    std::signal(SIGINT,  onTerminationSignal);
#endif

    const auto services = shell.backendServices();

    const int exitCode = shell.runEventLoop([&](int elapsedMs)
    {
        if (terminationRequested.load(std::memory_order_relaxed))
        {
            shell.requestClose();
            return;
        }

        // Commands from the endpoint, BEFORE the timer pump rather than after:
        // a --set-param queues the value for the processor and the pump is what
        // delivers it, so draining first means a parameter set now is audible
        // this tick rather than the next one.
        commandChannel.drain();

        // Only where gmpi_ui has no native timer source. This is what drives the
        // host's parameter queues and the form widgets' animation.
        if (services.timerSource == PlatformShell::TimerSource::appTick)
            gmpi::TimerManager::instance()->pump(elapsedMs);

        // Lets the visible page service its DSP->GUI queue once per frame,
        // rather than polling it from the audio thread's side. Skipped where the
        // frame's own render timer already does it, which would otherwise
        // service the queue twice per frame.
        if (services.redrawClient == PlatformShell::RedrawClientDriver::appTick)
            layout->preGraphicsRedraw();

        // Device changes the user asked for while we were inside input dispatch.
        // Re-opening an audio device there would mean joining the driver's
        // threads with a click still on the stack.
        settingsPane->pumpDeferred();
    });

    // --- teardown ------------------------------------------------------------
    // FIRST, and on this thread: stop() refuses further model access before it
    // joins its threads, so no command can still be reaching for the host, the
    // editor or the drivers the next lines tear down.
    commandChannel.stop();

    // Then the audio and MIDI threads, before anything they touch goes away.
    // close() on either driver returns only once no callback can still be
    // running.
    host.stopMidi();
    host.stopAudio();

    // Then the window, which is what releases the editor. The three shells used
    // to disagree about this line: Windows destroyed the frame from inside
    // WM_CLOSE, with both those threads still running, and then detached a
    // client from a frame that had already gone.
    shell.closeWindow();

    // settingsPane, menuBar and layout are released as this frame unwinds, then
    // settings, then host - which keeps SettingsPane's references to the last
    // two valid for the whole of its life.
    return exitCode;
}

} // namespace standalone
} // namespace gmpi
