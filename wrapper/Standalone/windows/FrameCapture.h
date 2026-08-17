#pragma once

// Reads the app's own window back out of its swap chain.
//
// This is what makes the command channel's --screenshot verb work on Windows,
// and it is the ONE piece of that channel that could not simply be shared with
// Linux: there, the app draws into a shm buffer it owns and can read the bytes
// it just handed the compositor. Here they live on the GPU.
//
// Output is BGRX8888 - the exact layout the Wayland shell reports - so
// everything above this (mcp/CommandDispatcher, mcp/PngWriter, and the MCP
// client beyond them) sees one pixel format on every platform.
//
// Two conversions happen on the way:
//
//   * The swap chain is normally DXGI_FORMAT_R16G16B16A16_FLOAT holding LINEAR
//     scRGB, because that is what gives gamma-correct blending. A PNG wants
//     sRGB, so each channel goes through gmpi_ui's own linearToSRGB01 - the
//     same curve the CPU backend's screen encoder uses, so a Windows
//     screenshot and a Linux one of the same GUI agree.
//   * On an HDR display the frame was multiplied by the monitor's SDR white
//     level before presentation. That is a property of the display, not of the
//     image, so it is divided back out - otherwise the same plugin would
//     screenshot brighter on an HDR monitor than on an SDR one.
//
// Why the swap chain and not a fresh render target: re-rendering the client
// into a bitmap of our own would draw it with device resources it did not
// cache against, which is exactly the "wrong resource domain" class of bug
// D2D reports at the worst possible moment. Reading back what was actually
// presented also means a screenshot cannot disagree with the screen.

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>

#include "backends/DrawingFrameWin.h"

namespace gmpi
{
namespace standalone
{

class FrameCapture
{
public:
    explicit FrameCapture(gmpi::hosting::DrawingFrame& frame) : frame_(frame) {}

    /// Signature and semantics of mcp::AppContext::framePixels.
    ///
    /// `forceRedraw` paints first, so a screenshot taken right after
    /// --set-param shows the new value rather than the frame that predates the
    /// command. False is for callers that only want the dimensions and should
    /// not make the app paint to answer a question.
    ///
    /// The returned pointer is owned by this object and stays valid until the
    /// next call. False means there is nothing to read yet (no swap chain, or
    /// no frame drawn), or that the swap chain is in a pixel format this does
    /// not understand - the caller reports that rather than writing a wrong
    /// picture.
    bool capture(bool forceRedraw,
                 const uint8_t*& pixels, int& width, int& height, int& stride);

    /// Why the last capture() returned false. Empty when it succeeded.
    const std::string& lastError() const { return lastError_; }

private:
    bool ensureStaging(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& backBufferDesc);

    gmpi::hosting::DrawingFrame& frame_;

    gmpi::directx::ComPtr<ID3D11Texture2D> staging_;
    D3D11_TEXTURE2D_DESC stagingDesc_{};

    std::vector<uint8_t> pixels_;
    int width_  = 0;
    int height_ = 0;

    std::string lastError_;
};

} // namespace standalone
} // namespace gmpi
