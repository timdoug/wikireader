#!/usr/bin/env python3
"""Run spibench in wremu through the launcher (MBR flash -> Grifo -> init.app
-> spibench.app) and print its results, against a device spibench.log when
given one.  WREMU_MODEL passes through to the emulator."""
import argparse
import importlib.util
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/wr128/spi-dma'
spec = importlib.util.spec_from_file_location(
    'fat', ROOT / 'emulator/tools/mem_dma_bench/run.py')
fat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fat)

FIELD = re.compile(r'(\w+)=(\S+)')


def results(text):
    rows = {}
    for line in text.splitlines():
        if line.startswith('RESULT '):
            f = dict(FIELD.findall(line))
            rows[(f['tx'], f['width'], f['dst'], f['cpu'])] = f
    return rows


def emulate(wremu, budget):
    work = Path(tempfile.mkdtemp(prefix='spibench.', dir='/tmp'))
    files = {
        'kernel.elf': (ROOT / 'samo-lib/grifo/grifo.elf').read_bytes(),
        'init.app': (ROOT / 'samo-lib/grifo/applications/init/init.app').read_bytes(),
        'spibench.app': (OUT / 'spibench.app').read_bytes(),
        'spibench.ico': (OUT / 'spibench.ico').read_bytes(),
        'init.ini': b'spibench.ico : spibench.app off\n',
    }
    fat.make_image(work / 'card.img', files, 1)
    subprocess.run([sys.executable, str(ROOT / 'samo-lib/mbr/make-flash.py'),
                    str(work / 'flash.rom')], check=True,
                   stdout=subprocess.DEVNULL)
    with open(work / 'boot.log', 'w') as log:
        subprocess.run([wremu, '-n', str(budget), '-c', str(work / 'card.img'),
                        '-e', str(work / 'flash.rom')], cwd=work, stdout=log,
                       stderr=subprocess.STDOUT,
                       env={**os.environ, 'WREMU_UART_TRACE': ''})
    return work, (work / 'boot.log').read_text(errors='replace')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--device', type=Path, help="the device's spibench.log")
    ap.add_argument('--wremu', default=str(ROOT / 'emulator/wremu'))
    ap.add_argument('-n', type=int, default=3000000000)
    ap.add_argument('--no-build', action='store_true')
    args = ap.parse_args()
    if not args.no_build:
        subprocess.run(['make', '-s', '-C', str(Path(__file__).parent)], check=True)
    work, text = emulate(args.wremu, args.n)
    emu = results(text)
    if 'END SPIBENCH' not in text:
        sys.exit(f'spibench did not finish; see {work}/boot.log')
    dev = results(args.device.read_text()) if args.device else {}
    print(f'# {work}')
    print(f'{"tx":5} {"width":>5} {"dst":5} {"cpu":11} {"unit":>7} {"dev":>7} {"err":>6}'
          f' {"pass":>7} {"dev":>7} {"solo":>6} {"dev":>6} ok')
    for key, e in emu.items():
        d = dev.get(key, {})
        unit = int(e['unit_x100']) / 100
        dunit = int(d['unit_x100']) / 100 if d else None
        err = f'{100 * (unit - dunit) / dunit:+.1f}%' if dunit else ''
        cells = [f'{int(x["pass_x100"]) / 100:.1f}' if x else ''
                 for x in (e, d)]
        solos = [f'{int(x["solo_pass_x100"]) / 100:.1f}' if x else ''
                 for x in (e, d)]
        print(f'{key[0]:5} {key[1]:>5} {key[2]:5} {key[3]:11} {unit:7.1f} '
              f'{dunit if dunit else "":>7} {err:>6} {cells[0]:>7} {cells[1]:>7}'
              f' {solos[0]:>6} {solos[1]:>6} {e["ok"]}{d.get("ok", "")}')


if __name__ == '__main__':
    main()
