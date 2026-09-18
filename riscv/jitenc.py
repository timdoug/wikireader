#!/usr/bin/env python3
"""Check the translator's instruction encoder against the real assembler.

host_enc prints, for every C33 instruction rv32_jit.c can emit, the bytes it
emits and the assembly meaning the same thing.  This assembles the second
column and diffs it against the first.

It is the only check on the emitter that does not involve booting a guest.
A wrong field is not a crash, it is a different instruction: the first version
of the shift encoding was right for every count below sixteen, and what that
cost was six full-system Linux boots to find.
"""
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BIN = HERE.parent / 'host-tools/toolchain-c33/work/install/bin'


def main():
    cases = []
    for line in subprocess.run([str(HERE / 'build/host_enc')],
                               capture_output=True, text=True,
                               check=True).stdout.splitlines():
        got, text = line.split('\t', 1)
        cases.append((got, text))

    src = '\t.text\n\t.align 1\n' + ''.join(f'\t{text}\n' for _, text in cases)
    (HERE / 'build/jitenc.s').write_text(src)
    subprocess.run([str(BIN / 'c33-epson-elf-as'), '-mc33pe',
                    '-o', str(HERE / 'build/jitenc.o'),
                    str(HERE / 'build/jitenc.s')], check=True)
    dump = subprocess.run([str(BIN / 'c33-epson-elf-objdump'), '-d',
                           str(HERE / 'build/jitenc.o')],
                          capture_output=True, text=True, check=True).stdout

    # objdump prints one line per instruction including each ext prefix, so
    # the words are gathered back up by matching the count this emitted.
    words = []
    for line in dump.splitlines():
        m = re.match(r'\s*[0-9a-f]+:\s+((?:[0-9a-f]{2} )+)\s', line)
        if m:
            b = m[1].split()
            for i in range(0, len(b), 2):
                words.append(b[i + 1] + b[i])

    bad = at = 0
    for got, text in cases:
        n = len(got) // 4
        want = ''.join(words[at:at + n])
        at += n
        if got != want:
            bad += 1
            if bad <= 20:
                print(f'{text:32} emitted {got}, assembles to {want}')
    if at != len(words):
        print(f'length mismatch: {at} words used of {len(words)}',
              file=sys.stderr)
        return 1
    print(f'{len(cases)} encodings checked, {bad} wrong')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
