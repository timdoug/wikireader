#!/usr/bin/env python3
"""Run the physical benchmark app through the real loader/kernel on a tiny FAT fixture.

Reads only local boot executables. Never accesses an attached card or a ZIM.
The loader harness supplies its inherited stack; earlier FLASH boot is omitted.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
STAGE = ROOT / "build/wr128/mem-dma"
TOOLCHAIN = ROOT / "host-tools/toolchain-c33/work/install/bin"


def make_image(path, files, sectors_per_cluster=1):
    """Build a FAT32 image.  One sector per cluster is the default because
    every existing caller was written against that geometry, but it is the
    worst case for read throughput: fs_fat32.c clips a multi-sector read to
    the sectors left in the cluster, so single-sector clusters mean the SD
    driver never issues CMD18 and pays a command, a response and a token
    poll for every 512 bytes.  Pass a real card's cluster size to measure
    the card rather than the fixture."""
    spc = sectors_per_cluster
    assert spc >= 1 and (spc & (spc - 1)) == 0
    part, reserved = 2048, 32
    # FAT32 is a cluster count, not a signature: keep enough of them that a
    # driver counting clusters still calls this FAT32 as the size grows.
    clusters = max(65600, 122968 // spc)
    fatsize = max(1000, ((clusters + 2) * 4 + 511) // 512)
    sectors = reserved + 2 * fatsize + clusters * spc
    data_sector = part + reserved + 2 * fatsize
    fat = bytearray(fatsize * 512)
    struct.pack_into('<III', fat, 0, 0x0ffffff8, 0x0fffffff, 0x0fffffff)
    root = bytearray(4096)
    objects = [(2, root)]
    root_clusters = max(1, (len(root) + 512 * spc - 1) // (512 * spc))
    last_root = 2 + root_clusters - 1
    for c in range(2, last_root + 1):
        struct.pack_into('<I', fat, c * 4,
                         c + 1 if c < last_root else 0x0fffffff)
    cluster = last_root + 1
    for i, (name, content) in enumerate(files.items()):
        stem, ext = name.upper().split('.')
        assert len(stem) <= 8 and len(ext) <= 3
        count = max(1, (len(content) + 512 * spc - 1) // (512 * spc))
        for c in range(cluster, cluster + count):
            struct.pack_into('<I', fat, c*4, c+1 if c+1 < cluster+count else 0x0fffffff)
        off = i * 32
        root[off:off+11] = (stem.ljust(8) + ext.ljust(3)).encode()
        root[off+11] = 0x20
        # Real cards carry real dates, and a guest with no RTC has nothing
        # else to set its clock from; a fixture stuck at the FAT floor would
        # never exercise that.
        now = time.localtime()
        fat_date = ((now.tm_year - 1980) << 9) | (now.tm_mon << 5) | now.tm_mday
        fat_time = (now.tm_hour << 11) | (now.tm_min << 5) | (now.tm_sec // 2)
        struct.pack_into('<HH', root, off+22, fat_time, fat_date)
        struct.pack_into('<H', root, off+18, fat_date)
        struct.pack_into('<H', root, off+20, cluster >> 16)
        struct.pack_into('<HI', root, off+26, cluster & 65535, len(content))
        objects.append((cluster, content))
        cluster += count
    assert len(files) < 128 and cluster < clusters + 2
    mbr = bytearray(512)
    mbr[450] = 0x0c
    struct.pack_into('<II', mbr, 454, part, sectors)
    mbr[510:] = b'\x55\xaa'
    vbr = bytearray(512)
    vbr[:11] = b'\xeb\x58\x90MSWIN4.1'
    struct.pack_into('<HBHBHHBHHHII', vbr, 11, 512, spc, reserved, 2, 0, 0, 0xf8, 0, 63, 255, part, sectors)
    struct.pack_into('<IHHIHH', vbr, 36, fatsize, 0, 0, 2, 1, 6)
    vbr[64], vbr[66] = 0x80, 0x29
    struct.pack_into('<I', vbr, 67, 0x12345678)
    vbr[71:82], vbr[82:90], vbr[510:] = b'WRBOOT     ', b'FAT32   ', b'\x55\xaa'
    fsinfo = bytearray(512)
    struct.pack_into('<I', fsinfo, 0, 0x41615252)
    # The builder knows this allocation state. Unknown hints make the
    # first boot scan the FAT before writing a diagnostic, skewing timing.
    struct.pack_into('<III', fsinfo, 484, 0x61417272,
                     clusters - (cluster - 2), cluster - 1)
    struct.pack_into('<I', fsinfo, 508, 0xaa550000)
    with path.open('wb') as out:
        out.truncate((part+sectors)*512)
        def put(sector, value):
            out.seek(sector*512)
            out.write(value)
        put(0, mbr)
        for offset in (0, 6):
            put(part+offset, vbr)
            put(part+offset+1, fsinfo)
        for offset in (reserved, reserved+fatsize):
            put(part+offset, fat)
        for c, content in objects:
            put(data_sector + (c - 2) * spc, content)


def read_file(path, wanted):
    with path.open('rb') as image:
        def read(offset, size):
            image.seek(offset)
            return image.read(size)
        part = struct.unpack_from('<I', read(0, 512), 454)[0]
        vbr = read(part*512, 512)
        reserved = struct.unpack_from('<H', vbr, 14)[0]
        spc = vbr[13]
        fatsize = struct.unpack_from('<I', vbr, 36)[0]
        fat = read((part+reserved)*512, fatsize*512)
        data = (part+reserved+2*fatsize)*512
        def chain(c):
            result = bytearray()
            seen = set()
            while 2 <= c < 0x0ffffff8:
                assert c not in seen
                seen.add(c)
                result.extend(read(data+(c-2)*512*spc, 512*spc))
                c = struct.unpack_from('<I', fat, c*4)[0] & 0x0fffffff
            return result
        root = chain(2)
        for off in range(0, len(root), 32):
            e = root[off:off+32]
            if e[0] in (0, 0xe5) or e[11] == 15:
                continue
            name = e[:8].decode().rstrip() + '.' + e[8:11].decode().rstrip()
            if name.lower() == wanted.lower():
                c = struct.unpack_from('<H', e, 20)[0] << 16 | struct.unpack_from('<H', e, 26)[0]
                return bytes(chain(c)[:struct.unpack_from('<I', e, 28)[0]])
    return None


def build_loader(*, lcd_ready=False):
    main = (ROOT / 'emulator/src/main.c').read_text()
    old = 'uint32_t boot_sp = 0;'
    assert main.count(old) == 1
    main = main.replace(old, 'uint32_t boot_sp = MASK_ROM_STACK_TOP;')
    if lcd_ready:
        # Direct file-loader entry inherits the menu's enabled LCD, just as
        # it inherits the mask-ROM stack. Supply that omitted boot state in
        # this harness only, including after the GUI's power-on reset.
        for old, new in (
            ('lcd_attach(&mem, &lcd);',
             'lcd_attach(&mem, &lcd);\n\tmem_write(&mem, REG_BASE + 0x1a04, 4, 3);'),
            ('lcd_reset(lcd);',
             'lcd_reset(lcd);\n\tif (path) mem_write(mem, REG_BASE + 0x1a04, 4, 3);'),
        ):
            assert main.count(old) == 1
            main = main.replace(old, new)
    source = STAGE / 'loader-main.c'
    source.write_text(main)
    obj = STAGE / 'loader-main.o'
    cflags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'sdl2'], text=True))
    libs = shlex.split(subprocess.check_output(['pkg-config', '--libs', 'sdl2'], text=True))
    subprocess.run(['cc', '-O2', '-I'+str(ROOT/'emulator/src'), '-I'+str(ROOT/'emulator'),
                    *cflags, '-c', str(source), '-o', str(obj)], check=True)
    objects = [ROOT/'emulator/src'/f'{name}.o' for name in
        'c33 mem elf uart sdcard periph lcd display touch timer wdt itc cmu port eeprom sdramc dma model'.split()]
    exe = STAGE / 'loader-wremu'
    subprocess.run(['cc', '-o', str(exe), str(obj), *map(str, objects), *libs], check=True)
    return exe


def parse_log(log):
    rows = []
    for line in log.splitlines():
        if line.startswith('RESULT '):
            row = dict(re.findall(r'(\w+)=([^ ]+)', line))
            for field in ('case','bytes','min','median','max','verified'):
                row[field] = int(row[field])
            row['ticks'] = [int(n) for n in row['ticks'].split(',')]
            rows.append(row)
    assert 'complete=1 state_restored=1' in log and 'failed=0' in log
    assert len(rows) == 94 and all(row['verified'] == 1 for row in rows), len(rows)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extra', type=int, default=0)
    parser.add_argument('--no-build', action='store_true')
    args = parser.parse_args()
    if not args.no_build:
        subprocess.run(['make', '-C', str(ROOT/'emulator'), 'wremu'], check=True)
        subprocess.run(['make', '-C', str(Path(__file__).parent)], check=True)
        exe = build_loader()
    else:
        exe = STAGE / 'loader-wremu'
    files = {name: (ROOT/'build/wr128/hsdma-tx/hsdma-boot'/name).read_bytes()
             for name in ('init.app', 'zim.ico')}
    files['kernel.elf'] = (ROOT/'samo-lib/grifo/grifo.elf').read_bytes()
    files.update({'init.ini': b'zim.ico : membench.app\n',
                  'membench.on': b'', 'membench.app': (STAGE/'membench.app').read_bytes()})
    image = STAGE / f'model-{args.extra}.img'
    make_image(image, files)
    symbols = subprocess.check_output([str(TOOLCHAIN/'c33-epson-elf-nm'), str(STAGE/'membench.elf')], text=True)
    done = re.search(r'^([0-9a-f]+) T membench_done$', symbols, re.M)[1]
    env = dict(os.environ, WREMU_MODEL=f'dma_mem_extra={args.extra}')
    transcript = STAGE / f'model-{args.extra}.txt'
    with transcript.open('w') as out:
        run = subprocess.run([str(exe), '-c', str(image), '-b', '0x'+done,
            '-n', '2000000000', str(ROOT/'samo-lib/mbr/file-loader.elf')],
            cwd=ROOT, env=env, stdout=out, stderr=subprocess.STDOUT, timeout=240)
    log = read_file(image, 'membench.log')
    if log is None:
        raise RuntimeError(f'No benchmark log; inspect {transcript}')
    (STAGE/f'model-{args.extra}.log').write_bytes(log)
    rows = parse_log(log.decode())
    assert run.returncode == 0, run.returncode
    assert read_file(image, 'membench.on') is None
    assert read_file(image, 'init.ini') == b'zim.ico : zim.app started-from-init\n'
    result = {'dma_mem_extra': args.extra, 'rows': rows,
        'app_sha256': hashlib.sha256(files['membench.app']).hexdigest(),
        'kernel_sha256': hashlib.sha256(files['kernel.elf']).hexdigest(),
        'normal_init_restored': True, 'one_shot_marker_consumed': True}
    (STAGE/f'model-{args.extra}.json').write_text(json.dumps(result, indent=2)+'\n')
    print(f'MEMBENCH: {len(rows)} verified cases, DMA memory overhead {args.extra} cycles/unit')
    for row in rows:
        if row['bytes'] in (5120, 524288):
            print(row['layout'], row['method'], row['bytes'], f"{row['median']/60:.1f} us")


if __name__ == '__main__':
    main()
