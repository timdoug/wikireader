#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Count NX drawing calls to verify that typing repaints only changed keys."""

import argparse
import json
from pathlib import Path
import re
import subprocess

from test_terminal import read_pgm, run


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path,
                        default=Path.home() / "wikireader")
    parser.add_argument("--image", type=Path, default=root / "nuttx")
    parser.add_argument("--out", type=Path,
                        default=root / "build/wikireader/keyboard-redraw")
    args = parser.parse_args()
    wr = args.wikireader.resolve()
    image = args.image.resolve()
    out = args.out.resolve()
    nm = wr / "host-tools/toolchain-c33/work/install/bin/c33-epson-elf-nm"
    symbols = subprocess.check_output([str(nm), str(image)], text=True)
    probes = []
    for name in ("nx_fill", "nx_bitmap"):
        match = re.search(r"^([0-9a-f]+) T " + name + r"$", symbols, re.M)
        if match is None:
            raise SystemExit(f"Missing ELF symbol: {name}")
        probes += ["-X", f"0x{match[1]},{name}"]

    counts = {}
    pixels = {}
    for case in ("idle", "control", "touch", "serial"):
        folder = out / case
        folder.mkdir(parents=True, exist_ok=True)
        command = [str(wr / "emulator/wremu"), "-n", "30000000", *probes]
        if case == "control":
            command += ["-T", "12,197,20000000"]
        elif case == "touch":
            command += ["-T", "12,131,20000000"]
        elif case == "serial":
            (folder / "input.txt").write_text("q")
            command += ["--uart-input", str(folder / "input.txt"),
                        "--uart-start", "20000000"]
        text = run([*command, str(image)], folder, "emulator.log")
        counts[case] = {name: int(count) for name, count in re.findall(
            r"--- probe (nx_fill|nx_bitmap)\s+(\d+) hits", text)}
        if len(counts[case]) != 2:
            raise SystemExit(f"Missing drawing probes: {folder}")
        pixels[case] = read_pgm(folder / "screen.pgm")

    (out / "counts.json").write_text(json.dumps(counts, indent=2) + "\n")

    # Serial and touch input must produce the same visible 'q'.  The only
    # extra drawing for touch is highlighting and releasing that one key.
    if pixels["touch"] != pixels["serial"] or pixels["touch"] == pixels["idle"]:
        raise SystemExit("Touch and serial input did not draw the same character")
    delta = {name: counts["touch"][name] - counts["serial"][name]
             for name in counts["touch"]}
    if delta != {"nx_fill": 2, "nx_bitmap": 2}:
        raise SystemExit(f"Ordinary tap repainted more than its key: {delta}")

    # Ctrl remains highlighted after release.  Its appearance changes only
    # on press, and all other keys must retain their pixels and draw counts.
    delta = {name: counts["control"][name] - counts["idle"][name]
             for name in counts["control"]}
    if delta != {"nx_fill": 1, "nx_bitmap": 3}:
        raise SystemExit(f"Ctrl tap repainted unchanged keys: {delta}")
    for index, (before, after) in enumerate(zip(pixels["idle"], pixels["control"])):
        x, y = index % 240, index // 240
        if not (1 <= x <= 22 and 187 <= y <= 206) and before != after:
            raise SystemExit("Ctrl tap changed pixels outside its interior")
    if pixels["control"] == pixels["idle"]:
        raise SystemExit("Ctrl did not remain highlighted after release")
    print("PASS: ordinary tap repaints one key twice; Ctrl paints once; "
          f"unchanged keys and borders are untouched. Counts: {out / 'counts.json'}")


if __name__ == "__main__":
    main()
