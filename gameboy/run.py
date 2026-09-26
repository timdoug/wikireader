#!/usr/bin/env python3
"""Benchmark the Game Boy under the full-system emulator, and check it.

Boots the real chain -- FLASH loader, kernel.elf, init.app -- into
gameboy.app with a scripted run, then compares the frame hashes the C33
build printed with the host build's (tests/host.c, `make test`).  A match
means the cross-compiled emulator computed the same pictures and the same
RAM, frame for frame.

The default is Libbet and the Magic Floor (zlib licence, pinobatch/libbet
v0.08): A at frame 300 skips the intro, Select at 400 starts its gameplay
demo, and the frames after 420 are timed.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / 'riscv'))
import run as harness                                  # its FAT32 card builder

LIBBET_SCRIPT = '300:A,306:-,400:s,406:-'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rom', type=Path)
    parser.add_argument('--frames', type=int, default=540)
    parser.add_argument('--window', type=int, default=420)
    parser.add_argument('--script', default=LIBBET_SCRIPT)
    parser.add_argument('--limit', type=int, default=6000000000,
                        help='C33 cycles before the emulator gives up')
    parser.add_argument('--profile', action='store_true',
                        help='write the PC profile to build/profile.txt; see hotspots.py')
    parser.add_argument('--gui', action='store_true')
    parser.add_argument('--state', type=Path,
                        help='start from a save state (build/host ... -save FILE)')
    parser.add_argument('--serial', action='store_true',
                        help="copy the game's link-port output to the console (test ROMs)")
    parser.add_argument('--play', type=int, metavar='CYCLES',
                        help='boot the game to play instead, stop after CYCLES and keep '
                             'the panel as screen.png beside the log')
    parser.add_argument('--tap', action='append', default=[], metavar='X,Y,CYCLE')
    parser.add_argument('--button', action='append', default=[], metavar='CODE,CYCLE',
                        help='0 random (Start), 1 search (B), 2 history (A)')
    args = parser.parse_args()

    host = HERE / 'build/host'
    if not host.exists():
        raise SystemExit('build the host reference first: make -C gameboy test')
    reference = subprocess.run([str(host), str(args.rom), str(args.frames), args.script]
                               + (['-load', str(args.state)] if args.state else []),
                               check=True, capture_output=True, text=True).stdout
    expected = dict(re.findall(r'frame (\d+) hash (\w+)', reference))

    out = Path(tempfile.mkdtemp(prefix='wr-gameboy-'))
    # The card is FAT with 8.3 names.
    name = re.sub(r'[^A-Z0-9]', '', args.rom.stem.upper())[:8] + args.rom.suffix.upper()[:4]
    arguments = f'{name.lower()} frames={args.frames} window={args.window}'
    if args.script:
        arguments += f' script={args.script}'
    if args.serial:
        arguments += ' serial'
    if args.state:
        arguments += ' state=state.bin'
    if args.play:
        arguments = name.lower()
        args.limit = args.play
    files = {
        'KERNEL.ELF': (ROOT / 'samo-lib/grifo/grifo.elf').read_bytes(),
        'INIT.APP': (ROOT / 'samo-lib/grifo/applications/init/init.app').read_bytes(),
        'GAMEBOY.APP': (HERE / 'gameboy.app').read_bytes(),
        'GAMEBOY.ICO': (HERE / 'gameboy.ico').read_bytes(),
        'INIT.INI': f'gameboy.ico : gameboy.app {arguments}\n'.encode(),
        name: args.rom.read_bytes(),
    }
    if args.state:
        files['STATE.BIN'] = args.state.read_bytes()
    harness.make_card(out / 'card.img', files)
    subprocess.run([sys.executable, str(ROOT / 'samo-lib/mbr/make-flash.py'),
                    str(out / 'flash.rom')], check=True, stdout=subprocess.DEVNULL)
    cmd = [str(ROOT / 'emulator/wremu'), '-c', str(out / 'card.img'),
           '-e', str(out / 'flash.rom'), '-n', str(args.limit)]
    if args.profile:
        # Windowed from the first emulated frame to the power-off: Grifo's
        # boot and card reads share nothing with the run being measured.
        mapfile = (HERE / 'gameboy.map').read_text()
        start, end = (re.search(r'^\s+(0x[0-9a-f]+)\s+' + name + r'$', mapfile, re.M)[1]
                      for name in ('gbw_run_frame', 'power_off'))
        cmd += ['-F', str(HERE / 'build/profile.txt'), '-Y', f'{start},{end}']
    if args.gui:
        cmd += ['-g']
    for tap in args.tap:
        cmd += ['-T', tap]
    for press in args.button:
        cmd += ['-N', press]
    print(' '.join(cmd), flush=True)
    log = out / 'run.log'
    with log.open('w') as handle:
        subprocess.run(cmd, cwd=out, stdout=handle, stderr=subprocess.STDOUT)
    text = log.read_text(errors='replace')
    if args.play:
        subprocess.run(['sips', '-s', 'format', 'png', str(out / 'screen.pgm'),
                        '--out', str(out / 'screen.png')], stdout=subprocess.DEVNULL)
        print(f'gb: panel at {out / "screen.png"}; log at {log}')
        return 0
    for line in text.splitlines():
        if line.startswith('gb:') and ' hash ' not in line:
            print(line)

    got = dict(re.findall(r'frame (\d+) hash (\w+)', text))
    wrong = [f for f in sorted(got, key=int) if expected.get(f) != got[f]]
    if wrong:
        print(f'gb: hashes differ from the host from frame {wrong[0]} '
              f'({len(wrong)} of {len(got)}); log at {log}', file=sys.stderr)
        return 1
    if 'gb: done' not in text:
        print(f'gb: run did not finish; {len(got)} hashes matched; log at {log}',
              file=sys.stderr)
        return 1
    print(f'gb: all {len(got)} frame hashes match the host; log at {log}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
