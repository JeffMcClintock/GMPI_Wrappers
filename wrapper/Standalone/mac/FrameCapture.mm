#include "FrameCapture.h"

#include "ToplevelWindowMac.h"

namespace gmpi
{
namespace standalone
{

FrameCapture::~FrameCapture()
{
    releaseContext();
}

void FrameCapture::releaseContext()
{
    if (context_)
    {
        CGContextRelease(context_);
        context_ = {};
    }
    width_ = height_ = 0;
    haveFrame_ = false;
}

bool FrameCapture::ensureContext(int pixelWidth, int pixelHeight)
{
    if (context_ && width_ == pixelWidth && height_ == pixelHeight)
        return true;

    releaseContext();

    // BGRX8888, spelled the way CoreGraphics spells it: a 32-bit LITTLE-ENDIAN
    // word with the pad in the high byte puts B,G,R,X in memory, which is the
    // layout PngWriter and the MCP client expect. NoneSkipFirst rather than an
    // alpha channel because the window is opaque and an alpha carrying padding
    // renders as a fully transparent PNG.
    //
    // sRGB, so the blit out of the frame's LINEAR bitmap is converted by
    // CoreGraphics rather than by a curve of our own - see the header.
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!colorSpace)
    {
        lastError_ = "no sRGB colour space";
        return false;
    }

    const size_t stride = static_cast<size_t>(pixelWidth) * 4;
    pixels_.assign(stride * static_cast<size_t>(pixelHeight), 0);

    context_ = CGBitmapContextCreate(
        pixels_.data(),
        static_cast<size_t>(pixelWidth), static_cast<size_t>(pixelHeight),
        8, stride, colorSpace,
        static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst)
            | static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little));

    CGColorSpaceRelease(colorSpace);

    if (!context_)
    {
        pixels_.clear();
        lastError_ = "could not allocate a bitmap to capture into";
        return false;
    }

    width_  = pixelWidth;
    height_ = pixelHeight;
    return true;
}

bool FrameCapture::capture(bool forceRedraw,
                           const uint8_t*& pixels, int& width, int& height, int& stride)
{
    lastError_.clear();

    NSView* view = window_.view();
    if (!view)
    {
        lastError_ = "the window has no editor frame yet";
        return false;
    }

    const NSRect bounds = [view bounds];
    const float scale = window_.rasterizationScale();

    // The same measurement the shell reports for --info, and taken from the same
    // place on purpose: a caller divides one by the other to recover the scale,
    // so a bitmap sized here by different arithmetic would make that ratio lie.
    int pixelWidth  = 0;
    int pixelHeight = 0;
    window_.canvasSize(pixelWidth, pixelHeight);

    if (pixelWidth <= 0 || pixelHeight <= 0)
    {
        lastError_ = "the window has no area to capture";
        return false;
    }

    // The contract in mcp/CommandDispatcher.h: only a caller that asks for a
    // redraw gets one. A verb that merely wants to look at the frame must not
    // make the whole editor paint to answer.
    //
    // What the unforced form can hand back is narrower here than on the other
    // two shells, and the difference is not laziness but what there is to read.
    // Windows reads its swap chain and Linux its shm buffer, so both report the
    // frame the USER is looking at. Nothing equivalent is in reach here:
    // gmpi_ui's copy is DrawingFrameCocoa::backBuffer, private to
    // backends/DrawingFrameMac.mm; the window server's copy needs a screen
    // capture API (CGWindowListCreateImage is deprecated as of macOS 14,
    // ScreenCaptureKit is consent-gated); and -cacheDisplayInRect: is the
    // re-render below under a different name. The app's own paints go to the
    // screen and never into the bitmap here.
    //
    // So the only frame this file can produce without painting is the one it
    // drew itself, on the last forced capture - and where there is none it says
    // so rather than quietly painting one.
    if (forceRedraw)
    {
        if (!ensureContext(pixelWidth, pixelHeight))
            return false;

        @autoreleasepool
        {
            // Points to backing pixels. The view's own coordinate system is in
            // points, and the CTM is the only thing that has to know about the
            // display's scale - everything below draws as if at scale 1.
            CGContextSaveGState(context_);
            CGContextScaleCTM(context_, scale, scale);

            // flipped:NO, matching the view. gmpi_ui's Cocoa view does NOT
            // override isFlipped when it is using its backing buffer, so it
            // lives in AppKit's default bottom-left-origin space - and its
            // final blit is a CGContextDrawImage in that space. Declaring this
            // context flipped would turn the capture upside down.
            NSGraphicsContext* destination =
                [NSGraphicsContext graphicsContextWithCGContext:context_ flipped:NO];

            [NSGraphicsContext saveGraphicsState];
            [NSGraphicsContext setCurrentContext:destination];

            // The synchronous trip through drawRect: -> DrawingFrameCocoa::
            // onRender. Ignoring opacity because the window is opaque and the
            // alternative would leave whatever was in our bitmap showing
            // through wherever the client did not paint.
            [view displayRectIgnoringOpacity:bounds inContext:destination];

            [NSGraphicsContext restoreGraphicsState];
            CGContextRestoreGState(context_);
        }

        haveFrame_ = true;
    }
    else if (!haveFrame_)
    {
        lastError_ = "nothing has been captured yet - on macOS only a screenshot paints";
        return false;
    }
    else if (pixelWidth != width_ || pixelHeight != height_)
    {
        // A capture at the previous size is not a stale view of this window, it
        // is a picture of a different one - and it would be handed over with the
        // OLD dimensions, which no longer divide into the window's logical size
        // by the scale factor --info reports. A caller mapping a coordinate onto
        // that PNG would land somewhere else entirely, so this refuses.
        lastError_ = "the window has been resized since the last capture";
        return false;
    }

    pixels = pixels_.data();
    width  = width_;
    height = height_;
    stride = width_ * 4;
    return true;
}

} // namespace standalone
} // namespace gmpi
