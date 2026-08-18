#pragma once

// The standalone app's top-level window on Windows.
//
// The peer of gmpi::wayland::WaylandToplevel, but much thinner, because on
// Windows gmpi_ui already ships the hard half: gmpi::hosting::DrawingFrame is
// the Direct2D editor frame the GMPI VST3 wrapper embeds in a DAW, and it
// carries the swap chain, the render loop, the mouse and keyboard dispatch, the
// tooltip and the native popup/text-edit/file-dialog implementations. What it
// cannot do is exist on its own: open() takes a PARENT and creates a WS_CHILD
// window inside it.
//
// So this class supplies exactly the piece that is missing - an overlapped
// window whose client area the frame fills - and nothing else. The plugin
// consequently sees the identical arrangement it sees in a DAW: its editor
// lives in a child HWND owned by someone else, at a DPI it must read from the
// window rather than assume. Anything that would only work because the frame
// happened to be top-level is a bug this shell would have hidden.
//
// DPI. The process is per-monitor-v2 aware (set in MainWin32.cpp), so a window
// dragged between monitors is rescaled rather than bitmap-stretched. Sizes
// crossing this class's interface are DIPs, matching what the editor measures
// itself in; the conversion to pixels happens here, against the DPI of the
// monitor the window is actually on.

#include <string>

#include <windows.h>

#include "backends/DrawingFrameWin.h"

namespace gmpi
{
namespace standalone
{

class ToplevelWindow
{
public:
    ~ToplevelWindow();

    // `clientWidth/Height` are the DIP size the plugin's editor asked for, plus
    // whatever the menu bar occupies. False means the window class or the
    // window itself could not be created, which is fatal to the app.
    bool create(const std::string& title, int clientWidthDips, int clientHeightDips);

    // Enforced through WM_GETMINMAXINFO, so the user cannot drag the window
    // smaller than the menu bar's titles. Unlike Wayland, where the compositor
    // is the only thing that can refuse a resize, here we simply say no.
    void setMinimumClientSize(int widthDips, int heightDips);

    // Idempotent, and called by the destructor. Tears the frame down BEFORE the
    // top-level window, so the child never receives messages from its parent's
    // destruction cascade after the frame that would dispatch them is gone.
    void close();

    // Asks for the window to go away WITHOUT tearing anything down yet: it
    // hides, the event loop stops, and main() then stops audio and MIDI before
    // close() destroys the frame. ToplevelWindowMac::requestClose is the peer,
    // and both exist for the same reason - closing straight from a WM_CLOSE or
    // a menu completion would destroy the frame while the audio thread is still
    // calling the plugin through it. Hiding first is what keeps the close
    // feeling instant while the device is still being handed back.
    void requestClose();

    HWND hwnd() const { return hwnd_; }

    // The editor frame filling the client area. Everything the app does to the
    // window's contents - attachClient, setFallbackHost, the drawing factory -
    // goes through here.
    gmpi::hosting::DrawingFrame& frame() { return frame_; }

    // Standard GetMessage pump, returning WM_QUIT's exit code. No idle tick of
    // its own: unlike Linux, gmpi::TimerManager has a native source here
    // (SetTimer), so the frame's redraw tick and the host's parameter pump both
    // arrive as messages this loop dispatches.
    int runEventLoop();

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT onMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void resizeFrameToClient();
    UINT dpi() const;

    HWND hwnd_{};
    gmpi::hosting::DrawingFrame frame_;

    int minClientWidthDips_  = 0;
    int minClientHeightDips_ = 0;
};

} // namespace standalone
} // namespace gmpi
