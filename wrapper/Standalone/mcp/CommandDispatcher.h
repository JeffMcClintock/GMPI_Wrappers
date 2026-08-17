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
    /// Returns false before the first frame has been drawn.
    ///
    /// Takes a redraw flag rather than always presenting: a screenshot wants
    /// the very latest frame (so a parameter set on the line before is
    /// visible), while --info only wants the dimensions and should not force
    /// the app to paint just to answer a question.
    std::function<bool(bool forceRedraw,
                       const uint8_t*& pixels, int& width, int& height, int& stride)> framePixels;

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
