#!/usr/bin/env python3
"""Boot the emulator to the icon launcher with everything installed.

The screen equivalent of install-card.sh: it puts the same applications and
icons on a disposable card image and boots the real chain -- FLASH loader,
kernel.elf, init.app -- so what you see is what the device shows.  Entries
whose application is not built are left out rather than shown dead.

  python3 run-launcher.py            a window; press power, then tap an icon
  python3 run-launcher.py --headless run without SDL and write screen.pgm
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'riscv'))
import run as harness                                  # its FAT32 card builder

# name on the card, file, launcher arguments; order is the icon order
ENTRIES = [
    ('zim',     'zim/zim.app',       'zim/zim.ico',     'started-from-init'),
    ('doom',    'doom/doom.app',     'doom/doom.ico',   '-wrbench'),
    ('nuttx',   'nuttx/nuttx.app',   'nuttx/nuttx.ico', ''),
    ('riscv',   'riscv/riscv.app',   'riscv/riscv.ico', 'rvbench.bin hold'),
    ('rvlinux', None,                'riscv/rvlinux.ico', 'rvlinux.bin'),
]
# extra payloads an entry needs on the card
PAYLOAD = {
    'riscv':   [('RVBENCH.BIN', 'riscv/build/rvbench.bin')],
    'rvlinux': [('RVLINUX.BIN', 'riscv/linux/Image'),
                ('RVLINUX.DTB', 'riscv/linux/wr-sh.dtb')],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--headless', action='store_true')
    parser.add_argument('--limit', type=int, default=400000000000)
    parser.add_argument('--tap', action='append', default=[], metavar='X,Y,CYCLE')
    args = parser.parse_args()

    out = Path(tempfile.mkdtemp(prefix='wr-launcher-'))
    files = {
        'KERNEL.ELF': (ROOT / 'samo-lib/grifo/grifo.elf').read_bytes(),
        'INIT.APP': (ROOT / 'samo-lib/grifo/applications/init/init.app').read_bytes(),
    }
    lines, shown = [], []
    for name, app, icon, arguments in ENTRIES:
        needed = [p for p in ([app] if app else []) + [icon]
                  + [src for _, src in PAYLOAD.get(name, [])]]
        if any(not (ROOT / p).exists() for p in needed):
            continue
        if app:
            files[f'{name.upper()[:8]}.APP'] = (ROOT / app).read_bytes()
        files[f'{name.upper()[:8]}.ICO'] = (ROOT / icon).read_bytes()
        for card_name, src in PAYLOAD.get(name, []):
            files[card_name] = (ROOT / src).read_bytes()
        runs = name if app else ENTRIES[3][0]        # rvlinux runs riscv.app
        lines.append(f'{name}.ico : {runs}.app {arguments}'.rstrip())
        shown.append(name)
    files['INIT.INI'] = ('\n'.join(lines) + '\n').encode()

    if len(shown) < 2:
        raise SystemExit('the launcher needs two entries or more; build some apps')
    print('launcher entries:', ', '.join(shown), flush=True)

    harness.make_card(out / 'card.img', files)
    subprocess.run([sys.executable, str(ROOT / 'samo-lib/mbr/make-flash.py'),
                    str(out / 'flash.rom')], check=True, stdout=subprocess.DEVNULL)
    cmd = [str(ROOT / 'emulator/wremu'), '-c', str(out / 'card.img'),
           '-e', str(out / 'flash.rom'), '-n', str(args.limit)]
    if not args.headless:
        cmd.append('-g')
    for tap in args.tap:
        cmd += ['-T', tap]
    print(' '.join(cmd), flush=True)
    subprocess.run(cmd, cwd=out)
    print(f'panel: {out / "screen.pgm"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
