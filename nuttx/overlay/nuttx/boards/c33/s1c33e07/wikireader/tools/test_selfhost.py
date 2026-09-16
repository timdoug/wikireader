#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bootstrap TinyCC through three native generations in the WikiReader."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import wr_boot
from test_terminal import read_pgm, write_png


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wikireader', type=Path, default=Path.home() / 'wikireader')
    parser.add_argument('--image', type=Path, default=root / 'nuttx')
    parser.add_argument('--out', type=Path, default=root / 'build/wikireader/selfhost')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    image = out / 'kernel.elf'
    if args.image.resolve() != image:
        shutil.copy2(args.image, image)
    # Write fixtures before the long bootstrap so the preloaded UART stream
    # cannot overrun the console input buffers while NSH waits for TinyCC.
    commands = ['tcc -h']
    for name in ('regression', 'float'):
        source = root / 'tinycc/tests/c33' / (name + '.c')
        for i, line in enumerate(source.read_text().splitlines()):
            if "'" in line:
                raise SystemExit('Fixture needs NSH quote escaping')
            commands.append(f"echo '{line}' {'>' if i == 0 else '>>'} /tmp/{name}.c")
    # Compare objects inside NuttX, without accessing their guest tmpfs from
    # the host. Print a checksum and size for the reproducibility record.
    compare = [
        '#include <stdio.h>', '#include <string.h>', '#include <setjmp.h>',
        'static jmp_buf env; static void jump(int n) { if(n)jump(n-1); else longjmp(env,17); }',
        'int main(void) { FILE *a=fopen("/tmp/tcc2.o","rb"), *b=fopen("/tmp/tcc3.o","rb");',
        'int val=setjmp(env); if(!val)jump(7); if(val!=17)return 3;',
        'char x[256],y[256]; unsigned n,m,i,h=2166136261U,total=0; if(!a||!b)return 1;',
        'do { n=fread(x,1,256,a); m=fread(y,1,256,b); if(n!=m||memcmp(x,y,n))return 2;',
        'for(i=0;i<n;i++)h=(h^(unsigned char)x[i])*16777619U; total+=n; } while(n);',
        'fclose(a); fclose(b); printf("BOOTSTRAP_MATCH %u %08x\\n",total,h); return 0; }',
    ]
    for i, line in enumerate(compare):
        commands.append(f"echo '{line}' {'>' if i == 0 else '>>'} /tmp/compare.c")
    commands += [
        'tcc -selfhost', 'echo STAGE1_STATUS=$?',
        'tcc -run /tmp/tcc.o -c /tmp/tcc/src/bootstrap.c -o /tmp/tcc2.o',
        'echo STAGE2_STATUS=$?',
        'tcc -run /tmp/tcc2.o -c /tmp/tcc/src/bootstrap.c -o /tmp/tcc3.o',
        'echo STAGE3_STATUS=$?',
        'tcc -run /tmp/tcc3.o -run /tmp/compare.c', 'echo MATCH_STATUS=$?',
        'tcc -run /tmp/tcc3.o -run /tmp/regression.c', 'echo INTEGER_STATUS=$?',
        'tcc -run /tmp/tcc3.o -run /tmp/float.c', 'echo FLOAT_STATUS=$?',
        'tcc -run /tmp/tcc3.o -c /tmp/float.c -o /tmp/float.o', 'echo OBJECT_STATUS=$?',
        'tcc -run /tmp/tcc3.o -run /tmp/float.o', 'echo RELOAD_STATUS=$?',
        "tcc -run /tmp/tcc3.o -e 'int main(void) { return unknown; }'", 'echo ERROR_STATUS=$?',
        'tcc -run /tmp/tcc3.o -run /tmp/hello.c', 'echo RECOVERY_STATUS=$?',
        "tcc -run /tmp/tcc3.o -e 'void exit(int); int main(void) { exit(23); }'", 'echo EXIT_STATUS=$?',
        'tcc -run /tmp/tcc3.o -run /tmp/fib.c', 'echo FIB_STATUS=$?',
        'echo SELFHOST_TEST_DONE',
    ]
    if any(len(line) >= 255 for line in commands):
        raise SystemExit('Fixture exceeds NSH line length')
    (out / 'input.txt').write_text('\n'.join(commands) + '\n')
    card = out / 'card.img'
    wr_boot.make_card(card, image, args.wikireader)
    flash = wr_boot.make_flash(out, args.wikireader)
    command = [str(args.wikireader / 'emulator/wremu'),
               '-n', str(12_000_000_000 + wr_boot.BOOT_CYCLES),
               '--uart-input', str(out / 'input.txt'),
               '--uart-start', wr_boot.UART_START,
               '--uart-gap', '250000', *wr_boot.boot_args(card, flash)]
    (out / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (out / 'emulator.log').open('w') as log:
        subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=600)
    text = (out / 'emulator.log').read_text(errors='replace').replace('\r', '')
    for name in ('STAGE1', 'STAGE2', 'STAGE3', 'MATCH', 'INTEGER', 'FLOAT',
                 'OBJECT', 'RELOAD', 'RECOVERY', 'FIB'):
        if f'\n{name}_STATUS=0\n' not in text or f'\n{name}_STATUS=1\n' in text:
            raise SystemExit(f'{name} failed: {out / "emulator.log"}')
    for name in ('ERROR', 'EXIT'):
        if f'\n{name}_STATUS=1\n' not in text:
            raise SystemExit(f'{name} did not fail')
    match = re.search(r'\nBOOTSTRAP_MATCH (\d+) ([0-9a-f]+)\n', text)
    if not match or '\nC33_REGRESSION_OK 3659629602\n' not in text or text.count('\nC33_FLOAT_OK\n') != 2:
        raise SystemExit('Compiled compiler failed a regression')
    for bad in ('C33: fatal', 'compiler is already running', 'unmapped read',
                'unmapped write', 'runaway:', 'Assertion failed', '\nC33_FLOAT_FAIL', '\nFAIL line'):
        if bad in text:
            raise SystemExit(f'Unexpected {bad!r}')
    if "\n<string>:1: error: 'unknown' undeclared\n" not in text:
        raise SystemExit('Incorrect diagnostic filename or line number')
    interrupt = out / 'interrupt'
    interrupt.mkdir(exist_ok=True)
    (interrupt / 'input.txt').write_text(
        "tcc -selfhost\ntcc -run /tmp/tcc.o -e 'int main(void) { for (;;) {} }'\n" +
        ' ' * 512 + '\x03\n' +
        'tcc -run /tmp/tcc.o -run /tmp/hello.c\necho INTERRUPT_STATUS=$?\n')
    command = [str(args.wikireader / 'emulator/wremu'),
               '-n', str(2_000_000_000 + wr_boot.BOOT_CYCLES),
               '--uart-input', str(interrupt / 'input.txt'),
               '--uart-start', wr_boot.UART_START,
               '--uart-gap', '1000000', *wr_boot.boot_args(card, flash)]
    (interrupt / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (interrupt / 'emulator.log').open('w') as log:
        subprocess.run(command, cwd=interrupt, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=240)
    interrupted = (interrupt / 'emulator.log').read_text(errors='replace').replace('\r', '')
    if ('\nINTERRUPT_STATUS=0\n' not in interrupted or
            '\nHello from native C33 C! argc=1\n' not in interrupted):
        raise SystemExit('Nested compiler cleanup after Ctrl-C failed')
    write_png(out / 'screen.png', read_pgm(out / 'screen.pgm'))
    metrics = {'elf_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
               'object_bytes': int(match[1]), 'object_fnv1a': match[2],
               'stage2_stage3_identical': True,
               'nested_ctrl_c_recovery': True,
               'executed_instructions': int(re.search(r'--- work: (\d+) instructions', text)[1])}
    (out / 'metrics.json').write_text(json.dumps(metrics, indent=2) + '\n')
    print('PASS: three native compiler generations, identical stage 2/3 objects,')
    print('      integer/float regressions, ELF reload, diagnostics and exit recovery.')
    print('Artifacts:', out)


if __name__ == '__main__':
    main()
