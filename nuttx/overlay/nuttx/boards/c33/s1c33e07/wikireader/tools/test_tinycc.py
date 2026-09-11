#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile C on the emulated WikiReader, execute it, and reload ELF objects."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from test_terminal import read_pgm, write_png


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wikireader', type=Path, default=Path.home() / 'wikireader')
    parser.add_argument('--image', type=Path, default=root / 'nuttx')
    parser.add_argument('--tinycc', type=Path, default=root / 'tinycc')
    parser.add_argument('--out', type=Path, default=root / 'build/wikireader/tinycc-test')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    image = out / 'kernel.elf'
    if args.image.resolve() != image:
        shutil.copy2(args.image, image)
    source = args.tinycc / 'tests/c33/regression.c'
    subprocess.run(['cc', '-O0', str(source), '-o', str(out / 'oracle')], check=True)
    expected = subprocess.check_output([out / 'oracle'], text=True).strip()
    # These echo commands write the actual C file inside NuttX tmpfs.
    commands = ['tcc -h']
    for i, line in enumerate(source.read_text().splitlines()):
        if "'" in line:
            raise SystemExit('Fixture contains a single quote; update NSH quoting')
        command = f"echo '{line}' {'>' if i == 0 else '>>'} /tmp/check.c"
        if len(command) >= 255:
            raise SystemExit('Fixture exceeds NSH line length')
        commands.append(command)
    commands += [
        'tcc -run /tmp/check.c', 'echo SOURCE_STATUS=$?',
        'tcc -c /tmp/check.c -o /tmp/check.o', 'echo OBJECT_STATUS=$?',
        'tcc -run /tmp/check.o', 'echo RELOAD_STATUS=$?',
        "tcc -e 'int main(void) { return nope; }'", 'echo BAD_STATUS=$?',
        "tcc -e 'int main(void) { volatile double x=1.5; return x*x!=2.25; }'", 'echo FLOAT_STATUS=$?',
        "tcc -e 'int main(void) { asm(\"nop\"); return 0; }'", 'echo ASM_STATUS=$?',
        "tcc -e 'int absent(void); int main(void) { return absent(); }'", 'echo LINK_STATUS=$?',
        "tcc -e 'int main(void) { return 37; }'", 'echo RETURN_STATUS=$?',
        "tcc -e 'void exit(int); int main(void) { exit(23); return 0; }'", 'echo EXIT_STATUS=$?',
        'tcc -run /tmp/hello.c -arg', 'echo RECOVERY_STATUS=$?',
    ]
    commands += ['tcc -run /tmp/hello.c', 'echo REPEAT_STATUS=$?'] * 5
    commands += ['echo TINYCC_TEST_DONE']
    (out / 'input.txt').write_text('\n'.join(commands) + '\n')
    def emulate(name, budget):
        folder = out / name
        folder.mkdir(exist_ok=True)
        command = [str(args.wikireader / 'emulator/wremu'), '-n', str(budget),
                   '--uart-input', str(folder / 'input.txt'), '--uart-start', '20000000',
                   '--uart-gap', '60000', str(image)]
        (folder / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
        with (folder / 'emulator.log').open('w') as log:
            subprocess.run(command, cwd=folder, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=45)
        return (folder / 'emulator.log').read_text().replace('\r', '')

    cmd = [str(args.wikireader / 'emulator/wremu'), '-n', '3000000000',
           '--uart-input', str(out / 'input.txt'), '--uart-start', '20000000',
           '--uart-gap', '60000', str(image)]
    (out / 'command.json').write_text(json.dumps(cmd, indent=2) + '\n')
    with (out / 'emulator.log').open('w') as log:
        subprocess.run(cmd, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=90)
    text = (out / 'emulator.log').read_text().replace('\r', '')
    for marker in (expected, 'TINYCC_TEST_DONE', 'Hello from native C33 C! argc=2',
                   'Hello from native C33 C! argc=1'):
        if '\n' + marker + '\n' not in text:
            raise SystemExit(f'Missing {marker!r}: {out / "emulator.log"}')
    if text.count('\n' + expected + '\n') != 2:
        raise SystemExit('Source and reloaded object did not both pass')
    for tag in ('SOURCE', 'OBJECT', 'RELOAD', 'RECOVERY', 'REPEAT', 'FLOAT'):
        if f'\n{tag}_STATUS=0\n' not in text or f'\n{tag}_STATUS=1\n' in text:
            raise SystemExit(f'{tag} failed')
    # NSH normalizes nonzero builtin exits to shell status 1.
    for tag in ('BAD', 'ASM', 'LINK', 'RETURN', 'EXIT'):
        if f'\n{tag}_STATUS=1\n' not in text:
            raise SystemExit(f'{tag} did not report failure')
    for bad in ('C33: fatal', 'compiler is already running', 'unmapped read',
                'unmapped write', 'runaway:', 'Assertion failed', 'C33_REGRESSION_FAIL'):
        if bad in text:
            raise SystemExit(f'Unexpected {bad!r}')
    # Isolate Ctrl-C from queued input behind earlier compilations. Otherwise
    # a preloaded UART stream can send it to an earlier foreground command.
    folder = out / 'interrupt'
    folder.mkdir(exist_ok=True)
    (folder / 'input.txt').write_text(
        "tcc -e 'int main(void) { for (;;) {} }'\n" + ' ' * 128 + '\x03\n' +
        'tcc -run /tmp/hello.c after interrupt\necho INTERRUPT_STATUS=$?\n')
    interrupted = emulate('interrupt', 200000000)
    if ('\nINTERRUPT_STATUS=0\n' not in interrupted or
            '\nHello from native C33 C! argc=3\n' not in interrupted):
        raise SystemExit('Ctrl-C recovery failed')
    write_png(out / 'screen.png', read_pgm(out / 'screen.pgm'))
    metrics = {'elf_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
               'reference': expected,
               'executed_instructions': int(re.search(r'--- work: (\d+) instructions', text)[1]),
               'source_and_object_passed': True, 'ctrl_c_recovery_passed': True}
    (out / 'metrics.json').write_text(json.dumps(metrics, indent=2) + '\n')
    print('PASS: native C compilation, integer regression, ELF reload, diagnostics,')
    print('      exit status, Ctrl-C recovery, and repeated compiler invocations.')
    print('Artifacts:', out)


if __name__ == '__main__':
    main()
