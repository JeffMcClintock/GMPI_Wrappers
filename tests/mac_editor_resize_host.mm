// ---------------------------------------------------------------------------
// A minimal Cocoa VST3 host that attaches a plugin editor and then asks it to
// resize to absurd extents, including the exact rect that killed a Windows host.
// The macOS counterpart of win_editor_resize_host.cpp, written for BACKLOG P7.
//
// WHAT IT FOUND, so nobody has to re-derive it: macOS does NOT reproduce the
// Windows crash, 3/3, on two different plugins. See the audit for why the Cocoa
// path is structurally immune. This file is now a regression guard rather than a
// reproduction.
//
// WHY THIS EXISTS
//
// On Windows, IPlugView::onSize({0, 0, 2178, 32672}) killed the host process:
// DrawingFrame::reSize checked its D2D device, called SetWindowPos -- which
// *sends* WM_SIZE, whose handler releases the device on a failed ResizeBuffers
// -- and then dereferenced the pointer it had checked before the call.
// Time-of-check/time-of-use across a re-entrant Win32 call. Fixed in P4a/P4b and
// proven 3/3 by the Windows harness beside this file.
//
// The macOS path was never audited for the same pattern. This is that audit's
// executable half.
//
// THE ONE THING A PORT MUST NOT COPY LITERALLY
//
// On Windows the crash happens INSIDE onSize, because SetWindowPos dispatches
// WM_SIZE synchronously. On macOS it cannot:
//
//     SEVSTGUIEditorMac::onSize -> resizeNativeView -> [NSView setFrame:]
//         -> GMPI_VIEW_CLASS setFrame: -> DrawingFrameCocoa::onResize()
//         -> CGContextRelease(backBuffer); backBuffer = nullptr;
//
// onResize() only *releases* the backing bitmap. The reallocation at the new
// size happens later, lazily, in DrawingFrameCocoa::onRender -- i.e. in the next
// drawRect:. So a test that calls onSize and reports "SURVIVED" has exercised
// almost nothing: it has proved that releasing a bitmap does not crash.
//
// Every resize in this harness is therefore followed by a FORCED SYNCHRONOUS
// PAINT (forcePaint below). That is where the oversized CGBitmapContextCreate is
// actually attempted, and it is the only place a macOS equivalent of the crash
// could surface.
//
// WHAT THE OVERSIZED PAINT ACTUALLY DOES, MEASURED
//
// Not what the Windows story predicts. CGBitmapContextCreate does not refuse
// these sizes -- measured directly, it accepts 2178 x 32672 and every other rect
// below it, and a binary search puts its square limit at 131071 x 131071 for the
// 16-bit format initBackingBitmap asks for. So backBuffer is NOT null, onRender
// does NOT bail, and there is no null to dereference.
//
// The consequence is memory, not a crash: resident size climbs into the hundreds
// of megabytes, and a single paint at 16385 x 600 was measured costing +253 MiB.
// That is why this harness reports resident size after every paint. Treat those
// numbers as orders of magnitude -- they include the harness's own bitmaps and
// are visibly noisy -- but the scale is the finding.
//
// A NOTE ON THE DISTINCT-COLOUR NUMBERS AT ABSURD SIZES
//
// They are diagnostic, NOT a pass criterion, and a low count at a huge extent
// does not mean the editor went blank. The client is arranged over the whole
// view, so whether the sampled region contains anything depends on where that
// client puts its content: at 16385 x 600 a Gain knob's sampled tiles came back
// uniform while TIDE's returned 65 distinct colours, on the same harness in the
// same run order. Only two counts gate the result -- the liveness probe before,
// and the recovery probe after.
//
// THE LIVENESS TRAP, AND WHY IT IS DIFFERENT HERE
//
// The Windows harness proves the renderer is live by making a benign resize and
// checking the window adopted it -- that proves reSize got past its device check
// and reached SetWindowPos. That reasoning DOES NOT TRANSFER: on macOS
// resizeNativeView calls setFrame unconditionally, with no device check at all,
// so "the view adopted the new size" would be true even with no renderer at all.
// A port that copied the Windows liveness probe would report a false PASS.
//
// So liveness here is two independent facts, and both must hold:
//
//   A. the plugin's own NSView adopted the probed size  (the resize path ran)
//   B. a forced paint of that view produced more than one distinct pixel value
//      (the renderer ran and CGBitmapContextCreate succeeded -- the macOS
//      analogue of "the D2D device exists")
//
// Without B, "it survived" and "it never drew" are indistinguishable.
//
// Usage:
//     mac_editor_resize_host <plugin.vst3> [right] [bottom]
//
// Exit status:
//     0  survived every oversized resize+paint, and the editor was provably live
//     1  setup failed -- the message says which step
//     3  the editor was NOT provably live, so surviving proves nothing
//
// A regression shows up as the process dying (SIGSEGV / SIGBUS / abort), not as
// an exit code. Run it from a script that treats any nonzero status as failure.
// ---------------------------------------------------------------------------

#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <set>
#include <algorithm>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"

using namespace Steinberg;

namespace
{

bool iidEqual(const TUID a, const TUID b) { return std::memcmp(a, b, 16) == 0; }

// ---------------------------------------------------------------------------
// The host side, minimally: IHostApplication so initialize() succeeds,
// IComponentHandler because a controller may ask for one, and IPlugFrame
// because a well-behaved view expects a frame before it is attached.
// Identical in shape to the Windows harness -- deliberately, so the two can be
// compared line for line.
// ---------------------------------------------------------------------------
class Host : public Vst::IHostApplication, public Vst::IComponentHandler, public IPlugFrame
{
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (iidEqual(iid, FUnknown_iid) || iidEqual(iid, Vst::IHostApplication_iid))
            { *obj = static_cast<Vst::IHostApplication*>(this); return kResultOk; }
        if (iidEqual(iid, Vst::IComponentHandler_iid))
            { *obj = static_cast<Vst::IComponentHandler*>(this); return kResultOk; }
        if (iidEqual(iid, IPlugFrame_iid))
            { *obj = static_cast<IPlugFrame*>(this); return kResultOk; }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API getName(Vst::String128 name) override
    {
        static const char16_t n[] = u"mac_editor_resize_host";
        std::memcpy(name, n, sizeof(n));
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID, TUID, void** obj) override
    {
        *obj = nullptr;
        return kNotImplemented;
    }

    tresult PLUGIN_API beginEdit(Vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(Vst::ParamID, Vst::ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(Vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override
    {
        std::printf("  [host] resizeView requested: %d x %d\n",
            newSize->right - newSize->left, newSize->bottom - newSize->top);
        return kResultOk;
    }
};

// Run the Cocoa run loop for ms milliseconds. The backing bitmap is created
// lazily on the first paint, so this has to have run before any of the probes
// below mean anything.
void pump(int ms)
{
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:(ms / 1000.0)];
    while ([until timeIntervalSinceNow] > 0)
    {
        NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES];
        if (e)
            [NSApp sendEvent:e];

        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
    }
}

// The view that matters is the PLUGIN's, not ours. gmpi_openNativeView creates
// its own NSView and adds it as a subview of the one the host hands over, and
// resizeNativeView calls setFrame on THAT. Our content view never moves.
//
// This is the macOS shape of the mistake P2 made on Windows by measuring the
// parent HWND instead of GetWindow(hwnd, GW_CHILD).
NSView* pluginChild(NSView* parent)
{
    NSArray<NSView*>* subs = [parent subviews];
    return ([subs count] > 0) ? [subs objectAtIndex:0] : nil;
}

// This process's resident size, in MiB. The macOS hazard of an unbounded extent
// turned out to be memory rather than a crash (see the header note), so the
// harness has to be able to see memory.
double residentMiB()
{
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS)
        return -1.0;
    return double(info.resident_size) / (1024.0 * 1024.0);
}

// Count distinct colours in one cached tile. Returns false if the tile could not
// be captured at all.
bool sampleTile(NSView* v, NSRect r, std::set<unsigned int>& seen)
{
    NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:r];
    if (!rep)
        return false;

    // The synchronous draw. Everything this file is about happens in here.
    // Note the selector: it is cacheDisplayInRect:toBitmapImageRep:, not
    // ...toBitmap:. The short form does not exist, compiles anyway as an unknown
    // selector returning id, and throws at run time.
    [v cacheDisplayInRect:r toBitmapImageRep:rep];

    unsigned char* px = [rep bitmapData];
    if (!px)
        return false;

    const long bpr = [rep bytesPerRow];
    const long spp = [rep samplesPerPixel];
    const long w   = [rep pixelsWide];
    const long h   = [rep pixelsHigh];

    const long stepX = (w > 32) ? (w / 32) : 1;
    const long stepY = (h > 32) ? (h / 32) : 1;

    for (long y = 0; y < h; y += stepY)
    {
        for (long x = 0; x < w; x += stepX)
        {
            const unsigned char* p = px + y * bpr + x * spp;
            const unsigned int v32 = (unsigned int)p[0]
                                   | ((unsigned int)((spp > 1) ? p[1] : 0) << 8)
                                   | ((unsigned int)((spp > 2) ? p[2] : 0) << 16);
            seen.insert(v32);
            if (seen.size() > 64)
                return true;            // plenty; stop counting
        }
    }
    return true;
}

// Force a synchronous trip through drawRect: -> DrawingFrameCocoa::onRender and
// report how much distinct colour came back.
//
// This is the load-bearing probe in this file, for two reasons:
//
//  * it is the only thing that makes the plugin reallocate its backing bitmap at
//    whatever size was last set -- on macOS the resize itself allocates nothing;
//  * distinct-colour count is the macOS answer to "is the renderer live". If the
//    backing bitmap were missing, onRender bails at `if(!backBuffer) return;`
//    having drawn nothing, and every tile comes back uniform.
//
// WHY TILES, NOT ONE RECT AT THE ORIGIN.
//
// The harness must bound the bitmap IT allocates -- asking for a 2178 x 32672 rep
// here would just make the harness the thing that dies of a 285 MB allocation --
// but a single small rect at the origin is a measurement bug, not a bound. The
// client is arranged over the WHOLE view, so at 16385 x 600 the top-left 200 x 200
// is legitimately empty background and a single-tile probe reports "drew nothing"
// for a perfectly healthy editor. That false negative was observed on the first
// run of this harness and read, briefly, as the editor going blank.
//
// So: several small tiles spread across the view, distinct colours unioned. Cost
// is bounded by tile size and count, and coverage no longer depends on where the
// client happens to put its content.
bool forcePaint(NSView* v, int& distinctOut)
{
    distinctOut = 0;

    if (!v)
        return false;

    // SAMPLE THE VISIBLE RECT, NOT THE BOUNDS.
    //
    // After onSize(0, 0, 2178, 32672) the view is 32672 points tall inside a
    // window that is still 200 tall, so all but a sliver of it is clipped and
    // nothing is rendered there. Tiles placed by `bounds` land in that clipped
    // region -- including the one at the origin, because AppKit's unflipped
    // origin is the BOTTOM-left, which after such a resize is far below the
    // window. Every tile then comes back uniform and the harness reports "drew
    // nothing" for a healthy editor.
    //
    // That false negative was observed and briefly read as the oversized resize
    // blanking the editor. It is the same shape as P2's Windows mistake of
    // measuring the parent HWND: the measurement was of the wrong rectangle, not
    // of a broken renderer. visibleRect is the part that genuinely got drawn.
    const NSRect b = [v visibleRect];
    if (b.size.width < 1 || b.size.height < 1)
        return false;

    const CGFloat tile = 200.0;
    const CGFloat tw = (std::min)(tile, b.size.width);
    const CGFloat th = (std::min)(tile, b.size.height);

    // Corners and centre of the VISIBLE rect (whose origin need not be 0,0).
    const CGFloat xs[] = { b.origin.x,
                           b.origin.x + (b.size.width  - tw) / 2,
                           b.origin.x + b.size.width  - tw };
    const CGFloat ys[] = { b.origin.y,
                           b.origin.y + (b.size.height - th) / 2,
                           b.origin.y + b.size.height - th };

    std::set<unsigned int> seen;
    bool anyCaptured = false;

    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            // The four corners and the centre is plenty; skip the edge midpoints.
            const bool isCorner = (i != 1) && (j != 1);
            const bool isCentre = (i == 1) && (j == 1);
            if (!isCorner && !isCentre)
                continue;

            NSRect r = NSIntersectionRect(NSMakeRect(xs[i], ys[j], tw, th), b);
            if (r.size.width < 1 || r.size.height < 1)
                continue;

            if (sampleTile(v, r, seen))
                anyCaptured = true;

            if (seen.size() > 64)
                break;
        }
    }

    distinctOut = (int)seen.size();
    return anyCaptured;
}

void viewSize(NSView* v, int& w, int& h)
{
    if (!v)
    {
        w = h = -1;
        return;
    }
    const NSRect f = [v frame];
    w = (int)f.size.width;
    h = (int)f.size.height;
}

// One resize + forced paint, reported as a unit. Returns the distinct-colour
// count from the paint that followed the resize.
int resizeAndPaint(IPlugView* view, NSView* child, int right, int bottom, const char* label)
{
    std::printf("\n>>> %s: onSize(0, 0, %d, %d)\n", label, right, bottom);
    std::fflush(stdout);

    ViewRect r{ 0, 0, right, bottom };
    view->onSize(&r);

    std::printf("    survived onSize\n");
    std::fflush(stdout);

    pump(120);

    const double rssBefore = residentMiB();

    // The half Windows gets for free from SetWindowPos.
    int distinct = 0;
    const bool painted = forcePaint(child, distinct);

    const double rssAfter = residentMiB();

    int cw = 0, chh = 0;
    viewSize(child, cw, chh);
    std::printf("    survived forced paint: child now %d x %d, paint %s, distinct colours %d\n",
        cw, chh, painted ? "ran" : "could not run", distinct);
    std::printf("    resident: %.1f MiB -> %.1f MiB (paint cost %+.1f MiB)\n",
        rssBefore, rssAfter, rssAfter - rssBefore);
    std::fflush(stdout);

    return distinct;
}

} // anonymous namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <plugin.vst3> [right] [bottom]\n", argv[0]);
        return 1;
    }

    const char* pluginPath = argv[1];
    // Defaults are the rect the Windows crash dump recorded REAPER passing.
    const int crashRight  = (argc > 2) ? std::atoi(argv[2]) : 2178;
    const int crashBottom = (argc > 3) ? std::atoi(argv[3]) : 32672;

    std::printf("plugin: %s\n", pluginPath);

    @autoreleasepool
    {
    // A command-line tool gets no NSApplication and no connection to the window
    // server unless it asks. Without this, every view below is inert and the
    // whole run is a false negative.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // -----------------------------------------------------------------------
    // Load the bundle. macOS VST3s are bundles with bundleEntry/bundleExit,
    // where Windows has InitDll/ExitDll -- that pair is the only real
    // difference in the loading half of this harness.
    // -----------------------------------------------------------------------
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, (const UInt8*)pluginPath, (CFIndex)std::strlen(pluginPath), true);
    if (!url)
    {
        std::fprintf(stderr, "FAIL: could not make a URL from '%s'\n", pluginPath);
        return 1;
    }

    CFBundleRef bundle = CFBundleCreate(nullptr, url);
    CFRelease(url);
    if (!bundle)
    {
        std::fprintf(stderr, "FAIL: CFBundleCreate failed -- not a bundle?\n");
        return 1;
    }

    CFErrorRef cfErr = nullptr;
    if (!CFBundleLoadExecutableAndReturnError(bundle, &cfErr))
    {
        std::fprintf(stderr, "FAIL: CFBundleLoadExecutable failed\n");
        if (cfErr)
        {
            CFStringRef d = CFErrorCopyDescription(cfErr);
            char buf[512]{};
            if (d && CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8))
                std::fprintf(stderr, "      %s\n", buf);
            if (d) CFRelease(d);
            CFRelease(cfErr);
        }
        return 1;
    }

    if (auto entry = reinterpret_cast<bool (*)(CFBundleRef)>(
            CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleEntry"))))
    {
        if (!entry(bundle))
        {
            std::fprintf(stderr, "FAIL: bundleEntry() returned false\n");
            return 1;
        }
    }

    auto getFactory = reinterpret_cast<IPluginFactory* (*)()>(
        CFBundleGetFunctionPointerForName(bundle, CFSTR("GetPluginFactory")));
    if (!getFactory)
    {
        std::fprintf(stderr, "FAIL: no GetPluginFactory export\n");
        return 1;
    }

    IPluginFactory* factory = getFactory();
    if (!factory)
    {
        std::fprintf(stderr, "FAIL: GetPluginFactory returned null\n");
        return 1;
    }

    Host host;

    IPluginFactory3* factory3 = nullptr;
    if (factory->queryInterface(IPluginFactory3_iid, reinterpret_cast<void**>(&factory3)) == kResultOk && factory3)
        factory3->setHostContext(static_cast<Vst::IHostApplication*>(&host));

    // Find the audio effect class.
    Vst::IComponent* component = nullptr;
    PClassInfo chosen{};
    for (int32 i = 0; i < factory->countClasses(); ++i)
    {
        PClassInfo ci{};
        if (factory->getClassInfo(i, &ci) != kResultOk)
            continue;
        if (std::strcmp(ci.category, kVstAudioEffectClass) != 0)
            continue;
        if (factory->createInstance(ci.cid, Vst::IComponent_iid, reinterpret_cast<void**>(&component)) == kResultOk && component)
        {
            chosen = ci;
            break;
        }
    }

    if (!component)
    {
        std::fprintf(stderr, "FAIL: no audio effect class could be instantiated\n");
        return 1;
    }
    std::printf("class:  \"%s\"\n", chosen.name);

    if (component->initialize(static_cast<Vst::IHostApplication*>(&host)) != kResultOk)
    {
        std::fprintf(stderr, "FAIL: IComponent::initialize failed\n");
        return 1;
    }

    // Controller: separate class if the plugin names one, otherwise the same
    // object.
    Vst::IEditController* controller = nullptr;
    TUID controllerCid{};
    if (component->getControllerClassId(controllerCid) == kResultOk)
    {
        if (factory->createInstance(controllerCid, Vst::IEditController_iid, reinterpret_cast<void**>(&controller)) == kResultOk && controller)
            controller->initialize(static_cast<Vst::IHostApplication*>(&host));
    }
    if (!controller)
        component->queryInterface(Vst::IEditController_iid, reinterpret_cast<void**>(&controller));

    if (!controller)
    {
        std::fprintf(stderr, "FAIL: no edit controller\n");
        return 1;
    }
    controller->setComponentHandler(static_cast<Vst::IComponentHandler*>(&host));

    IPlugView* view = controller->createView(Vst::ViewType::kEditor);
    if (!view)
    {
        std::fprintf(stderr, "FAIL: createView returned null -- no editor to resize\n");
        return 1;
    }

    if (view->isPlatformTypeSupported(kPlatformTypeNSView) != kResultTrue)
    {
        std::fprintf(stderr, "FAIL: view does not support NSView\n");
        return 1;
    }

    ViewRect vs{};
    view->getSize(&vs);
    const int w0 = (vs.right - vs.left) > 0 ? (vs.right - vs.left) : 800;
    const int h0 = (vs.bottom - vs.top) > 0 ? (vs.bottom - vs.top) : 600;
    std::printf("view size: %d x %d\n", w0, h0);

    // -----------------------------------------------------------------------
    // A real window. cacheDisplayInRect: needs the view to be in one.
    // -----------------------------------------------------------------------
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(100, 100, w0, h0)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    if (!window)
    {
        std::fprintf(stderr, "FAIL: could not create an NSWindow -- no window server?\n");
        return 1;
    }
    [window setTitle:@"TIDE editor resize harness (macOS)"];
    [window makeKeyAndOrderFront:nil];

    NSView* content = [window contentView];
    if (!content)
    {
        std::fprintf(stderr, "FAIL: window has no content view\n");
        return 1;
    }

    // setFrame before attached: that is where a view is meant to pick up its frame.
    view->setFrame(static_cast<IPlugFrame*>(&host));

    if (view->attached((void*)content, kPlatformTypeNSView) != kResultTrue)
    {
        std::fprintf(stderr, "FAIL: attached() failed\n");
        return 1;
    }
    std::printf("attached: ok\n");

    // Let it paint at least once.
    pump(900);

    NSView* child = pluginChild(content);
    if (!child)
    {
        std::fprintf(stderr, "FAIL: attached() created no subview to measure\n");
        return 1;
    }
    std::printf("plugin child view: %p\n", (void*)child);

    int cw = 0, ch = 0;
    viewSize(child, cw, ch);
    std::printf("child before probe: %d x %d\n", cw, ch);

    // -----------------------------------------------------------------------
    // Liveness, fact A: a benign resize the plugin's own view adopts.
    //
    // NOTE this proves LESS here than the same probe proves on Windows.
    // resizeNativeView calls setFrame with no device check, so adoption alone
    // says nothing about the renderer. That is what fact B is for.
    // -----------------------------------------------------------------------
    const int probeW = (cw > 500) ? cw - 120 : cw + 120;
    const int probeH = (ch > 400) ? ch - 90  : ch + 90;
    std::printf("probe onSize(0, 0, %d, %d)\n", probeW, probeH);

    ViewRect probe{ 0, 0, probeW, probeH };
    view->onSize(&probe);
    pump(400);

    int pw = 0, ph = 0;
    viewSize(child, pw, ph);
    std::printf("child after probe:  %d x %d\n", pw, ph);

    const bool adopted = (pw == probeW && ph == probeH);

    // -----------------------------------------------------------------------
    // Liveness, fact B: a forced paint that actually draws.
    // -----------------------------------------------------------------------
    int liveDistinct = 0;
    const bool painted = forcePaint(child, liveDistinct);

    std::printf("\nliveness A -- plugin view adopted the probed size:   %s\n", adopted ? "YES" : "NO");
    std::printf("liveness B -- forced paint produced real drawing:    %s (distinct colours %d)\n",
        (painted && liveDistinct > 1) ? "YES" : "NO", liveDistinct);

    const bool live = adopted && painted && liveDistinct > 1;
    std::printf("editor live (resize path ran AND renderer produced pixels): %s\n", live ? "YES" : "NO");

    // -----------------------------------------------------------------------
    // checkSizeConstraint lives on a different call than any crash. It is what a
    // *polite* host asks before calling onSize, and the VST3 contract is that it
    // writes back the nearest size the view will take. Returning kResultFalse
    // with the rect untouched tells the host nothing -- which is how an
    // unnegotiated rect reaches onSize in the first place.
    //
    // P4b fixed this on Windows. Probe it here so the macOS answer is on the
    // record either way; the oversized path below bypasses it entirely.
    // -----------------------------------------------------------------------
    ViewRect ask{ 0, 0, crashRight, crashBottom };
    const tresult csc = view->checkSizeConstraint(&ask);
    std::printf("\ncheckSizeConstraint(0, 0, %d, %d) -> %s, rect now %d x %d %s\n",
        crashRight, crashBottom,
        (csc == kResultTrue) ? "kResultTrue" : "kResultFalse",
        ask.right - ask.left, ask.bottom - ask.top,
        (ask.right == crashRight && ask.bottom == crashBottom)
            ? "(UNCHANGED -- host learns nothing)" : "(adjusted -- host has a usable answer)");

    // -----------------------------------------------------------------------
    // The crash input, and the degenerate/over-limit rects the Windows harness
    // also refuses. Each one is resize + forced paint, because on macOS the
    // resize alone allocates nothing.
    //
    // For reference, the backing bitmap the default rect implies:
    //   2178 * 32672 * 8 bytes (16-bit RGBA) = 569 MB, before the 32-bit float
    //   fallback in initBackingBitmap doubles it.
    // -----------------------------------------------------------------------
    resizeAndPaint(view, child, crashRight, crashBottom, "the rect from the Windows crash dump");
    resizeAndPaint(view, child, 0, 0,         "degenerate 0 x 0");
    resizeAndPaint(view, child, 1, 1,         "degenerate 1 x 1");
    resizeAndPaint(view, child, 16385, 600,   "over-limit width");
    resizeAndPaint(view, child, 600, 16385,   "over-limit height");

    // Back to something sane, and prove the editor still draws afterwards --
    // a clamp that turned resize into a permanent no-op would pass everything
    // above and still be a regression.
    const int recoverDistinct = resizeAndPaint(view, child, w0, h0, "recover to the original size");
    const bool recovered = recoverDistinct > 1;
    std::printf("\nstill draws after all of that: %s (distinct colours %d)\n",
        recovered ? "YES" : "NO", recoverDistinct);

    view->removed();
    view->setFrame(nullptr);
    view->release();
    controller->terminate();
    controller->release();
    component->terminate();
    component->release();
    [window close];

    if (!live)
    {
        std::fprintf(stderr,
            "\nINCONCLUSIVE: the editor was not provably live, so surviving proves nothing.\n");
        return 3;
    }

    if (!recovered)
    {
        std::fprintf(stderr,
            "\nFAIL: survived, but the editor no longer draws -- resize became a no-op.\n");
        return 1;
    }

    std::printf("\nPASS: editor was live, survived every oversized resize+paint, and still draws.\n");
    } // @autoreleasepool

    return 0;
}
