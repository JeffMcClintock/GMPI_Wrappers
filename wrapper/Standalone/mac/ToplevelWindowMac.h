#pragma once

// The standalone app's top-level window on macOS.
//
// The peer of windows/ToplevelWindow and gmpi::wayland::WaylandToplevel, and
// thin for the same reason the Windows one is: gmpi_ui already ships the hard
// half. On macOS that half is not a C++ class but an NSView - createNativeView
// in backends/DrawingFrameMac.mm builds the view that the GMPI AU, VST3 and
// CLAP wrappers all hand their host, carrying the CoreGraphics render path,
// mouse and keyboard dispatch, and the native popup/text-edit/file/colour
// dialogs. What it cannot do is exist on its own: it is created INSIDE a parent
// view, which in a DAW is the one the host supplies.
//
// So this class supplies exactly that missing parent - an NSWindow whose
// content view the editor frame fills - and nothing else. The plugin therefore
// sees the identical arrangement it sees in a DAW: its editor in a subview of a
// view owned by somebody else, at a backing scale it must read from the window
// rather than assume. Anything that only worked because the view happened to be
// the content view is a bug this shell would have hidden.
//
// TWO THINGS THE WINDOWS SHELL GETS FOR FREE AND THIS ONE DOES NOT:
//
//  * Resize does not reach the frame by itself. AppKit's autoresizing calls
//    setFrameSize:, and GMPI_VIEW_CLASS only overrides setFrame: - so an
//    autoresized editor would keep a backing bitmap at the old size and stretch.
//    The window delegate here calls resizeNativeView instead, which is the same
//    entry point the VST3 wrapper's onSize uses.
//  * Nothing calls preGraphicsRedraw. DrawingFrameWin drives it from its own
//    render timer; DrawingFrameCocoa has no equivalent, so the app's ticker
//    does it, exactly as the Wayland loop does.
//
// Sizes crossing this class's interface are POINTS, which is what the editor
// measures itself in and what a DIP is on the other two platforms. The
// conversion to backing pixels is AppKit's, and happens below this line.

#include <string>

#import <Cocoa/Cocoa.h>

#include "GmpiApiCommon.h"

namespace gmpi
{
namespace standalone
{

class ToplevelWindowMac
{
public:
    ~ToplevelWindowMac();

    // `clientWidth/Height` are the point size the plugin's editor asked for,
    // plus whatever the menu bar occupies. False means the window server would
    // not give us a window, which is fatal to the app.
    bool create(const std::string& title, int clientWidthPoints, int clientHeightPoints);

    // Enforced by AppKit through contentMinSize, so the user cannot drag the
    // window smaller than the menu bar's titles.
    void setMinimumClientSize(int widthPoints, int heightPoints);

    // Builds the editor frame inside the window and attaches `client` to it.
    //
    // The macOS counterpart of DrawingFrame::attachClient + setFallbackHost in
    // one call, because here they cannot be separated: the frame does not exist
    // until it has a client to be built around. `parameterHost` is what answers
    // the queryInterface for IEditorHost that the plugin's parameter pins make
    // during setHost - without it the first knob drag dereferences null.
    //
    // Must be called after create() and before the run loop starts.
    bool attachClient(gmpi::api::IUnknown* client, gmpi::api::IUnknown* parameterHost);

    // Paints the window NOW, without waiting for the run loop.
    //
    // Called between attaching the editor and opening the audio device, and the
    // reason is macOS-specific: opening a device with an INPUT triggers the
    // microphone permission prompt the first time, and that blocks the calling
    // thread - here, the main thread, before [NSApp run] - for as long as it
    // takes the user to answer. Without this the app asks for the microphone
    // while showing an empty window frame, which is exactly the shape of a
    // prompt people decline.
    void paintNow();

    // Asks for the window to shut, which ends runEventLoop(). This is what a
    // menu item calls, and it is DEFERRED to a later turn of the run loop on
    // purpose: a File>Quit arrives inside the menu's action handler with the
    // editor's own view on the stack, and tearing that view down underneath
    // itself is a crash the Windows shell cannot have (DestroyWindow from
    // inside a message handler is legal there).
    void requestClose();

    // Idempotent, and called by the destructor. Tears the editor frame down
    // BEFORE the window, so the view never receives anything from its window's
    // teardown after the frame that would dispatch it has gone.
    //
    // Call it AFTER runEventLoop() has returned, never from inside a menu or an
    // input handler - see requestClose().
    void close();

    NSWindow* window() const { return window_; }

    // The editor frame's own view - the SUBVIEW createNativeView added, not the
    // content view. Everything measured or captured is measured on this one;
    // measuring the parent is the macOS shape of the mistake the Windows resize
    // audit made by measuring the parent HWND (see tests/mac_editor_resize_host.mm).
    NSView* view() const { return view_; }

    // Backing pixels per point, from the screen the window is actually on.
    float rasterizationScale() const;

    // The content area in POINTS, which is the space pointer coordinates are in
    // and the space a screenshot is measured in at scale 1.
    void logicalSize(float& width, float& height) const;

    // Runs until the window closes. Returns the process exit code.
    int runEventLoop();

    // Called by the window delegate; not part of the app's own vocabulary.
    void onWindowResized();
    void onWindowClosing();

private:
    NSWindow* window_{};
    NSView*   view_{};        // the frame's view, owned by the content view
    id        delegate_{};    // the NSWindowDelegate that forwards to us

    bool closing_ = false;
};

} // namespace standalone
} // namespace gmpi
