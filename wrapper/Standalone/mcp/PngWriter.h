#pragma once

// PNG encode for the screenshot verb.
//
// The source is the app's OWN Wayland shm buffer - the exact pixels handed to
// the compositor. That matters more than it sounds: every desktop route to a
// screenshot on Wayland goes through the compositor, and GNOME refuses both
// org.gnome.Shell.Screenshot and the xdg-desktop-portal one to an unattended
// caller ("Screenshot is not allowed"). Reading our own buffer needs no
// permission, no portal, and no user sitting there to approve a dialog, so it
// works from a script and over ssh.
//
// libpng rather than a hand-rolled encoder because the Standalone wrapper
// already links it on Linux (see the `png` entry in its target_link_libraries)
// for gmpi_ui's image decoding. Neither other platform has libpng in this build
// and neither needs one: WIC is already linked for the Direct2D backend's image
// loading, and ImageIO for the Cocoa backend's, so those two arms encode
// through the framework that is there rather than adding a dependency. One
// writePng, three encoders, identical input - see the format note on the
// function itself.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
// Both guards before windows.h: NOMINMAX because the min/max macros turn
// every std::min/std::max in a file that includes this one into a syntax
// error, and LEAN_AND_MEAN because nothing here wants the shell, RPC or
// winsock headers. Spelled with #undef first, the way gmpi_ui's own headers
// do, so it does not matter who got here first.
#undef  WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#undef  NOMINMAX
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#else
#include <png.h>
#endif

namespace gmpi
{
namespace standalone
{
namespace mcp
{

/// Writes a BGRX/XRGB8888 buffer (the Wayland shm format) as an RGB8 PNG.
///
/// `stride` is in bytes and may exceed width*4, so rows are copied one at a
/// time rather than the whole block at once.
///
/// The X byte is dropped rather than written as alpha. The surface is opaque -
/// the comment on ShmBuffer spells out that there is no alpha for the
/// compositor to blend - and an alpha channel carrying undefined padding shows
/// up as a fully transparent PNG in any viewer that honours it.
#if defined(__APPLE__)

/// The same contract, encoded through ImageIO.
///
/// The source rows are still BGRX8888 - mac/FrameCapture renders into exactly
/// that layout so this file needs no third pixel path - and they are narrowed
/// to 24bpp RGB here for the same reason the other two arms do it: the X byte
/// is padding, and handing it over as alpha produces a PNG that any viewer
/// honouring alpha draws as fully transparent.
///
/// CGImageDestination rather than NSBitmapImageRep: this header is included by
/// plain C++ translation units (mcp/CommandDispatcher.cpp), so it may use
/// CoreGraphics and CoreFoundation but not AppKit or Objective-C.
inline bool writePng(const std::string& path,
                     const uint8_t* pixels,
                     int width,
                     int height,
                     int stride,
                     std::string& errorOut)
{
    if (!pixels || width <= 0 || height <= 0)
    {
        errorOut = "no pixels to write (the window may not have been drawn yet)";
        return false;
    }

    // Packed tightly, because CGDataProvider wants one contiguous block and the
    // source stride may be wider than the row.
    const size_t outStride = static_cast<size_t>(width) * 3;
    std::vector<uint8_t> rgb(outStride * static_cast<size_t>(height));

    for (int y = 0; y < height; ++y)
    {
        const uint8_t* src = pixels + static_cast<size_t>(y) * static_cast<size_t>(stride);
        uint8_t* dst = rgb.data() + static_cast<size_t>(y) * outStride;
        for (int x = 0; x < width; ++x)
        {
            dst[x * 3 + 0] = src[x * 4 + 2];   // R
            dst[x * 3 + 1] = src[x * 4 + 1];   // G
            dst[x * 3 + 2] = src[x * 4 + 0];   // B
        }
    }

    // No-copy provider over `rgb`, which outlives every use of the image below.
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr, rgb.data(), rgb.size(), nullptr);
    if (!provider)
    {
        errorOut = "could not wrap the pixels for encoding";
        return false;
    }

    // sRGB named explicitly. The capture has already been converted out of the
    // frame's linear working space, so tagging it with anything else - or with
    // the display's profile - would make the file disagree with the Windows and
    // Linux encoders, which both emit untagged sRGB.
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image = CGImageCreate(
        static_cast<size_t>(width), static_cast<size_t>(height),
        8, 24, outStride,
        colorSpace,
        kCGBitmapByteOrderDefault | kCGImageAlphaNone,
        provider, nullptr, false, kCGRenderingIntentDefault);

    CGColorSpaceRelease(colorSpace);
    CGDataProviderRelease(provider);

    if (!image)
    {
        errorOut = "could not build an image from the captured pixels";
        return false;
    }

    CFStringRef pathStr = CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8);
    CFURLRef url = pathStr ? CFURLCreateWithFileSystemPath(nullptr, pathStr, kCFURLPOSIXPathStyle, false)
                           : nullptr;
    if (pathStr)
        CFRelease(pathStr);

    if (!url)
    {
        CGImageRelease(image);
        errorOut = "'" + path + "' is not a usable file path";
        return false;
    }

    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    CFRelease(url);

    if (!dest)
    {
        CGImageRelease(image);
        errorOut = "could not open '" + path + "' for writing";
        return false;
    }

    CGImageDestinationAddImage(dest, image, nullptr);
    const bool ok = CGImageDestinationFinalize(dest);

    CFRelease(dest);
    CGImageRelease(image);

    if (!ok)
        errorOut = "write failed (disk full, or the directory does not exist?)";

    return ok;
}

#elif !defined(_WIN32)

inline bool writePng(const std::string& path,
                     const uint8_t* pixels,
                     int width,
                     int height,
                     int stride,
                     std::string& errorOut)
{
    if (!pixels || width <= 0 || height <= 0)
    {
        errorOut = "no pixels to write (the window may not have been drawn yet)";
        return false;
    }

    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
    {
        errorOut = "could not open '" + path + "' for writing";
        return false;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png)
    {
        fclose(f);
        errorOut = "png_create_write_struct failed";
        return false;
    }

    png_infop info = png_create_info_struct(png);
    if (!info)
    {
        png_destroy_write_struct(&png, nullptr);
        fclose(f);
        errorOut = "png_create_info_struct failed";
        return false;
    }

    // libpng reports errors by longjmp'ing here. Everything that needs
    // releasing on that path is either declared above it or has trivial
    // destruction, so there is nothing for the jump to skip.
    if (setjmp(png_jmpbuf(png)))
    {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        errorOut = "libpng failed while writing";
        return false;
    }

    png_init_io(png, f);
    png_set_IHDR(png, info, static_cast<png_uint_32>(width), static_cast<png_uint_32>(height),
                 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<uint8_t> row(static_cast<size_t>(width) * 3);

    for (int y = 0; y < height; ++y)
    {
        const uint8_t* src = pixels + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x < width; ++x)
        {
            // XRGB8888 is a 32-bit little-endian word, so in memory the bytes
            // run B,G,R,X.
            row[x * 3 + 0] = src[x * 4 + 2];   // R
            row[x * 3 + 1] = src[x * 4 + 1];   // G
            row[x * 3 + 2] = src[x * 4 + 0];   // B
        }
        png_write_row(png, row.data());
    }

    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);

    const bool ok = (ferror(f) == 0);
    fclose(f);

    if (!ok)
        errorOut = "write failed (disk full?)";

    return ok;
}

#else // _WIN32

/// The same contract, encoded through WIC.
///
/// The source rows are still BGRX8888 - the Windows frame grabber converts the
/// swap chain into exactly the layout the Wayland compositor hands us, so the
/// dispatcher above this and the MCP client above that see one pixel format on
/// every platform.
///
/// 24bpp BGR rather than 32bpp: the X byte is padding, and asking WIC to write
/// it as alpha produces a PNG that viewers honouring the channel render as
/// fully transparent. Same reasoning as the libpng path's PNG_COLOR_TYPE_RGB.
inline bool writePng(const std::string& path,
                     const uint8_t* pixels,
                     int width,
                     int height,
                     int stride,
                     std::string& errorOut)
{
    if (!pixels || width <= 0 || height <= 0)
    {
        errorOut = "no pixels to write (the window may not have been drawn yet)";
        return false;
    }

    // COM is already initialised on the thread that runs commands (the app's UI
    // thread, an STA). CoCreateInstance would fail cleanly if it were not, and
    // that failure is reported rather than papered over with a local init.
    IWICImagingFactory* factory{};
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IWICImagingFactory),
                                  reinterpret_cast<void**>(&factory))) || !factory)
    {
        errorOut = "the imaging component could not be created";
        return false;
    }

    const int outStride = width * 3;
    std::vector<uint8_t> rows(static_cast<size_t>(outStride) * height);

    for (int y = 0; y < height; ++y)
    {
        const uint8_t* src = pixels + static_cast<size_t>(y) * static_cast<size_t>(stride);
        uint8_t* dst = rows.data() + static_cast<size_t>(y) * outStride;

        for (int x = 0; x < width; ++x)
        {
            // 24bppBGR wants B,G,R in memory order, which is what the source
            // already holds minus its padding byte.
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }

    IWICStream* stream{};
    IWICBitmapEncoder* encoder{};
    IWICBitmapFrameEncode* frame{};
    IPropertyBag2* props{};

    bool ok = false;

    const std::wstring wide = [&path]
    {
        const int size = ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(size), 0);
        ::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), out.data(), size);
        return out;
    }();

    if (SUCCEEDED(factory->CreateStream(&stream))
        && SUCCEEDED(stream->InitializeFromFilename(wide.c_str(), GENERIC_WRITE))
        && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
        && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache))
        && SUCCEEDED(encoder->CreateNewFrame(&frame, &props))
        && SUCCEEDED(frame->Initialize(props)))
    {
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;

        ok = SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))
          && SUCCEEDED(frame->SetPixelFormat(&format))
          // The encoder is free to refuse the requested format and pick its
          // own; writing regardless would silently mis-order every channel.
          && format == GUID_WICPixelFormat24bppBGR
          && SUCCEEDED(frame->WritePixels(static_cast<UINT>(height),
                                          static_cast<UINT>(outStride),
                                          static_cast<UINT>(rows.size()),
                                          rows.data()))
          && SUCCEEDED(frame->Commit())
          && SUCCEEDED(encoder->Commit());
    }

    if (props)   props->Release();
    if (frame)   frame->Release();
    if (encoder) encoder->Release();
    if (stream)  stream->Release();
    factory->Release();

    if (!ok)
        errorOut = "could not write '" + path + "'";

    return ok;
}

#endif // _WIN32

} // namespace mcp
} // namespace standalone
} // namespace gmpi
