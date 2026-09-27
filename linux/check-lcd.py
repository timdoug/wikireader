#!/usr/bin/env python3
"""Verify the native Linux console output in a wremu PGM."""

import argparse
from pathlib import Path


def read_pgm(path):
    parts = path.read_bytes().split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"P5" or parts[2] != b"255":
        raise ValueError("expected a binary PGM")
    width, height = map(int, parts[1].split())
    pixels = parts[3]
    if len(pixels) != width * height:
        raise ValueError("PGM pixel data has the wrong length")
    return width, height, pixels


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--symbols", action="store_true")
    parser.add_argument("--edited", action="store_true")
    args = parser.parse_args()

    width, height, pixels = read_pgm(args.image)
    if (width, height) != (240, 208):
        raise SystemExit(f"unexpected LCD size {width}x{height}")

    if any(pixel != 255 for pixel in pixels[112 * width:120 * width]):
        raise SystemExit("LCD gap between text and keyboard is not blank")

    black_text_pixels = sum(pixel == 0 for pixel in pixels[:112 * width])
    if black_text_pixels < 100:
        raise SystemExit("LCD text area is unexpectedly blank")
    black_keyboard_pixels = sum(pixel == 0 for pixel in pixels[120 * width:])
    if black_keyboard_pixels < 1000:
        raise SystemExit("LCD on-screen keyboard is unexpectedly blank")
    if any(pixels[120 * width + x] != 0 for x in range(width)):
        raise SystemExit("LCD on-screen keyboard has no top border")
    if args.symbols and pixels[127 * width + 11] != 0:
        raise SystemExit("LCD on-screen keyboard did not switch to symbols")
    if args.edited:
        banner_pixels = sum(
            pixel == 0 for pixel in pixels[: width * 8]
        )
        if banner_pixels < 20:
            raise SystemExit("LCD terminal lost its banner while editing")
    print(f"LCD console passed: {black_text_pixels} text and "
          f"{black_keyboard_pixels} keyboard pixels")


if __name__ == "__main__":
    main()
