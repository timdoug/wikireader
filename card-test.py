#!/usr/bin/env python3
"""Run every launcher entry of a card image the way the device is used.

Each entry runs on its own copy of the image, booted through the real chain
-- FLASH loader, kernel.elf, init.app -- with its icon tapped on the
launcher, then driven on the panel:

  zim      types "cat" on the reader's keyboard and opens the first result
  doom     waits for the engine's ready line
  gameboy  starts the third game on its list
  nuttx    waits for the shell
  spibench waits for its last case (a calibration run, when on a card)
  linux    types t, then check, on the panel's keyboard and reads
           thread.txt and check.txt back off the card

Every run's console is searched for kernel faults, and the final screens go
into one contact sheet (screens.png) to look at.  An entry the script does
not know is tapped and its screen kept.

  python3 card-test.py build/wr-card.dmg [--output DIR] [--jobs N]
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parent
WREMU = ROOT / 'emulator/wremu'
# Launcher icon centres: three to a row, rows 68 pixels apart.
COLUMNS = (40, 120, 200)
ROW0, ROW_PITCH = 36, 68
TAP_AT = 100000000
LINUX_READY = ('C33 userspace console: DRM + evdev + PTY shell ready'
               '|C33 input: soft keyboard registered as a uinput device')
T_MARKERS = ('NPTL PASS', 'PTHREAD PASS', 'STATIC PTHREAD PASS', 'ATOMIC PASS',
             'IPC PASS', 'CXX PASS', 'BENCH PASS', 'STRACE PASS')
FAULTS = re.compile(r'C33 exception|Kernel panic|Oops|BUG:|unhandled C33')


class Card:
    """A card image's partitions, attached read-only under a private path."""

    def __init__(self, image):
        self.dir = Path(tempfile.mkdtemp(prefix='wr-card-test-mnt-'))
        out = subprocess.run(['hdiutil', 'attach', '-readonly', '-mountroot',
                              str(self.dir), str(image)],
                             check=True, capture_output=True, text=True).stdout
        self.disk = out.split()[0]
        self.boot = next(self.dir.glob('WRBOOT*'))

    def close(self):
        subprocess.run(['hdiutil', 'detach', self.disk], capture_output=True)
        shutil.rmtree(self.dir, ignore_errors=True)


def entries(image):
    card = Card(image)
    try:
        lines = (card.boot / 'init.ini').read_text().splitlines()
    finally:
        card.close()
    found = []
    for n, line in enumerate(l for l in lines if l.strip()):
        app = line.split(':', 1)[1].split()[0].removesuffix('.app')
        found.append((app, (COLUMNS[n % 3], ROW0 + ROW_PITCH * (n // 3))))
    return found


def run(image, flash, out, name, tap, extra, cycles):
    """Boot a copy of IMAGE, tap TAP, run CYCLES; return (log, card copy)."""
    work = out / name
    work.mkdir(parents=True, exist_ok=True)
    card = work / 'card.img'
    card.unlink(missing_ok=True)
    subprocess.run(['cp', '-c', str(image), str(card)], check=True)
    command = [str(WREMU), '-n', str(cycles), '-T', f'{tap[0]},{tap[1]},{TAP_AT}',
               *extra, '-c', str(card), '-e', str(flash)]
    with (work / 'log').open('w') as log:
        subprocess.run(command, cwd=work, stdout=log, stderr=subprocess.STDOUT,
                       timeout=1800, env={**os.environ, 'WREMU_HOLD_MS': '33'})
    return (work / 'log').read_bytes().decode(errors='replace'), card


def test_entry(image, flash, out, app, tap):
    problems = []
    if app == 'linux':
        typing = ['--keyboard', 'linux', '--type-after', LINUX_READY]
        log, card = run(image, flash, out, 'linux-t', tap,
                        typing + ['-K', '0,t#'], 12000000000)
        logs = [log]
        mounted = Card(card)
        try:
            thread = (mounted.boot / 'thread.txt').read_text(errors='replace') \
                if (mounted.boot / 'thread.txt').exists() else ''
        finally:
            mounted.close()
        problems += [f'thread.txt lacks {m}' for m in T_MARKERS if m not in thread]
        if 'FAIL' in thread or "can't" in thread:
            problems.append('thread.txt: ' + ' / '.join(
                l for l in thread.splitlines() if 'FAIL' in l or "can't" in l))
        log, card = run(image, flash, out, 'linux-check', tap,
                        typing + ['-K', '0,check#'], 12000000000)
        logs.append(log)
        mounted = Card(card)
        try:
            check = (mounted.boot / 'check.txt').read_text(errors='replace') \
                if (mounted.boot / 'check.txt').exists() else ''
            app_md5 = hashlib.md5((mounted.boot / 'linux.app').read_bytes()).hexdigest()
        finally:
            mounted.close()
        if not check:
            problems.append('check wrote no check.txt')
        if FAULTS.search(check):
            problems.append('check.txt: ' + FAULTS.search(check).group(0) + ' in the kernel log')
        if check and app_md5 not in check:
            problems.append('check.txt: linux.app md5 does not match the card')
        screens = [out / 'linux-t/screen.pgm', out / 'linux-check/screen.pgm']
    else:
        script = {
            'zim': (['-K', '700000000,cat', '-T', '30,40,1100000000'], 3000000000,
                    'starting ZIM reader'),
            'doom': ([], 1500000000, 'Doom ready:'),
            'gameboy': (['-T', '120,101,1000000000'], 2500000000, None),
            'nuttx': ([], 2500000000, 'WIKIREADER_TERMINAL_READY'),
            'spibench': ([], 40000000000, 'END SPIBENCH'),
        }.get(app, ([], 2000000000, None))
        extra, cycles, ready = script
        log, _ = run(image, flash, out, app, tap, extra, cycles)
        logs = [log]
        if ready and ready not in log:
            problems.append(f'no "{ready}" on the console')
        screens = [out / app / 'screen.pgm']
    for log in logs:
        if 'init choosing' not in log:
            problems.append('the launcher never came up')
        fault = FAULTS.search(log)
        if fault:
            problems.append(f'console: {fault.group(0)}')
    return app, problems, screens


def read_pgm(path):
    head, rest = path.read_bytes().split(b'\n', 1)
    dims, rest = rest.split(b'\n', 1)
    _, rest = rest.split(b'\n', 1)
    w, h = map(int, dims.split())
    return w, h, rest[:w * h]


def contact_sheet(screens, path):
    images = [read_pgm(s) for s in screens if s.exists()]
    if not images:
        return
    w, h = images[0][0], images[0][1]
    gap = 6
    width = len(images) * w + (len(images) - 1) * gap
    rows = []
    for y in range(h):
        row = b''
        for i, (_, _, pixels) in enumerate(images):
            row += pixels[y * w:(y + 1) * w] + (b'\x80' * gap if i < len(images) - 1 else b'')
        rows.append(b'\0' + row)

    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data +
                struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('>IIBBBBB', width, h, 8, 0, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--jobs', type=int, default=3)
    parser.add_argument('--entry', action='append', help='only this launcher entry')
    args = parser.parse_args()
    image = args.image.resolve()
    out = (args.output or Path(tempfile.mkdtemp(prefix='wr-card-test-'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    flash = out / 'flash.rom'
    flash.unlink(missing_ok=True)
    subprocess.run([sys.executable, str(ROOT / 'samo-lib/mbr/make-flash.py'), str(flash)],
                   check=True, stdout=subprocess.DEVNULL)
    found = [e for e in entries(image) if not args.entry or e[0] in args.entry]
    print('launcher:', ', '.join(f'{app} at {tap[0]},{tap[1]}' for app, tap in found), flush=True)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(lambda e: test_entry(image, flash, out, *e), found))
    failed = False
    screens = []
    for app, problems, shots in results:
        screens += shots
        print(f'{app}: ' + ('PASS' if not problems else 'FAIL: ' + '; '.join(problems)))
        failed |= bool(problems)
    contact_sheet(screens, out / 'screens.png')
    print(f'screens: {out / "screens.png"}  (runs in {out})')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
