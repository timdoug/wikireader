#!/usr/bin/env python3
"""Run the rv32ima benchmark, or Linux, under the full-system emulator.

Boots the way the hardware does and the only way the emulator now allows:
the real FLASH image runs the boot loader, which loads kernel.elf (Grifo)
from the card, which chains to init.app.  Nothing here may take a shortcut
past that -- a direct ELF boot leaves the SDRAM controller, the clocks and
the serial line in a state the hardware is never in, which has cost this
project four separate bugs, the last of them a console that could not
receive a byte because init_rs232_ch0() had never run.
"""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def make_card(output, files):
    part, sectors, reserved, fat_sectors = 2048, 131072, 32, 1024
    clusters = sectors - reserved - 2 * fat_sectors
    data_sector = part + reserved + 2 * fat_sectors
    fat = bytearray(fat_sectors * 512)
    struct.pack_into('<II', fat, 0, 0x0ffffff8, 0xffffffff)
    next_cluster = 3          # replaced below, once the root's size is known
    payloads = []

    def allocate(data):
        nonlocal next_cluster
        count = max(1, (len(data) + 511) // 512)
        start = next_cluster
        next_cluster += count
        if next_cluster > clusters + 2:
            raise ValueError('Files do not fit the card')
        for n in range(start, next_cluster):
            struct.pack_into('<I', fat, n * 4,
                             0x0fffffff if n + 1 == next_cluster else n + 1)
        payloads.append((start, data))
        return start

    def entry(name, cluster, size):
        base, _, ext = name.upper().partition('.')
        if len(base) > 8 or len(ext) > 3:
            raise ValueError(f'{name}: not an 8.3 name')
        record = bytearray(32)
        record[:11] = (base.ljust(8) + ext.ljust(3)).encode('ascii')
        record[11] = 0x20
        struct.pack_into('<H', record, 20, cluster >> 16)
        struct.pack_into('<HI', record, 26, cluster & 65535, size)
        return record

    # The root directory is a cluster chain like any other file, and one
    # cluster is one 512-byte sector here -- sixteen entries.  It used to be
    # the single cluster 2, which held until a card wanted more than sixteen
    # files: the directory then ran off the end of its cluster and the write
    # below laid the overflow straight over the start of the next file,
    # which was kernel.elf.  The card looked well-formed and would not boot.
    root_clusters = max(1, (32 * len(files) + 511) // 512)
    for n in range(2, 2 + root_clusters):
        struct.pack_into('<I', fat, n * 4,
                         0x0fffffff if n + 1 == 2 + root_clusters else n + 1)
    next_cluster = 2 + root_clusters

    root = bytearray()
    for name, data in files.items():
        root += entry(name, allocate(data), len(data))
    payloads.append((2, root.ljust(root_clusters * 512, b'\0')))

    mbr = bytearray(512)
    mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', 0x0c,
                               b'\xfe\xff\xff', part, sectors)
    mbr[510:] = b'\x55\xaa'
    boot = bytearray(512)
    boot[:11] = b'\xeb\x58\x90WRRISCV '
    struct.pack_into('<HBHBHHBHHHII', boot, 11, 512, 1, reserved, 2, 0, 0,
                     0xf8, 0, 63, 255, part, sectors)
    struct.pack_into('<IHHIHH', boot, 36, fat_sectors, 0, 0, 2, 1, 6)
    boot[64] = 0x80
    boot[66] = 0x29
    struct.pack_into('<I', boot, 67, 0x52495343)
    boot[71:90] = b'WRRISCV    FAT32   '
    boot[510:] = b'\x55\xaa'
    info = bytearray(512)
    struct.pack_into('<I', info, 0, 0x41615252)
    struct.pack_into('<III', info, 484, 0x61417272,
                     clusters - (next_cluster - 2), next_cluster - 1)
    struct.pack_into('<I', info, 508, 0xaa550000)
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


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, default=here / 'riscv.app')
    parser.add_argument('--image', type=Path, default=here / 'build/rvbench.bin')
    parser.add_argument('--limit', type=int, default=4000000000,
                        help='emulator instruction budget')
    parser.add_argument('--timeout', type=int, default=3600)
    parser.add_argument('--keep', action='store_true')
    parser.add_argument('--gui', action='store_true',
                        help='show the panel in a window; clicks are touches')
    parser.add_argument('--launcher', action='store_true',
                        help='boot to the icon panel and pick from there')
    parser.add_argument('--args',
                        help='run the application with these arguments, through '
                             'a one-entry launcher: grifo chains straight into a '
                             'panel with a single icon, so this needs no tap')
    parser.add_argument('--profile', action='store_true',
                        help='write a per-address profile next to the log')
    parser.add_argument('--dump', metavar='ADDR,LEN',
                        help='write LEN bytes of memory from ADDR, as they '
                             'are when the run ends, to dump.bin next to the log')
    parser.add_argument('--window', metavar='START,END',
                        help='profile only between the first hits of two '
                             'addresses.  Without one the profile covers the '
                             'whole run, and grifo boots from the same A0 RAM '
                             'the interpreter later occupies -- its SPI loop '
                             'and the hot path share addresses, and the '
                             'profile cannot tell them apart')
    parser.add_argument('--expect', type=Path, default=here / 'build/ref.txt',
                        help='kernel checksums from the native reference build')
    parser.add_argument('--dtb', type=Path,
                        help='device tree; supplying one boots Linux instead')
    parser.add_argument('--type', help='text to type on the guest console')
    parser.add_argument('--type-at', type=int, default=1000000,
                        help='emulator instruction at which typing starts')
    parser.add_argument('--type-gap', type=int, default=50000)
    parser.add_argument('--tap', action='append', default=[],
                        metavar='X,Y,CYCLE',
                        help='press the on-screen keyboard at a pixel')
    args = parser.parse_args()

    # Outside the build tree on purpose: a `make clean` during a run would
    # otherwise delete the log the run is still writing to.
    out = Path(tempfile.mkdtemp(prefix='wr-riscv-'))
    card = out / 'card.img'
    files = {'KERNEL.ELF': (ROOT / 'samo-lib/grifo/grifo.elf').read_bytes()}
    if args.args:
        files['INIT.APP'] = (ROOT / 'samo-lib/grifo/applications/init/init.app').read_bytes()
        files['RISCV.APP'] = args.app.read_bytes()
        files['RISCV.ICO'] = (here / 'riscv.ico').read_bytes()
        files['RVBENCH.BIN'] = args.image.read_bytes()
        files['INIT.INI'] = f'riscv.ico : riscv.app {args.args}\n'.encode()
    elif args.launcher:
        # Grifo chains to init.app; the stock one reads init.ini and draws
        # the icon panel.  It only draws it for two entries or more -- with
        # one it boots straight into it -- so both go on the card.
        linux = here / 'linux'
        files['INIT.APP'] = (ROOT / 'samo-lib/grifo/applications/init/init.app').read_bytes()
        files['RISCV.APP'] = args.app.read_bytes()
        files['RISCV.ICO'] = (here / 'riscv.ico').read_bytes()
        files['RVBENCH.BIN'] = (here / 'build/rvbench.bin').read_bytes()
        entries = ['riscv.ico : riscv.app rvbench.bin']
        if (linux / 'Image').exists():
            files['RVLINUX.ICO'] = (here / 'rvlinux.ico').read_bytes()
            files['RVLINUX.BIN'] = (linux / 'Image').read_bytes()
            files['RVLINUX.DTB'] = (linux / 'wr-sh.dtb').read_bytes()
            entries.append('rvlinux.ico : riscv.app rvlinux.bin')
        files['INIT.INI'] = ('\n'.join(entries) + '\n').encode()
    else:
        files['INIT.APP'] = args.app.read_bytes()
        files['RVBENCH.BIN'] = args.image.read_bytes()
        if args.dtb:
            files['RVBENCH.DTB'] = args.dtb.read_bytes()
    make_card(card, files)

    flash = out / 'flash.rom'
    subprocess.run([sys.executable, str(ROOT / 'samo-lib/mbr/make-flash.py'),
                    str(flash)], check=True, stdout=subprocess.DEVNULL)
    # The card is writable: the application saves its report onto it, which
    # is how a run on the device is read back, and a run that cannot write is
    # not the run the hardware does.  Each run gets its own throwaway image.
    cmd = [str(ROOT / 'emulator/wremu'), '-c', str(card),
           '-e', str(flash), '-n', str(args.limit)]
    if args.gui:
        cmd += ['-g']
    if args.profile:
        cmd += ['-F', 'profile.txt']
    if args.dump:
        addr, length = args.dump.split(',')
        cmd += ['-D', addr, '-L', length, '-O', 'dump.bin']
    if args.window:
        cmd += ['-Y', args.window]
    for tap in args.tap:
        cmd += ['-T', tap]
    if args.type:
        keys = out / 'keys.txt'
        keys.write_text(args.type.replace('\\n', '\n'))
        cmd += ['--uart-input', str(keys), '--uart-start', str(args.type_at),
                '--uart-gap', str(args.type_gap)]
    print(' '.join(cmd), flush=True)
    log = out / 'run.log'
    with log.open('w') as handle:
        result = subprocess.run(cmd, cwd=out, stdout=handle,
                                stderr=subprocess.STDOUT, timeout=args.timeout)
    text = log.read_text(errors='replace')
    sys.stdout.write(''.join(line + '\n' for line in text.splitlines()
                             if args.dtb or args.args or line.startswith('rv32:') or 'bench' in line
                             or re.match(r'^(alu|branch|mul|div|load|store|copy|bytes|crc|sieve|done) ', line)))
    if 'rv32: total' not in text and not args.dtb and not args.args:
        print(f'run did not finish; full log at {log}', file=sys.stderr)
        return 1
    # On the device the card is the only copy of the numbers, so check here
    # that it really got them: the same write, the same filesystem driver.
    # Searching the raw image rather than mounting it -- the report is one
    # cluster, so it is contiguous, and this needs no privileges.
    if not args.dtb and not args.launcher and not args.args:
        # Searching the raw image rather than mounting it: no privileges
        # needed, and the report is one cluster so it cannot be fragmented.
        # The pattern has to include the numbers -- the unfilled format
        # strings are also on the card, inside the application itself.
        image = card.read_bytes()
        entry = image.find(b'RVBENCH TXT')
        size = struct.unpack('<I', image[entry + 28:entry + 32])[0] if entry >= 0 else 0
        if not size or not re.search(rb'rv32: total\s+\d+ insns', image):
            print('the application did not save its report to the card',
                  file=sys.stderr)
            return 1
        print(f'rv32: saved {size} bytes to rvbench.txt on the card')
    # The host test cannot exercise C33 assembly, so the checksums the guest
    # prints are the only thing standing between a fast interpreter and a
    # wrong one.  Compare them against the native reference every run.
    if args.expect.exists() and not args.dtb and not args.args:
        want = args.expect.read_text().split()
        got = re.findall(r'^(\w+) (0x[0-9a-f]{8})$', text, re.M)
        got = [piece for pair in got for piece in pair]
        if got != want:
            print('CHECKSUM MISMATCH', file=sys.stderr)
            for i in range(0, min(len(want), len(got)), 2):
                if got[i:i + 2] != want[i:i + 2]:
                    print(f'  {want[i]}: expected {want[i + 1]}, got {got[i + 1]}',
                          file=sys.stderr)
            if len(got) != len(want):
                print(f'  {len(got) // 2} kernels reported, {len(want) // 2} expected',
                      file=sys.stderr)
            return 1
    print(f'full log: {log}')
    if args.profile:
        print(f'profile: {out / "profile.txt"}')
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())
