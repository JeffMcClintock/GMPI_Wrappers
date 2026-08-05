#!/usr/bin/env python3
"""Decide whether the plugin actually drew, by diffing two screenshots of the
nested compositor: one taken while the host window is mapped and plugin-free,
one after the plugin's editor has attached.

The plugin's buffers go straight to the compositor, so the test host cannot
inspect them; and nothing moves between the two captures, so every changed
pixel came from the plugin's subsurface.

Three things here were learned the hard way:

  * counting distinct colours proves nothing. The desktop wallpaper alone has
    thousands, and the first version of this check passed while the plugin area
    was completely blank.

  * deriving the window box from the host's own fill colour does not work
    either: the plugin covers part of that fill, so the box shrinks to whatever
    was left over. It reported a perfectly working 400x200 analyser as having
    drawn nothing, and only passed for one plugin by luck of layout.

  * bytes-per-pixel comes from bits_per_pixel, NOT bytes_per_line/width. These
    captures report 24bpp with a padded 5600-byte stride for 1400 pixels;
    assuming 4 squashed the x axis by exactly 3/4 and gave a plausible-looking
    but wrong window position.
"""
import struct
import sys

MIN_PLUGIN_PIXELS = 1000


def read_xwd(path):
    data = open(path, 'rb').read()

    # XWD: big-endian header of 25 words, then the window name (header_size
    # covers it), then an optional colormap, then the pixels.
    h = struct.unpack('>25I', data[:100])
    header_size, width, height = h[0], h[4], h[5]
    bits_per_pixel, bytes_per_line, ncolors = h[11], h[12], h[19]

    bpp = bits_per_pixel // 8
    off = header_size + ncolors * 12

    rows = []
    for y in range(height):
        start = off + y * bytes_per_line
        rows.append(data[start:start + width * bpp])

    return width, height, bpp, rows


def main(before_path: str, after_path: str) -> int:
    bw, bh, bbpp, before = read_xwd(before_path)
    aw, ah, abpp, after = read_xwd(after_path)

    if (bw, bh, bbpp) != (aw, ah, abpp):
        print(f"FAIL: captures differ in geometry "
              f"({bw}x{bh}@{bbpp} vs {aw}x{ah}@{abpp})")
        return 1

    changed = 0
    xs, ys = [], []
    for y in range(bh):
        rb, ra = before[y], after[y]
        if rb == ra:
            continue
        for x in range(bw):
            i = x * bbpp
            if rb[i:i + 3] != ra[i:i + 3]:
                changed += 1
                xs.append(x)
                ys.append(y)

    if not changed:
        print("FAIL: nothing on screen changed when the editor attached - "
              "the plugin put no pixels up at all")
        return 1

    print(f"pixels changed by the editor: {changed}, "
          f"within {min(xs)},{min(ys)}..{max(xs)},{max(ys)} "
          f"({max(xs) - min(xs) + 1}x{max(ys) - min(ys) + 1})")

    if changed < MIN_PLUGIN_PIXELS:
        print(f"FAIL: only {changed} pixels changed - too few to be an editor")
        return 1

    return 0


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("usage: check_wayland_capture.py <before.xwd> <after.xwd>")
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
