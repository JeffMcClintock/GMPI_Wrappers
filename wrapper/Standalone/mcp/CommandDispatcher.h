#pragma once

// The verb dispatcher for the standalone's command channel.
//
// One line in, one JSON object out. Deliberately host-agnostic in the same way
// SynthEdit's EditorCommandDispatcher is: it takes what it needs through the
// context below rather than reaching for globals, calls no exit(), and reports
// every failure in-band. That is what lets the same verbs be driven by a test,
// by the MCP server, or by `socat` from a terminal.
//
// ALWAYS INVOKED ON THE MAIN THREAD, via MainThreadQueue. Nothing here takes a
// lock or worries about the audio thread, with the single exception of MIDI
// injection - which goes through the host's existing lock-free FIFO, exactly
// where a real MIDI device's bytes enter.

#include <functional>
#include <string>

#include "GmpiApiDrawing.h"
#include "helpers/NativeUi.h"

namespace gmpi
{
namespace standalone
{

class StandaloneHost;

namespace mcp
{

/// What the dispatcher is allowed to touch. Supplied by the platform layer,
/// because only it knows what a window is.
struct AppContext
{
    StandaloneHost* host{};

    /// The app's own on-screen pixels, in the compositor's XRGB8888 layout.
    /// Returns false when there is no frame to hand over: everywhere before the
    /// first one, and on macOS - whose shell has no readable frame of its own,
    /// only what it last drew for a screenshot - also unforced after a resize
    /// (mac/FrameCapture.h says why).
    ///
    /// Takes a redraw flag rather than always presenting, because a screenshot
    /// wants the very latest frame - so a parameter set on the line before is
    /// visible - and a verb that merely reads the frame does not want to make
    /// the whole editor paint to answer.
    ///
    /// ONLY A SCREENSHOT ASKS FOR PIXELS. Anything after the window's
    /// dimensions asks canvasSize below, which is why --info neither paints nor
    /// depends on a frame existing.
    ///
    /// Which leaves exactly one caller, passing TRUE - so the unforced path is
    /// currently unexercised, and worth checking by hand before relying on it.
    /// It is kept rather than removed because each shell answers it meaningfully
    /// and differently (Wayland hands back the buffer it has already presented,
    /// without asking the CPU backend to redraw it), and because "the frame as
    /// it stands, cheaply" is the natural shape of the next verb that wants
    /// pixels repeatedly.
    std::function<bool(bool forceRedraw,
                       const uint8_t*& pixels, int& width, int& height, int& stride)> framePixels;

    /// Window size in PIXELS - the space a screenshot is measured in, and the
    /// dimensions framePixels would report.
    ///
    /// Separate from framePixels because it is a question about the WINDOW, not
    /// about a frame: a window has a size from the moment it exists, painted
    /// into or not. Deriving it from a captured frame instead made --info's
    /// answer depend on a screenshot having been taken first, which on macOS is
    /// the only thing that fills the capture bitmap at all.
    ///
    /// Zero on both before there is a window.
    std::function<void(int& width, int& height)> canvasSize;

    /// Where synthetic pointer events enter: the SAME IInputClient a real
    /// mouse reaches, so hover state, mouse capture and the menu bar all
    /// behave as they do under a hand.
    std::function<gmpi::api::IInputClient*()> inputClient;

    /// Window size in logical DIPs - the space pointer coordinates are in.
    /// Differs from the pixel dimensions above under fractional scaling, which
    /// is why both are reported and never conflated.
    std::function<void(float& width, float& height)> logicalSize;

    /// Top of the plugin's own editor within the window, in DIPs. The menu bar
    /// occupies the strip above it. Reported so a caller reasoning in the
    /// plugin's coordinate space can convert; the verbs themselves take window
    /// coordinates, because that is the space a screenshot is measured in.
    float editorOriginY = 0.0f;
};

/// Runs one command line. Never throws; a malformed line comes back as
/// {"ok":false,...} like any other failure.
std::string dispatchCommand(AppContext& context, const std::string& line);

} // namespace mcp
} // namespace standalone
} // namespace gmpi
