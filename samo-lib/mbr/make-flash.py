#!/usr/bin/env python3
"""Build the FLASH image the emulator boots from.

This is the real boot loader -- file-loader.c, compiled from this
directory -- with its LoadList trimmed to the one entry an emulator run
needs.  The full list overlaps the next diagnostic slot when linked against
the larger modern libraries, which is why flash.rom as the mbr Makefile
builds it currently stops partway through its banner.

Every emulator run of anything that runs on a WikiReader goes through this:
mask ROM, MBR, this loader, kernel.elf, init.app.  The emulator refuses to
boot a bare ELF without --bare-elf, which exists only for the toolchain
test suites.  Physical WikiReaders use their factory FLASH and need only
the application on the card.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def build(output, toolchain):
    if output.exists():
        raise ValueError('Output already exists')
    source = (ROOT / 'samo-lib/mbr/file-loader.c').read_text()
    begin = source.index('} LoadList[] = {') + len('} LoadList[] = {')
    end = source.index('\n};', begin)
    source = source[:begin] + '\n\t{"kernel.elf", 0},\n' + source[end:]
    cc = str(toolchain / 'c33-epson-elf-gcc')
    includes = ('samo-lib/mbr', 'samo-lib/drivers/include', 'samo-lib/fatfs/src',
                'samo-lib/fatfs/config/c33/read-only', 'samo-lib/mini-libc/include', 'samo-lib/include')
    with tempfile.TemporaryDirectory(prefix='flash-loader-', dir=output.parent) as tmp:
        tmp = Path(tmp)
        (tmp / 'loader.c').write_text(source)
        subprocess.run([cc, '-mc33pe', '-Os', '-fgnu89-inline', '-mno-long-calls',
                        '-fno-builtin', '-ffunction-sections', '-fdata-sections',
                        *['-I' + str(ROOT / p) for p in includes], '-c',
                        str(tmp / 'loader.c'), '-o', str(tmp / 'loader.o')], check=True)
        libgcc = subprocess.check_output([cc, '-mc33pe', '-print-libgcc-file-name'], text=True).strip()
        libraries = ('samo-lib/drivers/lib/libdrivers.a', 'samo-lib/fatfs/lib/read-only/libtinyfat.a',
                     'samo-lib/drivers/lib/libdrivers.a', 'samo-lib/mini-libc/lib/libc.a')
        subprocess.run([str(toolchain / 'c33-epson-elf-ld'), '-T', str(ROOT / 'samo-lib/mbr/application.lds'),
                        '-static', '-N', '--gc-sections', '--undefined=file_loader',
                        '-o', str(tmp / 'loader.elf'), str(tmp / 'loader.o'),
                        *[str(ROOT / p) for p in libraries], libgcc], check=True)
        subprocess.run([str(toolchain / 'c33-epson-elf-objcopy'), '-O', 'binary',
                        '--only-section=.text', '--only-section=.rodata', '--only-section=.data',
                        str(tmp / 'loader.elf'), str(tmp / 'loader.bin')], check=True)
        payload = (tmp / 'loader.bin').read_bytes()
    if len(payload) > 0x1d00:
        raise ValueError(f'Kernel loader exceeds FLASH slot: {len(payload)} bytes')
    flash = bytearray((ROOT / 'samo-lib/mbr/flash.rom').read_bytes())
    if len(flash) != 65536:
        raise ValueError('Expected a 64 KiB FLASH image')
    flash[0x2300:0x4000] = payload + b'\xff' * (0x1d00 - len(payload))
    with output.open('xb') as out:
        out.write(flash)
    print(f'{output}: kernel loader {len(payload)} / {0x1d00} bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--toolchain', type=Path, default=ROOT / 'host-tools/toolchain-c33/work/install/bin')
    args = parser.parse_args()
    build(args.output, args.toolchain)
