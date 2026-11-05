#!/usr/bin/env python3
"""Run Doom's native arithmetic in wremu against host 64-bit references."""
import argparse
from pathlib import Path
import os
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
    run([cc, '-mc33pe', '-mno-long-calls', '-DWR_C33', *common,
         '-c', ROOT / 'doom/c33_math.c', '-o', out / 'math.o'])
    run(['python3', ROOT / 'emulator/difftest/run.py',
         '--source', ROOT / 'doom/tests/math.c', '--toolchain', args.toolchain,
         '--work', out / 'grifo', '--extra-object', out / 'math.o',
         '--target-flags=-mno-long-calls -DWR_C33', '--file-output',
         '--limit', '2000000000', '--output', out / 'math.out'],
        env={**os.environ, 'DT_OPTS': '-O2 -fwrapv -fno-strict-aliasing'})
    actual = (out / 'math.out').read_text().splitlines()
    print(f'C33 math: {len(actual)} exact results passed (including A0 calls)')


if __name__ == '__main__':
    main()
