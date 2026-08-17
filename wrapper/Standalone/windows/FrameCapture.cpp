#include "FrameCapture.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "GmpiUiDrawing.h"
#include "helpers/BitmapMask.h"   // gmpi::drawing::detail::halfToFloat

namespace gmpi
{
namespace standalone
{

namespace
{

uint8_t toSrgbByte(float linear)
{
    // linearToSRGB01 lives in GmpiApiDrawing.h precisely so that the screen
    // encoder and every offline encoder share one curve; using a local
    // approximation here would make screenshots disagree with the display by a
    // code value or two, which is exactly the kind of difference that wastes an
    // afternoon when comparing two platforms' renders.
    const float encoded = gmpi::drawing::linearToSRGB01(std::clamp(linear, 0.0f, 1.0f));
    return static_cast<uint8_t>(std::clamp(encoded * 255.0f + 0.5f, 0.0f, 255.0f));
}

} // namespace

bool FrameCapture::ensureStaging(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& backBufferDesc)
{
    // Reused across calls: a screenshot every frame is a plausible thing for a
    // test to do, and allocating a full-window GPU texture each time would make
    // that visibly slow.
    if (staging_
        && stagingDesc_.Width  == backBufferDesc.Width
        && stagingDesc_.Height == backBufferDesc.Height
        && stagingDesc_.Format == backBufferDesc.Format)
    {
        return true;
    }

    staging_ = {};

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width          = backBufferDesc.Width;
    desc.Height         = backBufferDesc.Height;
    desc.MipLevels      = 1;
    desc.ArraySize      = 1;
    desc.Format         = backBufferDesc.Format;
    desc.SampleDesc     = { 1, 0 };
    desc.Usage          = D3D11_USAGE_STAGING;
    desc.BindFlags      = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, staging_.put())))
    {
        staging_ = {};
        return false;
    }

    stagingDesc_ = desc;
    return true;
}

bool FrameCapture::capture(bool forceRedraw,
                           const uint8_t*& pixels, int& width, int& height, int& stride)
{
    lastError_.clear();

    if (forceRedraw)
    {
        // A full invalidate, not the queued rects: the caller wants the whole
        // window, and a partial paint would leave the rest of the staging copy
        // holding whatever the previous capture read.
        //
        // Safe before the first real frame - PaintQueuedDirtyRects early-outs
        // without a device, and the swap-chain check below catches that case.
        frame_.invalidateAll();
        frame_.PaintQueuedDirtyRects();
    }

    if (!frame_.swapChain)
    {
        lastError_ = "no frame has been drawn yet";
        return false;
    }

    gmpi::directx::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(frame_.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                           backBuffer.put_void())) || !backBuffer)
    {
        lastError_ = "the swap chain would not hand over its back buffer";
        return false;
    }

    // DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, which is what the frame creates,
    // PERSISTS the back buffer across Present - that is what lets it present
    // dirty rectangles at all. So buffer 0 here still holds the frame the user
    // is looking at, rather than undefined contents.
    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);

    gmpi::directx::ComPtr<ID3D11Device> device;
    backBuffer->GetDevice(device.put());
    if (!device)
    {
        lastError_ = "the back buffer has no device";
        return false;
    }

    if (!ensureStaging(device.get(), desc))
    {
        lastError_ = "could not allocate a readback buffer";
        return false;
    }

    gmpi::directx::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.put());
    if (!context)
    {
        lastError_ = "the device has no immediate context";
        return false;
    }

    context->CopyResource(staging_.get(), backBuffer.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped)))
    {
        lastError_ = "the readback buffer could not be mapped";
        return false;
    }

    width_  = static_cast<int>(desc.Width);
    height_ = static_cast<int>(desc.Height);

    const int outStride = width_ * 4;
    pixels_.resize(static_cast<size_t>(outStride) * height_);

    // The display's SDR white level, divided back out. 1.0 on every SDR
    // monitor, so this is a no-op in the ordinary case; guarded against zero
    // because an unqueried level is left default-constructed.
    const float whiteLevel = frame_.windowWhiteLevel > 0.0f ? frame_.windowWhiteLevel : 1.0f;
    const float inverseWhite = 1.0f / whiteLevel;

    bool ok = true;

    switch (desc.Format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        for (int y = 0; y < height_; ++y)
        {
            const auto* src = reinterpret_cast<const uint16_t*>(
                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch);
            uint8_t* dst = pixels_.data() + static_cast<size_t>(y) * outStride;

            for (int x = 0; x < width_; ++x)
            {
                const float r = gmpi::drawing::detail::halfToFloat(src[x * 4 + 0]) * inverseWhite;
                const float g = gmpi::drawing::detail::halfToFloat(src[x * 4 + 1]) * inverseWhite;
                const float b = gmpi::drawing::detail::halfToFloat(src[x * 4 + 2]) * inverseWhite;

                // Alpha is not carried. The window is opaque - the client
                // paints every pixel of it - and the destination format's
                // fourth byte is padding, which is what XRGB8888 means.
                dst[x * 4 + 0] = toSrgbByte(b);
                dst[x * 4 + 1] = toSrgbByte(g);
                dst[x * 4 + 2] = toSrgbByte(r);
                dst[x * 4 + 3] = 0xff;
            }
        }
        break;

    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        // The 8-bit fallback swap chain, already sRGB-encoded and already in
        // the destination's channel order.
        for (int y = 0; y < height_; ++y)
        {
            const auto* src = static_cast<const uint8_t*>(mapped.pData)
                            + static_cast<size_t>(y) * mapped.RowPitch;
            uint8_t* dst = pixels_.data() + static_cast<size_t>(y) * outStride;

            for (int x = 0; x < width_; ++x)
            {
                dst[x * 4 + 0] = src[x * 4 + 0];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 2];
                dst[x * 4 + 3] = 0xff;
            }
        }
        break;

    default:
        // Reported rather than guessed at. Writing a picture in the wrong
        // colour space looks plausible enough to be believed, which is worse
        // than saying no.
        lastError_ = "the swap chain is in a pixel format this build cannot read ("
                   + std::to_string(static_cast<int>(desc.Format)) + ")";
        ok = false;
        break;
    }

    context->Unmap(staging_.get(), 0);

    if (!ok)
        return false;

    pixels = pixels_.data();
    width  = width_;
    height = height_;
    stride = outStride;
    return true;
}

} // namespace standalone
} // namespace gmpi
