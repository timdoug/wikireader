#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.0-or-later
"""Cross-link C33 TinyCC and GCC in all four caller/callee combinations."""
import argparse
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--wikireader', type=Path, default=Path.home() / 'wikireader')
p.add_argument('--out', type=Path, default=root.parent / 'build/wikireader/tinycc-abi')
a = p.parse_args()
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
binpath = a.wikireader / 'host-tools/toolchain-c33/work/install/bin'
gcc = str(binpath / 'c33-epson-elf-gcc')
ld = str(binpath / 'c33-epson-elf-ld')
nm = str(binpath / 'c33-epson-elf-nm')
src = root / 'tests/c33'

def run(cmd, name, cwd=out):
    result = subprocess.run([str(x) for x in cmd], cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
    (out / (name + '.log')).write_text(result.stdout)
    if result.returncode:
        raise SystemExit(f'{name} failed:\n{result.stdout[-5000:]}')
    return result.stdout

host = out / 'host'
host.mkdir(exist_ok=True)
run([root / 'configure', '--cc=cc', '--config-bcheck=no', '--config-backtrace=no'], 'configure', host)
run(['gmake', '-j4', 'c33-tcc'], 'host-build', host)
tcc = host / 'c33-tcc'
base = [gcc, '-mc33pe', '-medda32', '-O2', '-g0', '-ffreestanding', '-I', src]
for file in ('start.S', 'runtime.c'):
    run(base + ['-c', src / file, '-o', out / (file + '.o')], file)
for side in ('caller', 'callee'):
    for compiler in ('gcc', 'tcc'):
        cmd = base if compiler == 'gcc' else [tcc, '-nostdlib', '-I' + str(root / 'include'), '-I' + str(src)]
        run(cmd + ['-c', src / ('abi-' + side + '.c'), '-o', out / (compiler + '-' + side + '.o')], compiler + '-' + side)
libgcc = subprocess.check_output([gcc, '-mc33pe', '-medda32', '-print-libgcc-file-name'], text=True).strip()
for caller in ('gcc', 'tcc'):
    for callee in ('gcc', 'tcc'):
        name = caller + '-calls-' + callee
        elf = out / (name + '.elf')
        run([ld, '-T', src / 'target.ld', out / 'start.S.o',
             out / (caller + '-caller.o'), out / (callee + '-callee.o'),
             out / 'runtime.c.o', libgcc, '-o', elf], name + '-link')
        symbols = subprocess.check_output([nm, elf], text=True)
        addr = re.search(r'^([0-9a-f]+) T test_done$', symbols, re.M)[1]
        log = run([a.wikireader / 'emulator/wremu', '-n', '10000000', '-b', '0x' + addr, elf], name)
        if '\nC33_ABI_OK\n' not in log or 'C33_ABI_FAIL' in log or 'stop reason: runaway' in log:
            raise SystemExit(f'{name} failed: {out / (name + ".log")}')
        print('PASS:', name)
print('Logs:', out)
