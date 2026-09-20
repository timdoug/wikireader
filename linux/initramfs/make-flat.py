#!/usr/bin/env python3
"""Wrap a relocation-free C33 code image in a version-4 bFLT header."""

import argparse
import struct
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("text", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    text = args.text.read_bytes()
    header_size = 64
    end = header_size + len(text)
    fields = [
        4,              # format revision
        header_size,    # entry offset
        end,            # data start
        end,            # data end
        end,            # bss end
        16 * 1024,      # stack
        end,            # relocation table start
        0,              # relocation count
        0x11,           # load into RAM and log the mapping
        0,              # build date
        0, 0, 0, 0, 0,
    ]
    args.output.write_bytes(struct.pack(">4s15I", b"bFLT", *fields) + text)


if __name__ == "__main__":
    main()
