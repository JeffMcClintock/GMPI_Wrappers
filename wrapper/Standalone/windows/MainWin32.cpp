// The GMPI standalone app on Windows.
//
// The counterpart of linux/MainWayland.cpp, and deliberately the same program:
// the same AppLayout, the same drawn MenuBarView, the same SettingsPane. What
// differs is only what has to - Direct2D instead of the CPU renderer, WASAPI
// instead of PipeWire, winmm instead of ALSA - and each of those enters through
// the seam the portable half already had.
//
// The menu bar is DRAWN rather than native, which is the one visible choice
// worth defending. A native HMENU would look more like Windows, but it would
// also mean the app's chrome differed per platform in a way the plugin's editor
// never does, and MenuBarView's drop-downs already open through IDialogHost -
// which on Windows is TrackPopupMenu, so the menus themselves ARE native. What
// is drawn is the strip of titles, and it is 26 DIPs of it.
//
// Two things this file owns that a plugin never does:
//
//   * the process. DPI awareness, COM, and the message loop.
//   * the connection between the plugin and a soundcard, which in a DAW is the
//     host's job and here is ours.
//
// Unlike the Wayland shell there is no explicit timer pump: gmpi::TimerManager
// has a native source here (SetTimer), so the frame's redraw tick and
// StandaloneHost's parameter pump both arrive as messages the loop dispatches.

#include <cmath>
#include <functional>
#include <string>

#include <windows.h>

#include "AudioDriverWasapi.h"
#include "MidiDriverWin.h"
#include "ToplevelWindow.h"

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
#include "helpers/Timer.h"
#include "helpers/unicode_conversion.h"

namespace
{

// Anything the user must be told when there is no window to tell them in.
// A GUI-subsystem app has no stderr worth writing to, and a standalone that
// exits silently because the binary contains no plugin is indistinguishable
// from one that crashed.
void fatal(const std::string& message)
{
    ::MessageBoxW(nullptr,
                  gmpi::unicode::to_wide(message).c_str(),
                  L"GMPI Standalone",
                  MB_OK | MB_ICONERROR);
}

// A TimerClient that runs one callback. gmpi::TimerManager is the app's only
// periodic source, and the settings page needs a tick of its own - see
// SettingsPane::pumpDeferred, which exists because re-opening an audio device
// inside input dispatch would join the driver's threads with a click still on
// the stack.
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

} // namespace

int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    using namespace gmpi::standalone;

    // Before any window exists, and before anything asks for a DPI. V2 rather
    // than plain per-monitor: it is what makes the non-client area (the
    // caption, the resize border) scale with the rest when the window is
    // dragged to another monitor, which plain per-monitor leaves at the DPI the
    // window was created at.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // STA, because this thread owns windows: OLE drag-drop, the common file
    // dialog and the colour picker all require it, and gmpi_ui's frame calls
    // OleInitialize when it registers its window class. The audio driver's
    // threads initialise their own MTA and never touch an object created here.
    const HRESULT comInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int exitCode = 1;

    // Scoped so that everything below is destroyed before CoUninitialize:
    // releasing a COM object after the apartment has gone is a crash on exit
    // that looks like it came from whatever ran last.
    {
        StandaloneHost host;
        if (!host.init())
        {
            fatal("This binary contains no GMPI plugin (MP_GetFactory returned nothing).");
            if (SUCCEEDED(comInit))
                ::CoUninitialize();
            return 1;
        }

        Settings settings(host.pluginName());
        settings.load();

        // Size the window from the editor, plus the strip the menu bar occupies.
        float editorWidth = 0.0f;
        float editorHeight = 0.0f;
        host.getEditorSize(editorWidth, editorHeight);

        ToplevelWindow window;

        if (!window.create(host.pluginName(),
                           static_cast<int>(std::ceil(editorWidth)),
                           static_cast<int>(std::ceil(editorHeight + MenuBarView::kHeight))))
        {
            fatal("Could not create the application window.");
            if (SUCCEEDED(comInit))
                ::CoUninitialize();
            return 1;
        }

        // A window narrower than the menu bar's titles is not useful.
        window.setMinimumClientSize(320, static_cast<int>(MenuBarView::kHeight) + 80);

        auto& frame = window.frame();

        // The plugin's parameter pins take their host from a queryInterface for
        // IEditorHost during setHost. The frame answers the drawing, input and
        // dialog interfaces itself and forwards the rest here; without this the
        // first knob drag dereferences null.
        frame.setFallbackHost(host.parameterHost());

        // The menu bar's font. No text stack to wire up, unlike Wayland: the
        // Direct2D factory the frame already owns has DirectWrite behind it, so
        // this is a wrapper around the factory that is there rather than a
        // fontconfig/freetype/harfbuzz assembly.
        //
        // Segoe UI, then the factory's own fallback. Naming the desktop's UI
        // font is the difference between a menu bar that looks like Windows and
        // one that looks like Arial.
        gmpi::drawing::Factory factory;
        *gmpi::drawing::AccessPtr::put(factory) = &frame.DrawingFactory;

        const std::string_view menuFontFamily{ "Segoe UI" };
        auto menuFont = factory.createTextFormat(14.0f, std::span{ &menuFontFamily, 1 });

        host.setAudioDriver(std::make_unique<AudioDriverWasapi>());
        if (host.wantsMidiInput())
            host.setMidiDriver(std::make_unique<MidiDriverWin>());

        // --- the window's contents ------------------------------------------
        // Refcounted objects, so each is owned by a shared_ptr rather than by
        // the stack: the frame and the layout keep BORROWED pointers
        // (attachClient does not addRef), and the layout's children outlive
        // nothing.

        gmpi::shared_ptr<AppLayout> layout(new AppLayout());
        gmpi::shared_ptr<MenuBarView> menuBar(new MenuBarView());
        gmpi::shared_ptr<SettingsPane> settingsPane(new SettingsPane(host, settings));

        menuBar->setFont(gmpi::drawing::AccessPtr::get(menuFont));

        layout->setMenuBarHeight(MenuBarView::kHeight);
        layout->setMenuBar(static_cast<gmpi::api::IDrawingClient*>(menuBar.get()));

        const int pageEditor   = layout->addPage(host.editorDrawingClient());
        const int pageSettings = layout->addPage(static_cast<gmpi::api::IDrawingClient*>(settingsPane.get()));

        // The way OUT of the settings page. A menu opens it; its own Close
        // button dismisses it, the way a settings screen anywhere else does.
        // Only offered when there is a plugin editor to go back to.
        if (host.editorDrawingClient())
            settingsPane->setOnClose([&] { layout->showPage(pageEditor); });

        // --- menus ----------------------------------------------------------
        // The same two menus as the Wayland shell, which are in turn modelled
        // on JUCE's standalone: a File menu that quits and an Options menu that
        // reaches the device settings.
        {
            std::vector<MenuBarView::Menu> menus;

            menus.push_back({ "File", {
                { "Quit", [&window] { window.requestClose(); } },
            } });

            menus.push_back({ "Options", {
                {
                    // No "Plugin Editor" item beside it. Two items that switch
                    // between two pages is a radio pair, and nobody looks in a
                    // menu for the way out of a settings screen - they look for
                    // a button on the screen itself, which is where it now is.
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
                { "Quit", [&window] { window.requestClose(); } },
            } });

            menuBar->setMenus(std::move(menus));
        }

        frame.attachClient(static_cast<gmpi::api::IDrawingClient*>(layout.get()));

        // AFTER attachClient: the plugin's editor is initialised against a host
        // it now has, and initUi pushes every current parameter value into it.
        host.onEditorAttached();

        // --- devices --------------------------------------------------------
        {
            const auto deviceId = settings.getString(
                Settings::keyAudioDevice, AudioDriverWasapi::defaultDeviceId());
            const int sampleRate   = settings.getInt(Settings::keySampleRate, 48000);
            const int bufferFrames = settings.getInt(Settings::keyBufferFrames, 512);

            if (!host.startAudio(deviceId, sampleRate, bufferFrames))
            {
                // Not fatal, and not a message box either. A standalone that
                // refuses to open its window because the soundcard is busy has
                // removed the only UI that could pick a different one - so open
                // ON the settings page instead of the plugin's editor, where
                // the failure is named next to the device list that fixes it.
                settingsPane->reload();
                layout->showPage(pageSettings);
            }

            if (host.wantsMidiInput())
            {
                // The flag and the list are resolved together, in one place all
                // three shells share. With nothing saved, every readable input
                // is connected, so a fresh install plays the moment a keyboard
                // is plugged in. Only actually ticking or unticking an input
                // saves a list - opening the settings page and closing it again
                // does not - and from then on that list is honoured exactly,
                // including an empty one, which means no MIDI input at all
                // rather than all of them.
                host.startMidi(MidiInputSelection::saved(
                    settings.getBool(Settings::keyMidiInputsSet, false),
                    settings.getStringList(Settings::keyMidiInputs)));
            }
        }

#if GMPI_STANDALONE_COMMAND_CHANNEL
        // --- command channel ------------------------------------------------
        // A named pipe naming this process, so a test harness or an MCP server
        // can drive the very plugin instance the user is looking at. Failing to
        // open it is not fatal: someone launched this app to make a sound with,
        // and a missing debug channel must never be the reason it will not
        // start.
        FrameCapture frameCapture(frame);
        gmpi::standalone::mcp::IpcServer ipcServer;
        gmpi::standalone::mcp::AppContext ipcContext;
        {
            ipcContext.host = &host;

            // Pointer coordinates are window-relative, matching the screenshot,
            // so "find the knob in the PNG, then click it" needs no arithmetic.
            // This is what a caller adds to convert a plugin-relative one.
            ipcContext.editorOriginY = MenuBarView::kHeight;

            ipcContext.framePixels = [&frameCapture](bool forceRedraw,
                                                     const uint8_t*& pixels, int& w, int& h, int& stride)
            {
                return frameCapture.capture(forceRedraw, pixels, w, h, stride);
            };

            ipcContext.logicalSize = [&window](float& w, float& h)
            {
                // DIPs, which is the space pointer coordinates are in. Read
                // from the frame's own transform rather than from GetClientRect
                // so this cannot drift from what the editor was arranged at.
                RECT client{};
                ::GetClientRect(window.hwnd(), &client);

                const float scale = window.frame().getRasterizationScale();
                w = scale > 0.0f ? client.right  / scale : static_cast<float>(client.right);
                h = scale > 0.0f ? client.bottom / scale : static_cast<float>(client.bottom);
            };

            // Input enters at the layout, which is what the frame has attached
            // and therefore exactly where a real mouse arrives - so the menu
            // bar, the page switch and the plugin's own widgets all see
            // synthetic events on the same path, with the same capture
            // bookkeeping.
            ipcContext.inputClient = [&layout]() -> gmpi::api::IInputClient*
            {
                gmpi::api::IInputClient* client{};
                layout->queryInterface(&gmpi::api::IInputClient::guid,
                                       reinterpret_cast<void**>(&client));

                // queryInterface addRefs. The layout outlives every command, so
                // the reference is dropped here rather than making each caller
                // own one.
                if (client)
                    client->release();

                return client;
            };

            ipcServer.start(
                [&ipcContext](const std::string& line)
                {
                    return gmpi::standalone::mcp::dispatchCommand(ipcContext, line);
                });
        }
#endif

        // The app's one periodic job list. Two entries:
        //
        //  * commands from the pipe. This is the ONLY point at which they run -
        //    the listener thread never touches the plugin, it just parks here
        //    until we get to it. Drained BEFORE the settings apply and before
        //    anything else, so a --set-param is queued for the processor in
        //    time for this tick rather than the next.
        //  * device changes the user asked for while we were inside input
        //    dispatch. Re-opening an audio device there would mean joining the
        //    driver's threads with a click still on the stack.
        Ticker tick([&]
        {
#if GMPI_STANDALONE_COMMAND_CHANNEL
            ipcServer.mainThreadQueue().drain();
#endif
            settingsPane->pumpDeferred();
        });

        exitCode = window.runEventLoop();

#if GMPI_STANDALONE_COMMAND_CHANNEL
        // FIRST, and on this thread: stop() refuses further model access before
        // it joins its threads, so no command can still be reaching for the
        // host, the editor or the drivers that the next lines tear down.
        ipcServer.stop();
#endif

        // Stop the audio and MIDI threads before anything they touch goes away.
        // close() on either driver returns only once no callback can still be
        // running.
        host.stopMidi();
        host.stopAudio();

        frame.detachClient();

        // Only now is the window destroyed. WM_CLOSE only asked for it, so that
        // everything above ran while the frame was still whole.
        window.close();
    }

    if (SUCCEEDED(comInit))
        ::CoUninitialize();

    return exitCode;
}
