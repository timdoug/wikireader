#!/usr/bin/env python3
"""Give a synthetic FAT32 card image accurate FSInfo allocation hints.

Fresh fixtures with unknown hints make firmware scan the FAT to allocate its
first diagnostic log. That work is absent on a card with valid hints. Use
this only for generated fixtures: captured card metadata is evidence and
must retain the state in which it was measured. Never opens block devices.
"""
import argparse
import json
from pathlib import Path
import stat
import struct


def fsinfo_sector(free_clusters, last_allocated):
    result = bytearray(512)
    struct.pack_into('<I', result, 0, 0x41615252)
    struct.pack_into('<III', result, 484, 0x61417272, free_clusters, last_allocated)
    struct.pack_into('<I', result, 508, 0xaa550000)
    return result


def recount(path):
    path = Path(path)
    if not stat.S_ISREG(path.stat().st_mode):
        raise ValueError('Only ordinary fixture image files are accepted')
    with path.open('r+b') as image:
        def read(offset, count):
            image.seek(offset)
            value = image.read(count)
            if len(value) != count:
                raise ValueError('Truncated fixture')
            return value

        mbr = read(0, 512)
        if mbr[510:] != b'\x55\xaa' or mbr[450] not in (0x0b, 0x0c):
            raise ValueError('Expected FAT32 as the first MBR partition')
        part, length = struct.unpack_from('<II', mbr, 454)
        boot = read(part * 512, 512)
        sector_size, cluster_size, reserved, copies = struct.unpack_from('<HBHB', boot, 11)
        total, fat_sectors = struct.unpack_from('<II', boot, 32)
        fsinfo, backup = struct.unpack_from('<HH', boot, 48)
        if (sector_size != 512 or not cluster_size or cluster_size & (cluster_size - 1)
                or not reserved or copies not in (1, 2) or not fat_sectors
                or total > length or boot[510:] != b'\x55\xaa'):
            raise ValueError('Unsupported FAT32 geometry')
        clusters = (total - reserved - copies * fat_sectors) // cluster_size
        if not 65525 <= clusters < 0x0ffffff5 or clusters + 2 > fat_sectors * 128:
            raise ValueError('Invalid FAT32 data area')
        # Bound memory use and reject accidental huge/archive input files.
        if fat_sectors > 8192 or not 0 < fsinfo < reserved:
            raise ValueError('Expected a small boot-partition fixture')
        fat = read((part + reserved) * 512, fat_sectors * 512)
        free = [i for i in range(2, clusters + 2)
                if struct.unpack_from('<I', fat, i * 4)[0] & 0x0fffffff == 0]
        # FatFs starts searching just after last_clst. Point before a known
        # free cluster; this is a hint, never a claim about file contiguity.
        hint = max(2, free[0] - 1) if free else 2
        value = fsinfo_sector(len(free), hint)
        offsets = [fsinfo]
        if backup not in (0, 0xffff) and backup + fsinfo < reserved:
            offsets.append(backup + fsinfo)
        for offset in offsets:
            image.seek((part + offset) * 512)
            image.write(value)
    return dict(data_clusters=clusters, free_clusters=len(free),
                last_allocated_hint=hint, sectors_written=len(offsets))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    print(json.dumps(recount(args.image), indent=2))
