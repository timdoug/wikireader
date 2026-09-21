#!/usr/bin/env python3
"""Compile and execute the actual C33 SD DMA backend against a 512-byte card.

Uses only project files. No attached card or Wikipedia archive is accessed.
"""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
INCLUDES = [
    "samo-lib/include", "samo-lib/grifo/common", "samo-lib/grifo/src",
    "samo-lib/mini-libc/include", "samo-lib/drivers/include",
    "samo-lib/fatfs/src", "samo-lib/fatfs/config/c33/modern",
]


def compile_driver(stage, toolchain, bits, tx):
    script = stage / "test.lds"
    script.write_text('''OUTPUT_FORMAT("elf32-c33")
OUTPUT_ARCH(c33)
ENTRY(_start)
__dp = 0x10000000;
SECTIONS {
 . = 0x10000000;
 .text : { *(.text*) }
 .rodata : { *(.rodata*) }
 .data : { *(.data*) }
 .bss (NOLOAD) : { *(.bss*) *(COMMON) }
 .dstram 0x84000 (NOLOAD) : { *(.dstram) }
}
''')
    elf = stage / "test.elf"
    subprocess.run([str(toolchain / "c33-epson-elf-gcc"), "-O2", "-mc33pe",
                    f"-DSD_DMA_BITS={bits}", "-falign-loops=16",
                    f"-DSD_DMA_TX_HSDMA={int(tx == 'hsdma')}",
                    "-fno-builtin", "-ffunction-sections", "-fdata-sections",
                    "-nostdlib", *[f"-I{ROOT / p}" for p in INCLUDES],
                    "-Wl,--gc-sections", f"-Wl,-T,{script}",
                    str(ROOT / "emulator/tools/test_sd_dma_driver.c"),
                    "-lgcc", "-o", str(elf)], check=True)
    symbols = {}
    for line in subprocess.check_output(
            [str(toolchain / "c33-epson-elf-nm"), str(elf)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = fields[0]
    return elf, symbols


def run_driver(stage, elf, symbols, bits, tx):
    card = stage / "card.img"
    card.write_bytes(bytes(((i * 73) ^ (i >> 3) ^ 0x9d) & 255
                          for i in range(512)))
    result = stage / "result.bin"
    run = subprocess.run([str(ROOT / "emulator/wremu"), "--bare-elf",
                          "-R", "-c", str(card),
                          "-b", "0x" + symbols["test_done"],
                          "-D", "0x" + symbols["test_result"], "-L", "16",
                          "-O", str(result), "-n", "10000000", str(elf)],
                         cwd=stage, text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, timeout=60)
    data = result.read_bytes() if result.exists() else b""
    values = struct.unpack("<4I", data) if len(data) == 16 else None
    cases = 32 if bits == 32 else 29
    config = re.search(
        r"spi config: (\d+) busy control accesses, "
        r"(\d+) disables with interrupts set", run.stdout)
    if (run.returncode or values != (0x600d, cases, 0, 0) or
            not config or config.groups() != ('0', '0') or
            'spi clock: 0 unclamped disables with SD selected' not in run.stdout):
        print(run.stdout)
        raise SystemExit(
            f"SD DMA driver failed: {values} (status, case, C line, reserved)")
    channels = re.search(r'dma channels: HSDMA2 TX (\d+),', run.stdout)
    assert channels and (int(channels[1]) > 0) == (tx == 'hsdma' and bits == 32)
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
    for name in ("c33-epson-elf-gcc", "c33-epson-elf-nm"):
        if not (toolchain / name).is_file():
            parser.error(f"missing {toolchain / name}; specify --toolchain")
    with tempfile.TemporaryDirectory(prefix="wr-sd-dma-") as work:
        stage = Path(work)
        elf, symbols = compile_driver(stage, toolchain, args.bits, args.tx)
        run_driver(stage, elf, symbols, args.bits, args.tx)


if __name__ == "__main__":
    main()
