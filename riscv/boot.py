#!/usr/bin/env python3
"""Time a Linux boot: cycles per guest instruction from reset to the shell.

A boot has no end to report at, so the application prints a line every
million guest instructions with what it has retired and what that cost, and
this watches the console for the kernel's `Run /bin/sh as init process`,
takes the first progress line after it, and stops the emulator there.  The
answer is therefore the average over the boot to within a million
instructions, which is about three percent of it.

    python3 boot.py                    the current build
    python3 boot.py --app X.app        another one
"""
import argparse
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
import time

import run as harness

ROOT = Path(__file__).resolve().parents[1]
HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=HERE / 'linux/Image')
    parser.add_argument('--dtb', type=Path, default=HERE / 'linux/wr-sh.dtb')
    parser.add_argument('--app', type=Path, default=HERE / 'riscv.app')
    parser.add_argument('--until', default='Run /bin/sh as init process')
    parser.add_argument('--timeout', type=int, default=2400)
    parser.add_argument('--profile', action='store_true',
                        help='write a per-address profile next to the log')
    parser.add_argument('--window', metavar='START,END',
                        help='profile only between the first hits of two addresses')
    parser.add_argument('--dump', metavar='ADDR,LEN',
                        help='write LEN bytes of memory from ADDR to dump.bin')
    args = parser.parse_args()

    out = Path(tempfile.mkdtemp(prefix='wr-boot-'))
    card = out / 'card.img'
    harness.make_card(card, {
        'KERNEL.ELF': (ROOT / 'samo-lib/grifo/grifo.elf').read_bytes(),
        'INIT.APP': args.app.read_bytes(),
        'RVBENCH.BIN': args.image.read_bytes(),
        'RVBENCH.DTB': args.dtb.read_bytes(),
    })
    flash = out / 'flash.rom'
    subprocess.run([sys.executable, str(ROOT / 'samo-lib/mbr/make-flash.py'),
                    str(flash)], check=True, stdout=subprocess.DEVNULL)

    cmd = [str(ROOT / 'emulator/wremu'), '-c', str(card), '-e', str(flash),
           '-n', '200000000000']
    if args.profile:
        cmd += ['-F', 'profile.txt']
    if args.window:
        cmd += ['-Y', args.window]
    if args.dump:
        addr, length = args.dump.split(',')
        cmd += ['-D', addr, '-L', length, '-O', 'dump.bin']
    started = time.time()
    proc = subprocess.Popen(cmd, cwd=out, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    fd = proc.stdout.fileno()
    log = (out / 'run.log').open('wb')
    text = b''
    seen = False
    result = None
    deadline = started + args.timeout
    while time.time() < deadline:
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        log.write(chunk)
        text += chunk
        if not seen and args.until.encode() in text:
            seen = True
            text = text[text.index(args.until.encode()):]
        if seen:
            m = re.search(rb'rv32: at (\d+) insns, (\d+) cycles', text)
            if m:
                result = int(m[1]), int(m[2])
                break
    proc.terminate()
    end = time.time() + 30
    while time.time() < end:
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        log.write(chunk)
    proc.wait(timeout=30)
    log.close()
    print(f'full log: {out / "run.log"}')
    if not result:
        print('did not reach the shell', file=sys.stderr)
        return 1
    insns, cycles = result
    print(f'{args.until!r}: {insns} guest insns, {cycles} cycles, '
          f'{cycles / insns:.2f} cyc/insn, {time.time() - started:.0f} s wall')
    return 0


if __name__ == '__main__':
    sys.exit(main())
