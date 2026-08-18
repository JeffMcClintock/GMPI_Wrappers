#include "ToplevelWindowMac.h"

#include <algorithm>
#include <cmath>

// The three entry points gmpi_ui's Cocoa frame exposes to C++. Declared here
// rather than reached through a header, matching how every other consumer of
// them in this repo does it (wrapper/VST3/SEVSTGUIEditorMac.cpp,
// wrapper/CLAP/Editor_CLAP.cpp) - backends/DrawingFrameMac.h declares only the
// first, and pulling it in would drag Cocoa into a header that does not want it.
//
// `class IUnknown*` is the GLOBAL IUnknown these functions are declared against,
// not gmpi::api::IUnknown, hence the casts at the call sites below. Same cast
// the AU and VST3 wrappers make.
void* createNativeView(void* parent, class IUnknown* parameterHost, class IUnknown* client,
                       int width, int height);
void  resizeNativeView(void* view, int width, int height);
void  gmpi_onCloseNativeView(void* view);

// Objective-C runtime classes are per-PROCESS, not per-binary, so gmpi_ui gives
// each of its own a version-suffixed name to survive two plugins in one host.
// A standalone is one binary in one process and cannot collide with anything,
// but the convention is cheap and this class would collide loudly if the app
// ever grew a second window implementation.
#define GMPI_STANDALONE_WINDOW_DELEGATE GMPI_STANDALONE_WINDOW_DELEGATE_01

@interface GMPI_STANDALONE_WINDOW_DELEGATE : NSObject <NSWindowDelegate>
{
@public
    gmpi::standalone::ToplevelWindowMac* owner;
}
@end

@implementation GMPI_STANDALONE_WINDOW_DELEGATE

- (void)windowDidResize:(NSNotification*)notification
{
    (void)notification;
    if (owner)
        owner->onWindowResized();
}

// Fires for every step of a scale change too - dragging the window to a display
// with a different backing factor. The editor's bitmap is sized in backing
// pixels, so it has to be told even though the point size did not change.
- (void)windowDidChangeBackingProperties:(NSNotification*)notification
{
    (void)notification;
    if (owner)
        owner->onWindowResized();
}

- (void)windowWillClose:(NSNotification*)notification
{
    (void)notification;
    if (owner)
        owner->onWindowClosing();
}

@end

namespace gmpi
{
namespace standalone
{

ToplevelWindowMac::~ToplevelWindowMac()
{
    close();
}

bool ToplevelWindowMac::create(const std::string& title, int clientWidthPoints, int clientHeightPoints)
{
    const CGFloat width  = std::max(1, clientWidthPoints);
    const CGFloat height = std::max(1, clientHeightPoints);

    window_ = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, width, height)
                  styleMask:(NSWindowStyleMaskTitled
                           | NSWindowStyleMaskClosable
                           | NSWindowStyleMaskMiniaturizable
                           | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];

    if (!window_)
        return false;

    [window_ setTitle:[NSString stringWithUTF8String:title.c_str()]];

    // Cascaded from the top-left rather than left where initWithContentRect put
    // it, which on a multi-display setup is not necessarily on screen at all.
    [window_ center];

    // The window owns itself while the app runs and is released by close().
    // AppKit's default is to release on close, which would leave `window_`
    // dangling between windowWillClose: and our own teardown.
    [window_ setReleasedWhenClosed:NO];

    auto* delegate = [[GMPI_STANDALONE_WINDOW_DELEGATE alloc] init];
    delegate->owner = this;
    delegate_ = delegate;
    [window_ setDelegate:delegate];

    return true;
}

void ToplevelWindowMac::setMinimumClientSize(int widthPoints, int heightPoints)
{
    if (!window_)
        return;

    [window_ setContentMinSize:NSMakeSize(std::max(1, widthPoints), std::max(1, heightPoints))];
}

bool ToplevelWindowMac::attachClient(gmpi::api::IUnknown* client, gmpi::api::IUnknown* parameterHost)
{
    if (!window_ || view_)
        return false;

    NSView* content = [window_ contentView];
    if (!content)
        return false;

    const NSSize size = [content bounds].size;

    // createNativeView adds the view to `content` itself, so by the time this
    // returns the frame has already been through viewDidMoveToWindow and
    // arranged the client over the whole area.
    view_ = (NSView*)createNativeView(
        (void*)content,
        (class IUnknown*)parameterHost,
        (class IUnknown*)client,
        static_cast<int>(size.width),
        static_cast<int>(size.height));

    if (!view_)
        return false;

    // Deliberately NOT autoresizingMask. AppKit resizes subviews through
    // setFrameSize:, and gmpi_ui's view only overrides setFrame: - so the
    // backing bitmap would never be dropped and the editor would be a stretched
    // copy of itself at the old size. onWindowResized() calls resizeNativeView,
    // which goes through setFrame: like the VST3 wrapper's onSize does.
    [view_ setAutoresizingMask:NSViewNotSizable];

    [window_ makeFirstResponder:view_];
    [window_ makeKeyAndOrderFront:nil];

    return true;
}

void ToplevelWindowMac::onWindowResized()
{
    if (!window_ || !view_)
        return;

    const NSSize size = [[window_ contentView] bounds].size;

    // Points, not pixels: resizeNativeView takes the same space the editor
    // measured itself in, and the backing scale is applied below it when the
    // bitmap is reserved.
    resizeNativeView((void*)view_,
                     static_cast<int>(size.width),
                     static_cast<int>(size.height));

    // resizeNativeView only RELEASES the backing bitmap; the reallocation at the
    // new size happens lazily in the next drawRect:. Without this the window
    // shows stale content until something else happens to invalidate it, which
    // during a live resize drag is visible as the editor lagging the frame.
    [view_ setNeedsDisplay:YES];
}

void ToplevelWindowMac::paintNow()
{
    if (!window_)
        return;

    // -display rather than -setNeedsDisplay:, because the point is to be
    // finished drawing before this returns. The run loop has not been entered
    // yet at this stage, so a deferred repaint would not happen until after the
    // thing this is protecting against - see the header.
    [window_ display];
    [window_ flushWindowIfNeeded];
}

void ToplevelWindowMac::requestClose()
{
    if (!window_ || closing_)
        return;

    // Deferred to the next turn of the run loop. The caller is typically a menu
    // action dispatched with the editor's view still on the stack, and closing
    // the window from there would dealloc that view mid-handler.
    NSWindow* window = window_;
    dispatch_async(dispatch_get_main_queue(), ^{
        [window close];      // -> windowWillClose: -> onWindowClosing()
    });
}

void ToplevelWindowMac::onWindowClosing()
{
    if (closing_)
        return;
    closing_ = true;

    // Not [NSApp terminate:], which calls exit() and would skip every line
    // after runEventLoop() in MainMac.mm - the command channel's stop(), the
    // audio and MIDI threads, the frame detach. stop: unwinds the run loop
    // instead and lets that teardown happen in order.
    //
    // stop: only takes effect when the next event is processed, and a window
    // closing may well be the last event there is, so an event is posted to
    // guarantee one more trip round the loop.
    [NSApp stop:nil];

    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                       location:NSZeroPoint
                                  modifierFlags:0
                                      timestamp:0
                                   windowNumber:0
                                        context:nil
                                        subtype:0
                                          data1:0
                                          data2:0];
    [NSApp postEvent:wake atStart:YES];
}

float ToplevelWindowMac::rasterizationScale() const
{
    if (!window_)
        return 1.0f;

    const CGFloat scale = [window_ backingScaleFactor];
    return scale > 0.0 ? static_cast<float>(scale) : 1.0f;
}

void ToplevelWindowMac::logicalSize(float& width, float& height) const
{
    width = height = 0.0f;

    // Measured on the FRAME'S view rather than the content view, so this cannot
    // drift from what the editor was actually arranged at.
    NSView* v = view_ ? view_ : (window_ ? [window_ contentView] : nil);
    if (!v)
        return;

    const NSSize size = [v bounds].size;
    width  = static_cast<float>(size.width);
    height = static_cast<float>(size.height);
}

void ToplevelWindowMac::canvasSize(int& width, int& height) const
{
    width = height = 0;

    float points[2]{};
    logicalSize(points[0], points[1]);
    if (points[0] <= 0.0f || points[1] <= 0.0f)
        return;

    // std::lround, matching the CGBitmapContext FrameCapture allocates. Points
    // are CGFloat and the backing scale is fractional on nothing Apple ships
    // today, but the AppKit APIs promise neither, so the rounding is stated
    // rather than left to a truncating cast.
    const float scale = rasterizationScale();

    width  = static_cast<int>(std::lround(points[0] * scale));
    height = static_cast<int>(std::lround(points[1] * scale));
}

int ToplevelWindowMac::runEventLoop()
{
    [NSApp run];
    return 0;
}

void ToplevelWindowMac::close()
{
    if (view_)
    {
        // Stops the frame's NSTimer and drops its client references while the
        // view is still in a window - the timer RETAINS the view, so a view
        // released before it is stopped goes on being called into.
        //
        // Not followed by removeFromSuperview or release, matching every other
        // consumer of this entry point (SEVSTGUIEditorMac::removed, and the AU
        // and CLAP editors): the view belongs to the hierarchy that adopted it,
        // and the one +1 from its alloc is surrendered at process exit rather
        // than by deallocating a view something may still hold.
        gmpi_onCloseNativeView((void*)view_);
        view_ = nil;
    }

    if (window_)
    {
        // Before the close, so the cascade below cannot re-enter
        // onWindowClosing() and post another wake event into a loop that has
        // already stopped.
        [window_ setDelegate:nil];

        [window_ orderOut:nil];
        [window_ close];
        [window_ release];
        window_ = nil;
    }

    if (delegate_)
    {
        [delegate_ release];
        delegate_ = nil;
    }
}

} // namespace standalone
} // namespace gmpi
