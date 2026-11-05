#!/usr/bin/env python3
"""FSInfo hints must describe allocation without changing fixture contents."""
import importlib.util
from pathlib import Path
import struct
import tempfile

from fat32_fixture import recount

spec = importlib.util.spec_from_file_location('builder', Path(__file__).parent/'mem_dma_bench/run.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)/'card.img'
    files = {'init.app': b'a' * 700, 'kernel.elf': b'b' * 1500}
    builder.make_image(path, files)
    part, reserved, fat_size, clusters = 2048, 32, 1000, 122968
    used = 8 + 2 + 3  # root directory and the two independently sized files
    with path.open('r+b') as image:
        image.seek((part + 1) * 512 + 488)
        assert struct.unpack('<II', image.read(8)) == (clusters - used, 14)
        # Simulate a generated image with unknown allocation hints and an
        # allocated cluster far away from the files' contiguous run.
        for copy in (0, 1):
            image.seek((part + reserved + copy * fat_size) * 512 + 50000 * 4)
            image.write(struct.pack('<I', 0x0fffffff))
        for sector in (1, 7):
            image.seek((part + sector) * 512 + 488)
            image.write(b'\xff' * 8)
        image.seek((part + reserved) * 512)
        original_fat = image.read(fat_size * 1024)
    result = recount(path)
    assert result['free_clusters'] == clusters - used - 1
    assert result['last_allocated_hint'] == 14 and result['sectors_written'] == 2
    with path.open('rb') as image:
        image.seek((part + reserved) * 512)
        assert image.read(fat_size * 1024) == original_fat
        image.seek((part + 1) * 512)
        primary = image.read(512)
        image.seek((part + 7) * 512)
        assert primary == image.read(512)
    for name, value in files.items():
        assert builder.read_file(path, name) == value
    # Reader resource verification needs directories as well as root files.
    # Reuse the allocated distant cluster for a directory pointing at the
    # existing, fragmented-capable file chain; nothing executes this image.
    with path.open('r+b') as image:
        data = (part + reserved + 2 * fat_size) * 512
        entry = bytearray(32)
        entry[:11], entry[11] = b'ZIM        ', 0x10
        struct.pack_into('<H', entry, 26, 50000)
        image.seek(data + 64)
        image.write(entry)
        entry[:11], entry[11] = b'WIKI    NLS', 0x20
        struct.pack_into('<H', entry, 26, 10)
        struct.pack_into('<I', entry, 28, len(files['init.app']))
        image.seek(data + (50000 - 2) * 512)
        image.write(entry)
    assert builder.read_file(path, 'ZiM/wiki.nls') == files['init.app']
    assert builder.read_file(path, 'zim/missing.nls') is None
    assert builder.read_file(path, 'init.app/wiki.nls') is None
    assert builder.read_file(path, 'zim') is None
    try:
        builder.read_file(path, '../init.app')
    except ValueError:
        pass
    else:
        raise AssertionError('Accepted traversal outside the image root')
    bad = Path(directory)/'not-a-card.img'
    bad.write_bytes(bytes(512))
    for invalid in (Path(directory), bad):
        try:
            recount(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError('Accepted non-fixture input')
print('FAT32 fixture: valid hints, allocation, intact files/FAT, nested files and input bounds pass')
