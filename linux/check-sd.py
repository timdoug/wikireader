#!/usr/bin/env python3
"""Verify the booted Linux guest persisted its status to the FAT card."""

import importlib.util
from pathlib import Path
import sys


EXPECTED = b"C33 Linux wrote this boot status\n"


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} FAT_HELPER CARD_IMAGE")

    helper = Path(sys.argv[1])
    card = Path(sys.argv[2])
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("wr_linux_fat", helper)
    fat = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fat)

    actual = fat.read_file(card, "linux.ok")
    if actual != EXPECTED:
        raise SystemExit(f"FAT linux.ok mismatch: {actual!r}")
    print("SD card passed: FAT linux.ok contains the native Linux boot status")


if __name__ == "__main__":
    main()
