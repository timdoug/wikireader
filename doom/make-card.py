#!/usr/bin/env python3
"""Create a disposable 64 MiB FAT32 Doom emulator card; no mounted disks needed."""
import argparse
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def make_card(output, wad, arguments, app=ROOT / 'doom/doom.app'):
    files = {
        'KERNEL.ELF': (ROOT / 'samo-lib/grifo/grifo.elf').read_bytes(),
        'INIT.APP': (ROOT / 'samo-lib/grifo/applications/init/init.app').read_bytes(),
        'DOOM.APP': app.read_bytes(),
        'DOOM.ICO': (ROOT / 'doom/doom.ico').read_bytes(),
        'INIT.INI': ('doom.ico : doom.app ' + arguments + '\n').encode(),
    }
    wad_bytes = wad.read_bytes()
    if wad_bytes[:4] != b'IWAD':
        raise ValueError('Expected a Doom IWAD')
    if wad.name.lower() not in ('doom1.wad', 'doom.wad', 'doomu.wad', 'doom2.wad', 'tnt.wad', 'plutonia.wad'):
        raise ValueError('Keep the standard IWAD filename, e.g. doom1.wad')
    part, sectors, reserved, fat_sectors = 2048, 131072, 32, 1024
    clusters = sectors - reserved - 2 * fat_sectors
    data_sector = part + reserved + 2 * fat_sectors
    fat = bytearray(fat_sectors * 512)
    struct.pack_into('<III', fat, 0, 0x0ffffff8, 0xffffffff, 0x0fffffff)
    next_cluster = 3
    payloads = []

    def allocate(data):
        nonlocal next_cluster
        count = max(1, (len(data) + 511) // 512)
        start = next_cluster
        next_cluster += count
        if next_cluster > clusters + 2:
            raise ValueError('Files do not fit the card')
        for n in range(start, next_cluster):
            struct.pack_into('<I', fat, n * 4, 0x0fffffff if n + 1 == next_cluster else n + 1)
        payloads.append((start, data))
        return start

    def entry(name, cluster, size, directory=False):
        base, _, ext = name.upper().partition('.')
        record = bytearray(32)
        record[:11] = (base.ljust(8) + ext.ljust(3)).encode('ascii')
        record[11] = 0x10 if directory else 0x20
        struct.pack_into('<H', record, 20, cluster >> 16)
        struct.pack_into('<HI', record, 26, cluster & 65535, size)
        return record

    root = bytearray()
    for name, data in files.items():
        root += entry(name, allocate(data), len(data))
    wad_cluster = allocate(wad_bytes)
    doom_cluster = next_cluster
    directory = bytearray(512)
    directory[:32] = entry('', doom_cluster, 0, True)
    directory[:11] = b'.          '
    directory[32:64] = entry('', 0, 0, True)
    directory[32:43] = b'..         '
    directory[64:96] = entry(wad.name, wad_cluster, len(wad_bytes))
    allocate(directory)
    root += entry('DOOM', doom_cluster, 0, True)
    payloads.append((2, root))

    mbr = bytearray(512)
    mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', 0x0c, b'\xfe\xff\xff', part, sectors)
    mbr[510:] = b'\x55\xaa'
    boot = bytearray(512)
    boot[:11] = b'\xeb\x58\x90WRDOOM  '
    struct.pack_into('<HBHBHHBHHHII', boot, 11, 512, 1, reserved, 2, 0, 0, 0xf8, 0, 63, 255, part, sectors)
    struct.pack_into('<IHHIHH', boot, 36, fat_sectors, 0, 0, 2, 1, 6)
    boot[64] = 0x80
    boot[66] = 0x29
    struct.pack_into('<I', boot, 67, 0x444f4f4d)
    boot[71:90] = b'WRDOOM     FAT32   '
    boot[510:] = b'\x55\xaa'
    info = bytearray(512)
    struct.pack_into('<I', info, 0, 0x41615252)
    struct.pack_into('<III', info, 484, 0x61417272, clusters - (next_cluster - 2), next_cluster - 1)
    struct.pack_into('<I', info, 508, 0xaa550000)
    # Exclusive creation prevents overwriting an existing image or device.
    with output.open('xb') as image:
        image.truncate((part + sectors) * 512)
        def write(sector, data):
            image.seek(sector * 512)
            image.write(data)
        write(0, mbr)
        for offset in (0, 6):
            write(part + offset, boot)
            write(part + offset + 1, info)
        write(part + reserved, fat)
        write(part + reserved + fat_sectors, fat)
        for cluster, data in payloads:
            write(data_sector + cluster - 2, data)
    print(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('wad', type=Path)
    parser.add_argument('--args', default='', help='Doom command line, e.g. "-warp 1 1 -skill 2"')
    parser.add_argument('--app', type=Path, default=ROOT / 'doom/doom.app', help='Application build to put on the card')
    args = parser.parse_args()
    make_card(args.output, args.wad, args.args, args.app)
