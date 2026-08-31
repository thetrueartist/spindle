#!/usr/bin/env python3
"""Generate spindle.ico. Standard library only -- no Pillow, no dependencies.

The icon is the application's own visual language rather than a generic disc:
an asymmetric treemap in the category palette, amber dominant to match the
accent.

FRAME ENCODING MATTERS. The ICO container allows a frame to be either an
uncompressed DIB or a PNG, but Windows only reliably decodes PNG at 256x256.
Writing the small frames as PNG -- which is what image libraries tend to do by
default -- makes LoadImage fail outright, and the application silently falls
back to the stock Windows icon. So: DIB up to 128, PNG only at 256.
"""

import struct
import zlib

INK = (0x12, 0x16, 0x1C, 255)

# Same palette the application uses, so the icon and the treemap agree.
AMBER = (0xE8, 0xA3, 0x3D, 255)
BLUE = (0x4F, 0x9D, 0xD9, 255)
VIOLET = (0x7B, 0x6C, 0xD9, 255)
TEAL = (0x4F, 0xC4, 0xC4, 255)
GREEN = (0x52, 0xB7, 0x88, 255)

# Normalised block layout: a plausible squarified split, so it reads as a real
# treemap rather than a decorative grid.
BLOCKS = [
    (0.00, 0.00, 0.56, 0.58, AMBER),
    (0.56, 0.00, 1.00, 0.34, BLUE),
    (0.56, 0.34, 1.00, 0.58, VIOLET),
    (0.00, 0.58, 0.36, 1.00, TEAL),
    (0.36, 0.58, 1.00, 1.00, GREEN),
]

# Nested subdivision inside the dominant block, drawn only where there are
# enough pixels for it to read as nesting instead of as a smudge.
NESTED = (0.06, 0.34, 0.36, 0.52)

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def shade(rgba, factor):
    r, g, b, a = rgba
    return (min(255, int(r * factor)), min(255, int(g * factor)),
            min(255, int(b * factor)), a)


def render(size):
    """Render one frame as a list of RGBA tuples, row-major, top-down.

    Supersampled so the rounded corners are smooth; blocks land on
    supersample-grid boundaries so their edges stay crisp through the
    downsample.
    """
    ss = 4
    big = size * ss
    px = [(0, 0, 0, 0)] * (big * big)

    radius = big * 0.16
    r2 = radius * radius

    def inside_round(x, y):
        # Only the four corner boxes need the distance test.
        if x < radius:
            cx = radius
        elif x > big - radius:
            cx = big - radius
        else:
            return True
        if y < radius:
            cy = radius
        elif y > big - radius:
            cy = big - radius
        else:
            return True
        dx = x + 0.5 - cx
        dy = y + 0.5 - cy
        return dx * dx + dy * dy <= r2

    for y in range(big):
        row = y * big
        for x in range(big):
            if inside_round(x, y):
                px[row + x] = INK

    frac = 0.055 if size <= 24 else 0.09
    pad = max(1, round(size * frac)) * ss
    gap = max(1, round(size * 0.045)) * ss
    inner = big - pad * 2

    def fill(x0, y0, x1, y1, colour):
        for yy in range(max(0, y0), min(big, y1)):
            row = yy * big
            for xx in range(max(0, x0), min(big, x1)):
                if px[row + xx][3]:          # stay inside the rounded tile
                    px[row + xx] = colour

    for bx0, by0, bx1, by1, colour in BLOCKS:
        left = pad + int(bx0 * inner)
        top = pad + int(by0 * inner)
        right = pad + int(bx1 * inner) - gap
        bottom = pad + int(by1 * inner) - gap
        if right <= left or bottom <= top:
            continue
        fill(left, top, right, bottom, colour)
        # Top-lit edge, matching the sheen the renderer puts on every cell.
        if size >= 32:
            fill(left, top, right, top + ss, shade(colour, 1.18))

    if size >= 48:
        nx0, ny0, nx1, ny1 = NESTED
        fill(pad + int(nx0 * inner), pad + int(ny0 * inner),
             pad + int(nx1 * inner), pad + int(ny1 * inner),
             shade(AMBER, 0.62))

    # Box downsample, averaging in premultiplied space so edge pixels do not
    # pick up colour from fully transparent neighbours.
    out = []
    n = ss * ss
    for y in range(size):
        for x in range(size):
            ar = ag = ab = aa = 0
            for sy in range(ss):
                base = (y * ss + sy) * big + x * ss
                for sx in range(ss):
                    r, g, b, a = px[base + sx]
                    ar += r * a
                    ag += g * a
                    ab += b * a
                    aa += a
            if aa == 0:
                out.append((0, 0, 0, 0))
            else:
                out.append((ar // aa, ag // aa, ab // aa, aa // n))
    return out


def dib_frame(pixels, size):
    """A BITMAPINFOHEADER frame: 32bpp BGRA, bottom-up, plus an AND mask."""
    header = struct.pack(
        "<IiiHHIIiiII",
        40,          # biSize
        size,        # biWidth
        size * 2,    # biHeight -- doubled: colour data then the AND mask
        1,           # biPlanes
        32,          # biBitCount
        0,           # biCompression = BI_RGB
        0, 0, 0, 0, 0,
    )

    body = bytearray()
    for y in range(size - 1, -1, -1):        # bottom-up
        row = y * size
        for x in range(size):
            r, g, b, a = pixels[row + x]
            body += bytes((b, g, r, a))      # BGRA

    # AND mask: 1 bit per pixel, rows padded to 4 bytes. With a 32-bit alpha
    # channel modern Windows ignores it, but the frame is malformed without
    # one and some code paths still read it.
    stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for y in range(size - 1, -1, -1):
        bits = bytearray(stride)
        row = y * size
        for x in range(size):
            if pixels[row + x][3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits

    return bytes(header) + bytes(body) + bytes(mask)


def png_frame(pixels, size):
    """A minimal RGBA PNG. Only used at 256x256, where ICO permits it."""
    raw = bytearray()
    for y in range(size):
        raw.append(0)                        # filter type 0 (None)
        row = y * size
        for x in range(size):
            r, g, b, a = pixels[row + x]
            raw += bytes((r, g, b, a))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
            chunk(b"IEND", b""))


def main():
    frames = []
    for size in SIZES:
        pixels = render(size)
        # PNG only at 256; anything smaller must be a DIB or Windows will not
        # load it, and the application ends up with the stock icon.
        data = png_frame(pixels, size) if size == 256 else dib_frame(pixels, size)
        frames.append((size, data))
        print(f"  {size:3}x{size:<3} {'PNG' if size == 256 else 'DIB'} "
              f"{len(data):7} bytes")

    out = bytearray(struct.pack("<HHH", 0, 1, len(frames)))
    offset = 6 + 16 * len(frames)
    for size, data in frames:
        out += struct.pack(
            "<BBBBHHII",
            0 if size == 256 else size,      # 0 means 256 in an ICO entry
            0 if size == 256 else size,
            0,                               # palette entries
            0,                               # reserved
            1,                               # colour planes
            32,                              # bits per pixel
            len(data),
            offset,
        )
        offset += len(data)
    for _, data in frames:
        out += data

    with open("build/spindle.ico", "wb") as f:
        f.write(out)
    print(f"wrote build/spindle.ico  ({len(out)} bytes, {len(frames)} frames)")


if __name__ == "__main__":
    main()
