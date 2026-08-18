// The GMPI standalone app on Linux/Wayland.
//
// No toolkit and no X11: the window, input, menus and dialogs come from
// gmpi_ui's Wayland backend and everything is drawn by the CPU backend. The
// plugin sees exactly what it sees in a DAW - a Processor driven by process()
// and an Editor attached to a drawing host - with an audio device and a MIDI
// port standing in for the host.
//
// The counterpart of windows/MainWin32.cpp and mac/MainMac.mm, and deliberately
// the same program - now literally so. The startup sequence, the menus, the
// tick and the teardown are ONE copy, in ../StandaloneApp.cpp; what is left in
// this file is the answers only this platform can give, in the order that file
// asks for them.
//
// Wayland-only, deliberately. A build that silently fell back to XWayland
// would hide the problems this target exists to find, and X11 hosts are the
// VST3 wrapper's job, not this one's.
//
// Two things this file owns that a plugin never does:
//
//   * the event loop. Neither timer framework has a native source here, and the
//     frame has no render timer of its own, so this loop's tick drives both -
//     parameter queues, editor animation, preGraphicsRedraw and the deferred
//     settings apply all hang off it. Both facts are stated once, in
//     backendServices() below, and the portable tick reads them from there.
//   * the connection. In a plugin both belong to the host; that is why the
//     backend takes them as constructor arguments rather than creating them.

#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "AudioDriverPipeWire.h"
#include "MidiDriverAlsa.h"

#include "../StandaloneApp.h"

#include "GmpiUiDrawing.h"
#include "backends/DrawingFrameWayland.h"
#include "helpers/CpuTextEngine.h"
#include "helpers/DecodeImage.h"
#include "helpers/FontProvider.h"

namespace
{

using namespace gmpi::standalone;

// The CPU backend deliberately ships no font or shaping code, so the app wires
// in the three pieces every gmpi_ui host does: fontconfig to find faces,
// freetype to rasterise, harfbuzz to shape. Unlike SynthEdit this does NOT
// bundle a face - a standalone has no reference images to match, and the
// desktop's own UI font is the right thing to look like.
gmpi::drawing::TextFormat wireTextStack(gmpi::wayland::WaylandToplevel& frame,
                                        gmpi::drawing::Factory& facade)
{
    static gmpi::drawing::CpuTextEngine textEngine{ gmpi::drawing::findFont };

    textEngine.imageDecoder             = gmpi::drawing::decodeImageMemory;
    frame.drawingFactory().imageDecoder = gmpi::drawing::decodeImageFile;
    frame.drawingFactory().textEngine   = &textEngine;

    *gmpi::drawing::AccessPtr::put(facade) = &frame.drawingFactory();

    const std::string_view family{ "sans-serif" };
    return facade.createTextFormat(14.0f, std::span{ &family, 1 });
}

// Everything ../StandaloneApp.cpp needs to know about Wayland.
//
// The methods are in the order PlatformShell declares them, which is the order
// they are called in; windows/MainWin32.cpp and mac/MainMac.mm carry the same
// list.
class WaylandShell final : public PlatformShell
{
public:
    bool createWindow(const std::string& title, int clientWidthDips, int clientHeightDips) override
    {
        if (!connection_.open())
        {
            lastError_ = title + ": no Wayland display. This build is Wayland-only;\n"
                                 "under X11 or a remote session there is nothing to connect to.";
            return false;
        }

        // BEFORE create(), which is the opposite of the Windows shell and for
        // the opposite reason: there the font comes out of the frame's own
        // DirectWrite factory and cannot exist until the frame does, whereas
        // here the frame has to be given a font it can draw its popup menus
        // with, and those exist from the first configure onwards.
        menuFont_ = wireTextStack(frame_, facade_);
        if (!menuFont_)
            reportStatus("No usable font was found; text will not draw.");

        frame_.setMenuFont(gmpi::drawing::AccessPtr::get(menuFont_));

        return frame_.create(title.c_str(), "com.gmpi.standalone", clientWidthDips, clientHeightDips);
    }

    std::string lastError() const override { return lastError_; }

    void setMinimumClientSize(int widthDips, int heightDips) override
    {
        // A request, not a clamp: the client cannot refuse a configure, so only
        // the compositor can actually stop the drag.
        frame_.setMinimumSize(widthDips, heightDips);
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
        frame_.setFallbackHost(parameterHost);
        frame_.attachClient(client);
        return true;
    }

    void showAndPaint() override
    {
        // Nothing to do: the surface becomes visible on its first configure, and
        // the frame paints on the callback that follows. Only macOS has to force
        // a paint here, and only because of the microphone prompt.
    }

    void requestClose() override
    {
        // Ends runEventLoop on its next turn; nothing is torn down here.
        frame_.close();
    }

    void closeWindow() override
    {
        // Only the client. The surface itself goes with frame_ in
        // ~WaylandShell, because a Wayland client's window IS its surface and
        // tearing it down before ~Connection's cleanup roundtrip is what
        // segfaults mutter.
        frame_.detachClient();
    }

    std::unique_ptr<AudioDriver> createAudioDriver() override
    {
        return std::make_unique<AudioDriverPipeWire>();
    }

    std::unique_ptr<MidiDriver> createMidiDriver() override
    {
        return std::make_unique<MidiDriverAlsa>();
    }

    BackendServices backendServices() const override
    {
        // Neither timer framework has a native source here, and the frame has no
        // render timer of its own - so the app's tick is the source of both.
        return { TimerSource::appTick, RedrawClientDriver::appTick };
    }

    int runEventLoop(const std::function<void(int elapsedMs)>& onTick) override
    {
        // The loop already takes exactly this callback, elapsed time and all,
        // so there is no Ticker here and nothing to adapt. It returns void:
        // a Wayland client has no exit code of its own to report.
        frame_.runEventLoop(Ticker::kIntervalMs, onTick);
        return 0;
    }

    void reportStatus(const std::string& message) override
    {
        std::fprintf(stderr, "%s\n", message.c_str());
    }

    // No showFatalAlert override: stderr is the whole report on this platform,
    // and PlatformShell::showFatalAlert says why.

#if GMPI_STANDALONE_COMMAND_CHANNEL
    bool framePixels(bool forceRedraw,
                     const uint8_t*& pixels, int& width, int& height, int& stride) override
    {
        // No readback and no second draw, unlike the other two shells: the app
        // drew into an shm buffer it owns, so the bytes it handed the
        // compositor are simply already here.
        //
        // A screenshot must show the effect of the command before it, so it
        // renders rather than reading a frame that predates the change.
        // present() early-outs before the first configure, so this is safe even
        // if a client connects while the window is still opening.
        if (forceRedraw)
            frame_.present();

        const auto& buffer = frame_.frameBuffer();
        if (!buffer.pixels())
            return false;

        pixels = buffer.pixels();
        width  = buffer.width();
        height = buffer.height();
        stride = buffer.stride();
        return true;
    }

    void logicalSize(float& width, float& height) override
    {
        width  = static_cast<float>(frame_.logicalWidth());
        height = static_cast<float>(frame_.logicalHeight());
    }
#endif

private:
    // MEMBER ORDER IS THE CONTRACT, and this is the one the compositor cares
    // about: connection_ is declared FIRST so it is destroyed LAST, which leaves
    // ~WaylandToplevel a live display to make its cleanup roundtrip on. These
    // used to be locals in main(), in this same order, where the ordering was a
    // property of the statements rather than of the declarations.
    //
    // Constructing frame_ before connection_.open() is safe: WaylandFrameBase's
    // constructor only stores the reference (DrawingFrameWayland.h:651).
    gmpi::wayland::Connection      connection_;
    gmpi::wayland::WaylandToplevel frame_{ connection_ };

    // After the frame, so both are gone before it: facade_ wraps the frame's own
    // factory, and menuFont_ was minted by it.
    gmpi::drawing::Factory         facade_;
    gmpi::drawing::TextFormat      menuFont_;

    std::string                    lastError_;
};

} // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    // Constructed here rather than inside runStandaloneApp so that the display
    // connection and the surface outlive it - the app is torn down first, and
    // only then, as this frame unwinds, the connection it ran on.
    WaylandShell shell;
    return gmpi::standalone::runStandaloneApp(shell);
}
