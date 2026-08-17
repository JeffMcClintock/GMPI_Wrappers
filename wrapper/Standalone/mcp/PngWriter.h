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
// already links it (see the `png` entry in its target_link_libraries) for
// gmpi_ui's image decoding.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <png.h>

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

} // namespace mcp
} // namespace standalone
} // namespace gmpi
