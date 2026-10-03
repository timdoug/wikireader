#!/usr/bin/env python3
"""Run the C33 SD DMA backend (samo-lib/grifo/src/sd_dma.c) as a Grifo
application against an emulated card, booted the way the device boots:
mask ROM, MBR, the FLASH boot loader, grifo as kernel.elf, init.app, then the
test, which reads a pattern sector with the card's own driver idle.

Uses only project files. No attached card or Wikipedia archive is accessed.
"""
import argparse
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
INCLUDES = [
    "samo-lib/include", "samo-lib/grifo/include", "samo-lib/grifo/common",
    "samo-lib/grifo/src", "samo-lib/mini-libc/include",
    "samo-lib/drivers/include", "samo-lib/fatfs/src",
    "samo-lib/fatfs/config/c33/modern",
]
TEST_SECTOR = 1     # in the card's reserved area, before the partition
spec = importlib.util.spec_from_file_location(
    "fat", ROOT / "emulator/tools/mem_dma_bench/run.py")
fat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fat)


def pattern():
    return bytes(((i * 73) ^ (i >> 3) ^ 0x9d) & 255 for i in range(512))


def compile_driver(stage, toolchain, bits, tx):
    # The application script, with the driver's descriptor RAM in DSTRAM
    # where grifo keeps its own, past grifo's copy.
    lds = (ROOT / "samo-lib/grifo/lds/application.lds").read_text()
    anchor = "    . = ALIGN(1024);\n\n    .rodata : {"
    assert lds.count(anchor) == 1
    lds = lds.replace(anchor, "    .dstram 0x84400 (NOLOAD) : { *(.dstram) }\n\n" + anchor)
    script = stage / "test.lds"
    script.write_text(lds)
    elf = stage / "sddma.elf"
    subprocess.run([str(toolchain / "c33-epson-elf-gcc"), "-O2", "-mc33pe",
                    "-mlong-calls", "-std=gnu99", "-fgnu89-inline",
                    f"-DSD_DMA_BITS={bits}", "-falign-loops=16",
                    f"-DSD_DMA_TX_HSDMA={int(tx == 'hsdma')}",
                    "-fno-builtin", "-ffunction-sections", "-fdata-sections",
                    "-nostdlib", *[f"-I{ROOT / p}" for p in INCLUDES],
                    "-Wl,--gc-sections", f"-Wl,-T,{script}",
                    str(ROOT / "emulator/tools/test_sd_dma_driver.c"),
                    str(ROOT / "samo-lib/grifo/lib/libgrifo.a"),
                    str(ROOT / "samo-lib/mini-libc/lib/libc.a"),
                    "-lgcc", "-o", str(elf)], check=True)
    app = stage / "sddma.app"
    subprocess.run([str(toolchain / "c33-epson-elf-strip"), "--strip-unneeded",
                    "--strip-debug", "-o", str(app), str(elf)], check=True)
    return app


def run_driver(stage, app, bits, tx):
    card = stage / "card.img"
    fat.make_image(card, {
        "kernel.elf": (ROOT / "samo-lib/grifo/grifo.elf").read_bytes(),
        "init.app": (ROOT / "samo-lib/grifo/applications/init/init.app").read_bytes(),
        "sddma.app": app.read_bytes(),
        "sddma.ico": bytes(512),
        "init.ini": b"sddma.ico : sddma.app\n",
    })
    with card.open("r+b") as f:
        f.seek(TEST_SECTOR * 512)
        f.write(pattern())
    flash = stage / "flash.rom"
    subprocess.run([sys.executable, str(ROOT / "samo-lib/mbr/make-flash.py"),
                    str(flash)], check=True, stdout=subprocess.DEVNULL)
    run = subprocess.run([str(ROOT / "emulator/wremu"), "-R",
                          "-e", str(flash), "-c", str(card),
                          "-n", "400000000"],
                         cwd=stage, text=True, errors="replace",
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         timeout=300)
    found = re.search(r"SD DMA DRIVER RESULT (\w+) (\d+) (\d+) (\d+)", run.stdout)
    values = (int(found[1], 16), *map(int, found.groups()[1:])) if found else None
    cases = 32 if bits == 32 else 29
    if values != (0x600d, cases, 0, 0) or "stop reason: powered off" not in run.stdout:
        print(run.stdout)
        raise SystemExit(
            f"SD DMA driver failed: {values} (status, case, C line, reserved)")
    print(f"C33 {bits}-bit SD DMA ({tx} TX): {cases} aligned/unaligned, byte-order, CRC-boundary, "
          "timeout, overflow, profiling and SPI handoff cases pass")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--toolchain", type=Path,
        default=ROOT / "host-tools/toolchain-c33/work/install/bin")
    parser.add_argument("--bits", type=int, choices=(8, 32), default=32)
    parser.add_argument("--tx", choices=("idma", "hsdma"), default="idma")
    args = parser.parse_args()
    toolchain = args.toolchain.resolve()
    for name in ("c33-epson-elf-gcc", "c33-epson-elf-strip"):
        if not (toolchain / name).is_file():
            parser.error(f"missing {toolchain / name}; specify --toolchain")
    with tempfile.TemporaryDirectory(prefix="wr-sd-dma-") as work:
        stage = Path(work)
        app = compile_driver(stage, toolchain, args.bits, args.tx)
        run_driver(stage, app, args.bits, args.tx)


if __name__ == "__main__":
    main()
