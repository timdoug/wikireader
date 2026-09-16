#!/usr/bin/env python3
"""Score the emulator's timing model against the device, and sweep it.

The four placement variants run the same guest workload with different
amounts of the interpreter in internal RAM, so together they separate an
error in what SDRAM costs from an error in what internal RAM costs -- which
one number could not.  The device's own reports are checked in beside this
script; nothing here needs the hardware.

  python3 fit-model.py                         score the model as it stands
  python3 fit-model.py --set dq_iram_extra=4   score it with that override
  python3 fit-model.py --sweep dq_iram_extra=2,3,4,5

The score is the RMS of log(device / model) over every kernel of every
variant, so being twice too fast and twice too slow count the same, and a
variant cannot be flattered by having large numbers.
"""
import argparse
import concurrent.futures
import math
import os
from pathlib import Path
import re
import subprocess
import sys

import compare

HERE = Path(__file__).resolve().parent

# variant -> the device report it is to be scored against
DEVICE = {
    'sdram': 'rvsdr-device.txt',
    'code': 'rva0-device.txt',
    'code+state': 'rva0s-device.txt',
    'asm': 'rvbench-device.txt',
}
KERNELS = ['alu', 'branch', 'mul', 'div', 'load', 'store', 'copy',
           'bytes', 'crc', 'sieve']


def read_report(text):
    """cyc/insn by kernel, from either a device file or a run.py log."""
    rows = {m[1]: float(m[2]) for m in
            re.finditer(r'rv32: (\w+)\s+\d+ \d+\s+([\d.]+)', text)}
    total = re.search(r'rv32: total.*?([\d.]+) cyc/insn', text)
    if total:
        rows['total'] = float(total[1])
    return rows


def device_tables():
    return {name: read_report((HERE / path).read_text())
            for name, path in DEVICE.items()}


def run(name, app, setting):
    out = subprocess.run(
        [sys.executable, str(HERE / 'run.py'), '--app', str(app),
         '--limit', '4000000000', '--timeout', '1800'],
        cwd=HERE, capture_output=True, text=True,
        env=dict(os.environ, WREMU_MODEL=setting) if setting else None)
    return name, read_report(out.stdout)


def evaluate(apps, setting):
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(apps)) as pool:
        model = dict(pool.map(lambda kv: run(*kv, setting), apps.items()))
    device, errors = device_tables(), []
    for variant in DEVICE:
        for kernel in KERNELS:
            m, d = model[variant].get(kernel), device[variant].get(kernel)
            if not m or not d:
                raise SystemExit(f'{variant}: no {kernel} in the run output')
            errors.append(math.log(d / m))
    rms = math.sqrt(sum(e * e for e in errors) / len(errors))
    return model, rms


def report(model, rms, setting):
    device = device_tables()
    print(f'\n{setting or "the model as it stands"}: RMS log error {rms:.4f}')
    print(f'{"":11}' + ''.join(f'{v:>22}' for v in DEVICE))
    print(f'{"":11}' + ''.join(f'{"model device ratio":>22}' for _ in DEVICE))
    for kernel in KERNELS + ['total']:
        row = f'{kernel:11}'
        for variant in DEVICE:
            m, d = model[variant][kernel], device[variant][kernel]
            row += f'{m:9.1f}{d:8.1f}{d / m:6.3f}'
        print(row)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--set', default='', metavar='NAME=VALUE[,...]',
                        help='WREMU_MODEL override to score')
    parser.add_argument('--sweep', metavar='NAME=V1,V2,...',
                        help='score one parameter at each of several values')
    parser.add_argument('--build', action='store_true',
                        help='relink the variants first')
    args = parser.parse_args()

    if args.build:
        apps = {name: compare.build(name, flags, 1)
                for name, flags in compare.VARIANTS.items()}
    else:
        apps = {name: HERE / 'build' / f'riscv-{name}.app'
                for name in compare.VARIANTS}
        missing = [str(p) for p in apps.values() if not p.exists()]
        if missing:
            raise SystemExit(f'run with --build first; missing {missing[0]}')

    if args.sweep:
        name, _, values = args.sweep.partition('=')
        best = None
        for value in values.split(','):
            setting = ','.join(filter(None, [args.set, f'{name}={value}']))
            model, rms = evaluate(apps, setting)
            print(f'{setting:48} RMS {rms:.4f}')
            if best is None or rms < best[1]:
                best = (setting, rms, model)
        print(f'\nbest: {best[0]}')
        report(best[2], best[1], best[0])
        return 0

    model, rms = evaluate(apps, args.set)
    report(model, rms, args.set)
    return 0


if __name__ == '__main__':
    sys.exit(main())
