// The GMPI standalone app on Windows.
//
// The counterpart of linux/MainWayland.cpp and mac/MainMac.mm, and deliberately
// the same program - now literally so. The startup sequence, the menus, the
// tick and the teardown are ONE copy, in ../StandaloneApp.cpp; what is left in
// this file is the answers only this platform can give, in the order that file
// asks for them. Direct2D instead of the CPU renderer, WASAPI instead of
// PipeWire, winmm instead of ALSA - and nothing else.
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
// Unlike the Wayland shell there is no explicit timer pump, and unlike the
// Cocoa one no preGraphicsRedraw either: gmpi::TimerManager has a native source
// here (SetTimer), so the frame's redraw tick and StandaloneHost's parameter
// pump both arrive as messages the loop dispatches, and DrawingFrameWin's own
// render timer already calls preGraphicsRedraw on the attached client. Both are
// stated once, in backendServices() below, and the app's tick then does the
// half this platform does not already do for itself.

#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <windows.h>

#include "AudioDriverWasapi.h"
#include "MidiDriverWin.h"
#include "ToplevelWindow.h"

#include "../StandaloneApp.h"

#if GMPI_STANDALONE_COMMAND_CHANNEL
#include "FrameCapture.h"
#endif

#include "GmpiUiDrawing.h"
#include "helpers/unicode_conversion.h"

namespace
{

using namespace gmpi::standalone;

// DPI awareness and the COM apartment, as a LIFETIME.
//
// This is what the hand-written scope in the old wWinMain was for, expressed as
// a class so the compiler enforces it instead of a pair of braces somebody has
// to remember: declared first inside the shell below, it is therefore destroyed
// last, which puts CoUninitialize after the window, after the Direct2D factory
// and after FrameCapture's staging texture. Releasing a COM object once the
// apartment has gone is a crash on exit that looks like it came from whatever
// ran last.
class ProcessInit
{
public:
    ProcessInit()
    {
        // Before any window exists, and before anything asks for a DPI. V2
        // rather than plain per-monitor: it is what makes the non-client area
        // (the caption, the resize border) scale with the rest when the window
        // is dragged to another monitor, which plain per-monitor leaves at the
        // DPI the window was created at.
        ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        // STA, because this thread owns windows: OLE drag-drop, the common file
        // dialog and the colour picker all require it, and gmpi_ui's frame calls
        // OleInitialize when it registers its window class. The audio driver's
        // threads initialise their own MTA and never touch an object created
        // here.
        comInit_ = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }

    ~ProcessInit()
    {
        if (SUCCEEDED(comInit_))
            ::CoUninitialize();
    }

private:
    HRESULT comInit_{};
};

// Everything ../StandaloneApp.cpp needs to know about Windows.
//
// The methods are in the order PlatformShell declares them, which is the order
// they are called in; linux/MainWayland.cpp and mac/MainMac.mm carry the same
// list.
class Win32Shell final : public PlatformShell
{
public:
    bool createWindow(const std::string& title, int clientWidthDips, int clientHeightDips) override
    {
        if (!window_.create(title, clientWidthDips, clientHeightDips))
            return false;

        // The menu bar's font, and it can only be minted here: no text stack to
        // wire up, unlike Wayland - the Direct2D factory the frame already owns
        // has DirectWrite behind it, so this is a wrapper around the factory
        // that is there rather than a fontconfig/freetype/harfbuzz assembly, and
        // the factory does not exist until the frame does.
        //
        // Segoe UI, then the factory's own fallback. Naming the desktop's UI
        // font is the difference between a menu bar that looks like Windows and
        // one that looks like Arial.
        *gmpi::drawing::AccessPtr::put(factory_) = &window_.frame().DrawingFactory;

        const std::string_view menuFontFamily{ "Segoe UI" };
        menuFont_ = factory_.createTextFormat(14.0f, std::span{ &menuFontFamily, 1 });

        return true;
    }

    void setMinimumClientSize(int widthDips, int heightDips) override
    {
        window_.setMinimumClientSize(widthDips, heightDips);
    }

    gmpi::drawing::api::ITextFormat* menuFont() override
    {
        return gmpi::drawing::AccessPtr::get(menuFont_);
    }

    bool attachClient(gmpi::api::IDrawingClient* client, gmpi::api::IUnknown* parameterHost) override
    {
        // The fallback host first, always: the plugin's parameter pins take
        // their host from a queryInterface for IEditorHost during setHost, which
        // attachClient is what triggers.
        window_.frame().setFallbackHost(parameterHost);
        window_.frame().attachClient(client);
        return true;
    }

    void showAndPaint() override
    {
        // Nothing to do: runEventLoop's ShowWindow puts it on screen, and the
        // frame paints on the WM_PAINT that follows. Only macOS has to force a
        // paint here, and only because of the microphone prompt.
    }

    void requestClose() override
    {
        // Ends the loop and destroys nothing - the window hides and posts
        // WM_QUIT. It has to be that way because this is reached from a menu
        // completion with the editor's own view on the stack, and because the
        // teardown belongs to runStandaloneApp, which cannot run until the loop
        // it is called from has returned.
        window_.requestClose();
    }

    void closeWindow() override
    {
        // Against a LIVE frame: WM_CLOSE no longer destroys anything, so this
        // runs with the frame still whole and both device threads already
        // stopped.
        window_.frame().detachClient();
        window_.close();
    }

    std::unique_ptr<AudioDriver> createAudioDriver() override
    {
        return std::make_unique<AudioDriverWasapi>();
    }

    std::unique_ptr<MidiDriver> createMidiDriver() override
    {
        return std::make_unique<MidiDriverWin>();
    }

    BackendServices backendServices() const override
    {
        // SetTimer is gmpi::TimerManager's source here, and DrawingFrameWin's
        // own render timer already calls preGraphicsRedraw on the attached
        // client - so the app's tick must do neither.
        return { TimerSource::backendNative, RedrawClientDriver::frameRenderTimer };
    }

    int runEventLoop(const std::function<void(int elapsedMs)>& onTick) override
    {
        // A Ticker rather than a tick argument, because gmpi::TimerManager has a
        // native source here: SetTimer turns the callback into a WM_TIMER the
        // pump below dispatches. The Wayland loop, which has no such source,
        // takes the callback directly instead.
        Ticker tick(onTick);
        return window_.runEventLoop();
    }

    void reportStatus(const std::string& message) override
    {
        // BOTH, because neither alone is reliable for a GUI-subsystem app: a
        // debugger always sees OutputDebugString, and stderr is connected when
        // the app was launched as a child with its output piped, which is how
        // the MCP server runs it. Printing to nowhere is how this shell came to
        // report nothing at all.
        ::OutputDebugStringW((gmpi::unicode::to_wide(message) + L"\r\n").c_str());
        std::fprintf(stderr, "%s\n", message.c_str());
    }

    void showFatalAlert(const std::string& message) override
    {
        // A message box as well as the log line the caller has already written,
        // because there is no window to say it in and no stderr worth relying
        // on: a standalone that exits silently because the binary contains no
        // plugin is indistinguishable from one that crashed.
        ::MessageBoxW(nullptr,
                      gmpi::unicode::to_wide(message).c_str(),
                      L"GMPI Standalone",
                      MB_OK | MB_ICONERROR);
    }

#if GMPI_STANDALONE_COMMAND_CHANNEL
    bool framePixels(bool forceRedraw,
                     const uint8_t*& pixels, int& width, int& height, int& stride) override
    {
        return capture_.capture(forceRedraw, pixels, width, height, stride);
    }

    void logicalSize(float& width, float& height) override
    {
        // DIPs, which is the space pointer coordinates are in. The scale comes
        // from the frame's own transform rather than from the monitor, so this
        // cannot drift from what the editor was arranged at.
        RECT client{};
        ::GetClientRect(window_.hwnd(), &client);

        const float scale = window_.frame().getRasterizationScale();
        width  = scale > 0.0f ? client.right  / scale : static_cast<float>(client.right);
        height = scale > 0.0f ? client.bottom / scale : static_cast<float>(client.bottom);
    }

    void canvasSize(int& width, int& height) override
    {
        // The client rect IS the pixel size here: GetClientRect reports physical
        // pixels for a per-monitor-DPI-aware window, and the swap chain is
        // created with a zero-sized DXGI_SWAP_CHAIN_DESC1, which makes DXGI take
        // its extent from that same client area. So this is the number a
        // screenshot comes back with, without needing one to have been taken.
        //
        // Read straight rather than as logicalSize() * scale, so the two answers
        // divide into exactly the scale factor the frame is using.
        RECT client{};
        ::GetClientRect(window_.hwnd(), &client);

        width  = client.right;
        height = client.bottom;
    }
#endif

private:
    // MEMBER ORDER IS THE CONTRACT. ProcessInit first so that it is destroyed
    // last - see its comment - and FrameCapture last so its D3D staging texture
    // goes before the window that owns the device it came from.
    ProcessInit               processInit_;
    ToplevelWindow            window_;
    gmpi::drawing::Factory    factory_;
    gmpi::drawing::TextFormat menuFont_;
#if GMPI_STANDALONE_COMMAND_CHANNEL
    FrameCapture              capture_{ window_.frame() };
#endif
};

} // namespace

int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    // Constructed here rather than inside runStandaloneApp so that the COM
    // apartment and the window outlive it - the app is torn down first, and
    // only then, as this frame unwinds, the process it ran in.
    Win32Shell shell;
    return gmpi::standalone::runStandaloneApp(shell);
}
