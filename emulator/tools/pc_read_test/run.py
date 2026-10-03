#!/usr/bin/env python3
"""Run pctest in wremu through the launcher (MBR flash -> Grifo -> init.app
-> pctest.app) and print its results; with --device, beside the device's
pctest.log."""
import argparse
import importlib.util
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/wr128/pc-read'
spec = importlib.util.spec_from_file_location(
    'fat', ROOT / 'emulator/tools/mem_dma_bench/run.py')
fat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fat)


def results(text):
    return {m[1]: m[2] for m in re.finditer(r'^RESULT kind=(\S+) match=(\S+)', text, re.M)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--device', type=Path, help="the device's pctest.log")
    args = ap.parse_args()
    subprocess.run(['make', '-s', '-C', str(Path(__file__).parent)], check=True)
    work = Path(tempfile.mkdtemp(prefix='pctest.', dir='/tmp'))
    fat.make_image(work / 'card.img', {
        'kernel.elf': (ROOT / 'samo-lib/grifo/grifo.elf').read_bytes(),
        'init.app': (ROOT / 'samo-lib/grifo/applications/init/init.app').read_bytes(),
        'pctest.app': (OUT / 'pctest.app').read_bytes(),
        'pctest.ico': (OUT / 'pctest.ico').read_bytes(),
        'init.ini': b'pctest.ico : pctest.app off noplain\n',
    }, 1)
    subprocess.run([sys.executable, str(ROOT / 'samo-lib/mbr/make-flash.py'),
                    str(work / 'flash.rom')], check=True, stdout=subprocess.DEVNULL)
    run = subprocess.run([str(ROOT / 'emulator/wremu'), '-n', '400000000',
                          '-e', str(work / 'flash.rom'), '-c', str(work / 'card.img')],
                         cwd=work, text=True, errors='replace',
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (work / 'boot.log').write_text(run.stdout)
    # The serial port lags the app, which powers off when done: read the
    # log it wrote to the card.
    log = (fat.read_file(work / 'card.img', 'pctest.log') or b'').decode()
    if 'END PCTEST' not in log:
        sys.exit(f'pctest did not finish; see {work}/boot.log')
    emu = results(log)
    dev = results(args.device.read_text()) if args.device else {}
    print(f'# {work}')
    print(f'{"kind":14} {"wremu":>12} {"device":>12}')
    for kind, match in emu.items():
        print(f'{kind:14} {match:>12} {dev.get(kind, ""):>12}')


if __name__ == '__main__':
    main()
