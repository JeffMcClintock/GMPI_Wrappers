#pragma once

// Draws the app's own window into a bitmap it owns.
//
// This is what makes the command channel's --screenshot verb work on macOS. Of
// the three platforms it is the least like the others, and the reason is where
// the frame lives:
//
//   Linux    the app drew into an shm buffer it owns, so the pixels are simply
//            already there.
//   Windows  the frame is on the GPU, so windows/FrameCapture reads the swap
//            chain back and converts scRGB half-floats to sRGB by hand.
//   macOS    the frame is a CGBitmapContext inside gmpi_ui's Cocoa view, and
//            nothing outside that file can reach it. So this asks AppKit to
//            draw the view AGAIN, into a context of ours.
//
// Drawing again rather than reading back is not a compromise here. The Windows
// note warns that re-rendering into a bitmap of one's own would use device
// resources the client did not cache against - a real hazard for Direct2D. The
// Cocoa backend has no device: DrawingFrameCocoa::onRender reserves its bitmap
// lazily, paints into it and blits, and the blit lands in whatever context is
// current. Handing it ours is exactly what drawRect: does with the screen's.
//
// It also gets the colour conversion for free. That backing bitmap is in LINEAR
// sRGB, and CoreGraphics converts on the blit because the destination context
// below declares sRGB - so this file has no equivalent of the Windows
// linearToSRGB01 loop, and cannot drift from it.
//
// Output is BGRX8888 - the same layout the Wayland shell reports and the
// Windows one converts to - so mcp/PngWriter and the MCP client beyond it see
// one pixel format on every platform.
//
// The consequence of drawing rather than reading back, and the whole of this
// file's asymmetry with the other two: the bitmap below only ever holds frames
// THIS class drew. The app's own paints go to the screen, so a caller that does
// not ask for a redraw has nothing here to be handed until a forced capture has
// run - and gets an honest no rather than a paint. capture() says why that is
// the only frame within reach on this backend.
//
// Nothing else in the app is narrowed by that. The one thing a caller used to
// come here for without wanting a picture - how big the canvas is - is answered
// from the window instead, by PlatformShell::canvasSize, on every platform
// alike.

#include <cstdint>
#include <string>
#include <vector>

#import <Cocoa/Cocoa.h>

namespace gmpi
{
namespace standalone
{

class ToplevelWindowMac;

class FrameCapture
{
public:
    explicit FrameCapture(ToplevelWindowMac& window) : window_(window) {}
    ~FrameCapture();

    /// Signature and semantics of mcp::AppContext::framePixels.
    ///
    /// `forceRedraw` paints first, so a screenshot taken right after
    /// --set-param shows the new value rather than a frame that predates the
    /// command. It is the only thing in this file that paints.
    ///
    /// False never paints: it hands back the last forced capture if that
    /// capture is still the size of the window, and otherwise fails. So on this
    /// platform there is nothing to be had here before the first screenshot.
    ///
    /// That is a restriction on PIXELS only. The window's dimensions are
    /// geometry and need no paint at all, so nobody asks for them through this
    /// method: PlatformShell::canvasSize answers them from the window itself,
    /// which is what --info reports and what this file sizes its bitmap by.
    ///
    /// The returned pointer is owned by this object and stays valid until the
    /// next call. False means there is nothing to hand back - no view, no area,
    /// or no capture at the window's current size - and lastError() says which.
    bool capture(bool forceRedraw,
                 const uint8_t*& pixels, int& width, int& height, int& stride);

    /// Why the last capture() returned false. Empty when it succeeded.
    const std::string& lastError() const { return lastError_; }

private:
    // Reused across calls: a screenshot every frame is a plausible thing for a
    // test to do, and a full-window bitmap context per call would make that
    // visibly slow.
    bool ensureContext(int pixelWidth, int pixelHeight);
    void releaseContext();

    ToplevelWindowMac& window_;

    CGContextRef context_{};
    std::vector<uint8_t> pixels_;
    int width_  = 0;   // backing pixels
    int height_ = 0;
    bool haveFrame_ = false;

    std::string lastError_;
};

} // namespace standalone
} // namespace gmpi
