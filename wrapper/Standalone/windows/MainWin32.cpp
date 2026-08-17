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
#include "../MenuBarView.h"
#include "../SettingsPane.h"
#include "../StandaloneHost.h"
#include "../StandaloneSettings.h"

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

        // --- menus ----------------------------------------------------------
        // The same two menus as the Wayland shell, which are in turn modelled
        // on JUCE's standalone: a File menu that quits and an Options menu that
        // reaches the device settings.
        {
            std::vector<MenuBarView::Menu> menus;

            menus.push_back({ "File", {
                { "Quit", [&window] { window.close(); } },
            } });

            menus.push_back({ "Options", {
                {
                    "Audio/MIDI Settings",
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
                {
                    "Plugin Editor",
                    [&] { layout->showPage(pageEditor); },
                    [&host] { return host.editorDrawingClient() != nullptr; },
                    [&] { return layout->currentPage() == pageEditor; }
                },
                {},   // separator
                { "Quit", [&window] { window.close(); } },
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
                // An empty list means "connect everything readable", which is
                // what makes a fresh install play the moment a keyboard is
                // plugged in. Only a user who has actually visited the settings
                // page gets the narrower list.
                const auto midiInputs = settings.getBool(Settings::keyMidiInputsSet, false)
                                      ? settings.getStringList(Settings::keyMidiInputs)
                                      : std::vector<std::string>{};

                host.startMidi(midiInputs);
            }
        }

        // Device changes the user asked for while we were inside input
        // dispatch, applied from a timer instead. Re-opening an audio device
        // there would mean joining the driver's threads with a click still on
        // the stack.
        Ticker deferredApply([&settingsPane] { settingsPane->pumpDeferred(); });

        exitCode = window.runEventLoop();

        // Stop the audio and MIDI threads before anything they touch goes away.
        // First, and on this thread: close() on either driver returns only once
        // no callback can still be running.
        host.stopMidi();
        host.stopAudio();

        frame.detachClient();
    }

    if (SUCCEEDED(comInit))
        ::CoUninitialize();

    return exitCode;
}
