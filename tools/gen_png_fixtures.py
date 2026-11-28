#!/usr/bin/env python3
"""PNG files for the host tests (test/host/test_png.c) and the UI preview (test/host/preview.c).

    python3 tools/gen_png_fixtures.py   ->  test/host/fixtures/*.png

The test images are written by the small encoder below, not by an imaging library, so that every
colour type, bit depth, row filter (each row uses the next of the five), split IDAT chunks and the
transparency variants are covered on purpose. The pixel patterns must match the ones in
test/host/test_png.c. The preview icons are drawn with Pillow.
"""
import os
import struct
import zlib

from PIL import Image, ImageDraw

OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "..", "test", "host", "fixtures")
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def pattern_rgba(x, y):
    return ((x * 37 + y * 11) & 255, (x * 5 + y * 53) & 255, (x * y * 3) & 255, (x * 29 + y * 7) & 255)


def write_png(name, width, height, color_type, bit_depth, pixels, palette=None, transparency=None, interlace=0):
    """pixels[y][x]: tuple of samples at bit_depth (CHANNELS[color_type] of them)."""
    channels = CHANNELS[color_type]
    filter_bytes_per_pixel = max(1, channels * bit_depth // 8)
    raw = bytearray()
    previous = bytearray((width * channels * bit_depth + 7) // 8)
    for y in range(height):
        row = pack_row(pixels[y], bit_depth)
        filter_type = y % 5
        raw.append(filter_type)
        raw += apply_filter(filter_type, row, previous, filter_bytes_per_pixel)
        previous = row
    compressed = zlib.compress(bytes(raw), 9)
    split = len(compressed) // 2
    chunks = [(b"IHDR", struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, interlace))]
    if palette:
        chunks.append((b"PLTE", b"".join(bytes(entry) for entry in palette)))
    if transparency is not None:
        chunks.append((b"tRNS", transparency))
    chunks += [(b"IDAT", compressed[:split]), (b"IDAT", compressed[split:]), (b"IEND", b"")]
    with open(os.path.join(OUTPUT_DIR, name), "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n")
        for chunk_type, data in chunks:
            file.write(struct.pack(">I", len(data)) + chunk_type + data)
            file.write(struct.pack(">I", zlib.crc32(chunk_type + data) & 0xFFFFFFFF))


def pack_row(samples_per_pixel, bit_depth):
    row = bytearray()
    if bit_depth >= 8:
        for samples in samples_per_pixel:
            for sample in samples:
                row += sample.to_bytes(bit_depth // 8, "big")
        return row
    accumulator, bits = 0, 0
    for samples in samples_per_pixel:
        for sample in samples:
            accumulator = (accumulator << bit_depth) | sample
            bits += bit_depth
            if bits == 8:
                row.append(accumulator)
                accumulator, bits = 0, 0
    if bits:
        row.append(accumulator << (8 - bits))
    return row


def apply_filter(filter_type, row, previous, bytes_per_pixel):
    filtered = bytearray(len(row))
    for i, value in enumerate(row):
        left = row[i - bytes_per_pixel] if i >= bytes_per_pixel else 0
        above = previous[i]
        above_left = previous[i - bytes_per_pixel] if i >= bytes_per_pixel else 0
        predictor = [0, left, above, (left + above) >> 1, paeth(left, above, above_left)][filter_type]
        filtered[i] = (value - predictor) & 255
    return filtered


def paeth(left, above, above_left):
    estimate = left + above - above_left
    distances = (abs(estimate - left), abs(estimate - above), abs(estimate - above_left))
    if distances[0] <= distances[1] and distances[0] <= distances[2]:
        return left
    return above if distances[1] <= distances[2] else above_left


def test_images():
    width, height = 16, 12
    write_png("rgba8.png", width, height, 6, 8, [[pattern_rgba(x, y) for x in range(width)] for y in range(height)])
    # 16-bit: the high byte is the 8-bit pattern, the low byte noise the decoder must drop
    write_png("rgba16.png", width, height, 6, 16,
              [[tuple(value * 256 + ((x + y) & 1) * 99 for value in pattern_rgba(x, y)) for x in range(width)]
               for y in range(height)])
    key = (10, 20, 30)
    write_png("rgb_key.png", 8, 8, 2, 8,
              [[key if (x + y) % 3 == 0 else pattern_rgba(x, y)[:3] for x in range(8)] for y in range(8)],
              transparency=struct.pack(">HHH", *key))
    write_png("gray_alpha.png", width, height, 4, 8,
              [[(pattern_rgba(x, y)[0], pattern_rgba(x, y)[3]) for x in range(width)] for y in range(height)])
    write_png("gray2.png", 9, 5, 0, 2, [[((x + y) % 4,) for x in range(9)] for y in range(5)],
              transparency=struct.pack(">H", 1))
    palette = [(i * 20, 255 - i * 20, i * 7) for i in range(10)]
    write_png("palette4.png", 7, 6, 3, 4, [[((x * 3 + y) % 10,) for x in range(7)] for y in range(6)],
              palette=palette, transparency=bytes(i * 50 for i in range(5)))
    write_png("scale96.png", 96, 96, 6, 8, [[scale_pattern(x, y) for x in range(96)] for y in range(96)])
    write_png("wide96x40.png", 96, 40, 6, 8, [[(1, 2, 3, 255)] * 96 for _ in range(40)])
    write_png("interlaced.png", 4, 4, 6, 8, [[(0, 0, 0, 255)] * 4 for _ in range(4)], interlace=1)
    write_png("too_large.png", 97, 10, 6, 8, [[(0, 0, 0, 255)] * 97 for _ in range(10)])


def scale_pattern(x, y):
    """96x96 of uniform 3x3 blocks (scaled 1.5x down to 64x64: each block -> 2x2 icon pixels), except
    every 7th block: an opaque red top row over two transparent blue rows (scaled: red with alpha 170
    in the upper icon row, transparent in the lower)."""
    block_x, block_y = x // 3, y // 3
    if (block_x + block_y) % 7 == 0:
        return (250, 10, 10, 255) if y % 3 == 0 else (0, 0, 255, 0)
    red, green, blue, _ = pattern_rgba(block_x, block_y)
    return (red, green, blue, 255)


def preview_icons():
    for name, color in (("bulb_on.png", (255, 214, 90, 255)), ("bulb_off.png", (110, 122, 150, 220))):
        scale = 4 * 64 / 48  # drawn large and scaled down: anti-aliased edges in the alpha channel
        image = Image.new("RGBA", (64 * 4, 64 * 4), (0, 0, 0, 0))
        drawer = ImageDraw.Draw(image)
        drawer.ellipse((10 * scale, 2 * scale, 38 * scale, 30 * scale), fill=color)
        drawer.rectangle((18 * scale, 28 * scale, 30 * scale, 38 * scale), fill=color)
        drawer.rectangle((19 * scale, 40 * scale, 29 * scale, 44 * scale), fill=color)
        image.resize((64, 64), Image.LANCZOS).save(os.path.join(OUTPUT_DIR, name))


if __name__ == "__main__":
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    test_images()
    preview_icons()
