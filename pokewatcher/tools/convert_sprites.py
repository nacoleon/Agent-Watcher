#!/usr/bin/env python3
"""Convert PNG sprite sheets to RGB565 raw format for PokéWatcher.

Usage:
    python convert_sprites.py <input.png> <output.raw>

Output format:
    - First 4 bytes: width (uint16 LE) + height (uint16 LE)
    - Followed by width*height*2 bytes of RGB565 pixel data (LE)

Transparent pixels are converted to magenta (0xF81F) as a color key.
"""

import sys
import struct
from PIL import Image


def rgb888_to_rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def convert(input_path, output_path):
    img = Image.open(input_path).convert("RGBA")
    w, h = img.size
    pixels = img.load()

    with open(output_path, "wb") as f:
        # Header: width, height as uint16 LE
        f.write(struct.pack("<HH", w, h))

        # Pixel data
        for y in range(h):
            for x in range(w):
                r, g, b, a = pixels[x, y]
                if a < 128:
                    # Transparent -> magenta color key
                    pixel = 0xF81F
                else:
                    pixel = rgb888_to_rgb565(r, g, b)
                f.write(struct.pack("<H", pixel))

    print(f"Converted {input_path} ({w}x{h}) -> {output_path} ({4 + w*h*2} bytes)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.png> <output.raw>")
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])
