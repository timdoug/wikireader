#!/usr/bin/env python3
"""Check actual Grifo suspend/resume, register restoration and retained RAM.

An ordinary idle wait is insufficient: power.txt must record completed deep
suspends. A bounded instruction run also catches A0 status-polling hangs.
This checks functional behaviour; it does not qualify physical wake timing.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, out):
    out.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).with_suffix('.c')
    app = out / 'smoke.app'
    command = [str(args.toolchain / 'c33-epson-elf-gcc'), '-O2', '-mc33pe',
               '-mlong-calls', '-fno-builtin', '-nostdlib',
               '-I' + str(ROOT / 'samo-lib/grifo/include'),
               '-I' + str(ROOT / 'samo-lib/include'),
               '-I' + str(ROOT / 'samo-lib/mini-libc/include'),
               '-Wl,-T,' + str(ROOT / 'samo-lib/grifo/lds/application.lds'),
               str(source), str(ROOT / 'samo-lib/grifo/lib/libgrifo.a'),
               str(ROOT / 'samo-lib/mini-libc/lib/libc.a'), '-lgcc', '-o', str(app)]
    subprocess.run(command, check=True)
    nm = subprocess.check_output([str(args.toolchain / 'c33-epson-elf-nm'), str(app)], text=True)
    anchor = re.search(r'^([0-9a-f]+) T suspend_input_anchor$', nm, re.M)
    if not anchor:
        raise RuntimeError('Missing suspend input anchor')
    spec = importlib.util.spec_from_file_location('fat', ROOT / 'emulator/tools/mem_dma_bench/run.py')
    fat = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fat)
    files = {'kernel.elf': (ROOT / 'samo-lib/grifo/grifo.elf').read_bytes(),
             'init.app': (ROOT / 'samo-lib/grifo/applications/init/init.app').read_bytes(),
             'smoke.app': app.read_bytes(), 'smoke.ico': bytes(512),
             'init.ini': b'smoke.ico : smoke.app\n', 'powerlog.on': b''}
    fat.make_image(out / 'card.img', files, 64)
    flash = fat.build_flash(out)
    command = [str(args.wremu), '-e', str(flash), '-c', str(out / 'card.img'),
               '-Z', '0x' + anchor[1], '-N', '2,1000000', '-n', '300000000']
    inputs = dict(command=command, emulator_sha256=sha(args.wremu),
                  source_sha256=sha(source), flash_sha256=sha(flash),
                  setting=os.environ.get('WREMU_MODEL', ''),
                  files={name: hashlib.sha256(data).hexdigest() for name, data in files.items()})
    (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    timed_out = False
    with (out / 'boot.log').open('w') as log:
        try:
            result = subprocess.run(command, cwd=out,
                                    env={**os.environ, 'WREMU_BUTTON_HOLD_MS': '120'},
                                    stdout=log, stderr=subprocess.STDOUT, timeout=120)
            returncode = result.returncode
        except subprocess.TimeoutExpired:
            timed_out, returncode = True, None
    text = (out / 'boot.log').read_text(errors='replace')
    power = fat.read_file(out / 'card.img', 'power.txt') or b''
    (out / 'power.txt').write_bytes(power)
    match = re.search(rb'suspend entries=(\d+) resumes=(\d+) timeouts=(\d+)', power)
    checks = dict(exit_status=returncode == 0 and not timed_out,
                  real_boot=all(s in text for s in ['load: kernel.elf', 'Grifo starting', 'init starting']),
                  native_restoration_and_data='SUSPEND_SMOKE_PASS' in text,
                  actual_deep_suspend=bool(match and int(match[1]) >= 1 and
                                           match[1] == match[2] and int(match[3]) == 0),
                  powered_off='stop reason: powered off' in text,
                  no_faults=not any(s in text for s in ['Panic:', 'C33 exception', '[watchdog reset]']),
                  executable_unchanged=sha(args.wremu) == inputs['emulator_sha256'])
    record = dict(**inputs, checks=checks, returncode=returncode, host_timeout=timed_out,
                  complete=all(checks.values()), log_sha256=sha(out / 'boot.log'))
    (out / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
    if not record['complete']:
        raise SystemExit(f'Suspend failed: {[k for k, v in checks.items() if not v]}; {out}')
    print(f'Grifo suspend/resume: {int(match[1])} completed; configuration and RAM restored; {out}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wremu', type=Path, default=ROOT / 'emulator/wremu')
    parser.add_argument('--toolchain', type=Path, default=ROOT / 'host-tools/toolchain-c33/work/install/bin')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    args.wremu = args.wremu.resolve()
    args.toolchain = args.toolchain.resolve()
    out = args.out or ROOT / 'build/wr128/suspend-check' / str(time.time_ns())
    run(args, out.resolve())


if __name__ == '__main__':
    main()
