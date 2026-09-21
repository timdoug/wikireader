#!/usr/bin/env python3
"""Create a disposable FAT32 WikiReader card containing Mini vMac media."""
import argparse
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def make_card(output: Path, rom_path: Path, disk_path: Path):
    rom = rom_path.read_bytes()
    disk = disk_path.read_bytes()
    if len(rom) != 128 * 1024 or int.from_bytes(rom[:4], 'big') not in (
            0x4D1EEEE1, 0x4D1EEAE1, 0x4D1F8172):
        raise ValueError('Expected a supported 128 KiB Macintosh Plus ROM')
    raw_sizes = {400 * 1024, 800 * 1024, 1440 * 1024}
    dc42_sizes = {84 + size + (size // 512) * 12 for size in raw_sizes}
    if len(disk) not in raw_sizes | dc42_sizes:
        raise ValueError('Expected a 400K, 800K, or 1.44MB raw/DC42 disk image')

    root_files = {
        'KERNEL.ELF': (ROOT / 'samo-lib/grifo/grifo.elf').read_bytes(),
        'INIT.APP': (ROOT / 'samo-lib/grifo/applications/init/init.app').read_bytes(),
        'MINIVMAC.APP': (ROOT / 'minivmac/minivmac.app').read_bytes(),
        'MINIVMAC.ICO': (ROOT / 'minivmac/minivmac.ico').read_bytes(),
        'INIT.INI': b'minivmac.ico : minivmac.app\n',
    }
    part, sectors, reserved, fat_sectors = 2048, 131072, 32, 1024
    clusters = sectors - reserved - 2 * fat_sectors
    data_sector = part + reserved + 2 * fat_sectors
    fat = bytearray(fat_sectors * 512)
    struct.pack_into('<III', fat, 0, 0x0FFFFFF8, 0xFFFFFFFF, 0x0FFFFFFF)
    next_cluster = 3
    payloads = []

    def allocate(data):
        nonlocal next_cluster
        count = max(1, (len(data) + 511) // 512)
        start = next_cluster
        next_cluster += count
        if next_cluster > clusters + 2:
            raise ValueError('Files do not fit the card')
        for cluster in range(start, next_cluster):
            value = 0x0FFFFFFF if cluster + 1 == next_cluster else cluster + 1
            struct.pack_into('<I', fat, cluster * 4, value)
        payloads.append((start, data))
        return start

    def entry(name, cluster, size, directory=False):
        base, _, ext = name.upper().partition('.')
        record = bytearray(32)
        record[:11] = (base.ljust(8) + ext.ljust(3)).encode('ascii')
        record[11] = 0x10 if directory else 0x20
        struct.pack_into('<H', record, 20, cluster >> 16)
        struct.pack_into('<HI', record, 26, cluster & 0xFFFF, size)
        return record

    root = bytearray()
    for name, data in root_files.items():
        root += entry(name, allocate(data), len(data))

    rom_cluster = allocate(rom)
    disk_cluster = allocate(disk)
    directory_cluster = next_cluster
    directory = bytearray(512)
    directory[:32] = entry('', directory_cluster, 0, True)
    directory[:11] = b'.          '
    directory[32:64] = entry('', 0, 0, True)
    directory[32:43] = b'..         '
    directory[64:96] = entry('MACPLUS.ROM', rom_cluster, len(rom))
    directory[96:128] = entry('DISK1.DSK', disk_cluster, len(disk))
    allocate(directory)
    root += entry('MINIVMAC', directory_cluster, 0, True)
    payloads.append((2, root))

    mbr = bytearray(512)
    mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', 0x0C,
                               b'\xfe\xff\xff', part, sectors)
    mbr[510:] = b'\x55\xaa'
    boot = bytearray(512)
    boot[:11] = b'\xeb\x58\x90WRMAC   '
    struct.pack_into('<HBHBHHBHHHII', boot, 11, 512, 1, reserved, 2, 0, 0,
                     0xF8, 0, 63, 255, part, sectors)
    struct.pack_into('<IHHIHH', boot, 36, fat_sectors, 0, 0, 2, 1, 6)
    boot[64], boot[66] = 0x80, 0x29
    struct.pack_into('<I', boot, 67, 0x4D414331)
    boot[71:90] = b'WRMAC      FAT32   '
    boot[510:] = b'\x55\xaa'
    info = bytearray(512)
    struct.pack_into('<I', info, 0, 0x41615252)
    struct.pack_into('<III', info, 484, 0x61417272,
                     clusters - (next_cluster - 2), next_cluster - 1)
    struct.pack_into('<I', info, 508, 0xAA550000)

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
    parser.add_argument('rom', type=Path)
    parser.add_argument('disk', type=Path)
    args = parser.parse_args()
    make_card(args.output, args.rom, args.disk)
