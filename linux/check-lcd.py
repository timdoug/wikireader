#!/usr/bin/env python3
"""Verify the native Linux text and checkpoint output in a wremu PGM."""

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
    parser.add_argument("--stages", type=int, default=7)
    args = parser.parse_args()

    width, height, pixels = read_pgm(args.image)
    if (width, height) != (240, 208):
        raise SystemExit(f"unexpected LCD size {width}x{height}")

    for stage in range(args.stages):
        x = stage * 16 + 4
        if pixels[196 * width + x] != 0:
            raise SystemExit(f"LCD checkpoint {stage} is absent")
        if pixels[196 * width + x + 8] != 255:
            raise SystemExit(f"LCD checkpoint {stage} has no separator")

    black_text_pixels = sum(pixel == 0 for pixel in pixels[:192 * width])
    if black_text_pixels < 100:
        raise SystemExit("LCD text area is unexpectedly blank")
    print(f"LCD console passed: {args.stages} checkpoints, "
          f"{black_text_pixels} black text pixels")


if __name__ == "__main__":
    main()
