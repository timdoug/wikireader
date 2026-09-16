#!/usr/bin/env python3
"""Run Doom's native arithmetic in wremu against host 64-bit references."""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def run(cmd, **kwargs):
    return subprocess.run([str(x) for x in cmd], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain', type=Path,
                        default=ROOT / 'host-tools/toolchain-c33/work/install/bin')
    args = parser.parse_args()
    out = ROOT / 'doom/build/test-math'
    out.mkdir(parents=True, exist_ok=True)
    runtime = ROOT / 'emulator/difftest/runtime'
    cc = args.toolchain / 'c33-epson-elf-gcc'
    common = ['-O2', '-fwrapv', '-fno-strict-aliasing', '-I', runtime]
    run(['cc', *common, '-DDT_HOST', ROOT / 'doom/tests/math.c', '-o', out / 'host'])
    expected = run([out / 'host'], capture_output=True).stdout.decode().splitlines()
    for src, obj in [(ROOT / 'doom/tests/math.c', 'test.o'),
                     (ROOT / 'doom/c33_math.c', 'math.o')]:
        run([cc, '-mc33pe', '-mno-long-calls', '-DWR_C33', *common,
             '-c', src, '-o', out / obj])
    run([args.toolchain / 'c33-epson-elf-as', '-mc33pe', runtime / 'start.s',
         '-o', out / 'start.o'])
    libgcc = run([cc, '-mc33pe', '-print-libgcc-file-name'],
                 capture_output=True, text=True).stdout.strip()
    run([args.toolchain / 'c33-epson-elf-ld', '-T', ROOT / 'doom/tests/math.lds',
         out / 'start.o', out / 'test.o', out / 'math.o', libgcc, '-o', out / 'math.elf'])
    with (out / 'run.log').open('w') as log:
        # --bare-elf: this is arithmetic compiled for the core, not firmware.
        run([ROOT / 'emulator/wremu', '-n', '300000000',
             '--bare-elf', out / 'math.elf'],
            cwd=out, stdout=log, stderr=subprocess.STDOUT, timeout=180)
    log = (out / 'run.log').read_text()
    actual = re.findall(r'^[0-9a-f]{8}$', log, re.M)
    if actual != expected:
        first = next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                     min(len(actual), len(expected)))
        raise RuntimeError(f'C33 math mismatch at result {first}; see {out / "run.log"}')
    print(f'C33 math: {len(actual)} exact results passed (including A0 calls)')


if __name__ == '__main__':
    main()
