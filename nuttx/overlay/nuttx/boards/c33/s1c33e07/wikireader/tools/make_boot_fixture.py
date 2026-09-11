#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build isolated FLASH/card fixtures; never write to the WikiReader checkout."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path, default=Path.home() / "wikireader")
    parser.add_argument("--image", type=Path, default=root / "nuttx")
    parser.add_argument("--out", type=Path,
                        default=root / "build/wikireader/boot-fixture")
    args = parser.parse_args()
    wr = args.wikireader.resolve()
    out = args.out.resolve()
    if out == wr or wr in out.parents:
        parser.error("Output must be outside the WikiReader checkout")
    out.mkdir(parents=True, exist_ok=True)
    tool = wr / "host-tools/toolchain-c33/work/install/bin"
    mbr = wr / "samo-lib/mbr"
    image = args.image.resolve()
    if image.read_bytes()[:5] != b"\x7fELF\x01":
        parser.error("Expected a 32-bit NuttX ELF image")

    # Preserve the original GPL notice in this generated derivative.  Only
    # the load list changes: a single kernel is sufficient for this fixture.
    source = (mbr / "file-loader.c").read_text()
    marker = "} LoadList[] = {"
    if source.count(marker) != 1:
        raise SystemExit("Unrecognized file-loader load table")
    begin = source.index(marker) + len(marker)
    end = source.index("\n};", begin)
    source = source[:begin] + '\n\t{"kernel.elf", 0},\n' + source[end:]
    (out / "file-loader.c").write_text(source)
    cc = [str(tool / "c33-epson-elf-gcc"), "-mc33pe", "-Os", "-fgnu89-inline",
          "-mno-long-calls", "-fno-builtin", "-ffunction-sections", "-fdata-sections"]
    for path in ("samo-lib/mbr", "samo-lib/drivers/include", "samo-lib/fatfs/src",
                 "samo-lib/fatfs/config/c33/read-only", "samo-lib/mini-libc/include",
                 "samo-lib/include"):
        cc += ["-I" + str(wr / path)]
    subprocess.run(cc + ["-c", str(out / "file-loader.c"), "-o",
                        str(out / "file-loader.o")], check=True)
    libgcc = subprocess.check_output(
        [str(tool / "c33-epson-elf-gcc"), "-mc33pe", "-print-libgcc-file-name"],
        text=True).strip()
    libs = [wr / path for path in ("samo-lib/drivers/lib/libdrivers.a",
            "samo-lib/fatfs/lib/read-only/libtinyfat.a",
            "samo-lib/drivers/lib/libdrivers.a", "samo-lib/mini-libc/lib/libc.a")]
    subprocess.run([str(tool / "c33-epson-elf-ld"), "-T", str(mbr / "application.lds"),
                    "-static", "-N", "--gc-sections", "--undefined=file_loader",
                    "-o", str(out / "file-loader.elf"), str(out / "file-loader.o"),
                    *map(str, libs), libgcc], check=True)
    subprocess.run([str(tool / "c33-epson-elf-objcopy"), "-O", "binary",
                    "--only-section=.text", "--only-section=.rodata",
                    "--only-section=.data", str(out / "file-loader.elf"),
                    str(out / "file-loader.bin")], check=True)
    payload = (out / "file-loader.bin").read_bytes()

    # MBR copies 8192 - 0x300 bytes; the following FLASH slot starts at 0x4000.
    # These bounds coincide for a payload beginning at FLASH offset 0x2300.
    start, end = 0x2300, 0x4000
    if len(payload) > end - start:
        raise SystemExit(f"Loader is {len(payload)} bytes; maximum is {end - start}")
    original = (mbr / "flash.rom").read_bytes()
    if len(original) < end:
        raise SystemExit("FLASH image is too short")
    flash = original[:start] + payload + b"\xff" * (end - start - len(payload))
    flash += original[end:]
    (out / "flash-nuttx.rom").write_bytes(flash)

    # Reuse the checkout's existing pure FAT fixture builder/reader, without
    # invoking its main program, build_loader(), or creating a __pycache__.
    sys.dont_write_bytecode = True
    helper = wr / "emulator/tools/mem_dma_bench/run.py"
    spec = importlib.util.spec_from_file_location("wr_fat_fixture", helper)
    fat = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fat)
    kernel = image.read_bytes()
    card = out / "nuttx-card.img"
    fat.make_image(card, {"kernel.elf": kernel})
    if fat.read_file(card, "kernel.elf") != kernel:
        raise SystemExit("FAT kernel readback mismatch")
    inputs = [image, mbr / "file-loader.c", mbr / "application.lds",
              mbr / "flash.rom", helper, *libs, Path(libgcc)]
    manifest = {"purpose": "Emulator-only kernel boot fixture",
                "loader_bytes": len(payload), "loader_max_bytes": end - start,
                "flash_patch_begin": start, "flash_patch_end": end,
                "inputs_sha256": {str(p): digest(p) for p in inputs},
                "outputs_sha256": {p.name: digest(p) for p in
                    (out / "file-loader.bin", out / "flash-nuttx.rom", card)}}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Built {out}: loader {len(payload)}/{end - start} bytes; "
          "kernel FAT readback verified")


if __name__ == "__main__":
    main()
