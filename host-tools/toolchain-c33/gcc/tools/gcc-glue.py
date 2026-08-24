#!/usr/bin/env python3
"""Register the Seiko Epson C33 target in a modern GCC tree.  Idempotent."""
import sys, pathlib

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '.')
changed = []

def edit(rel, fn):
    p = ROOT / rel
    src = p.read_text(encoding='latin-1')
    out = fn(src)
    if out and out != src:
        p.write_text(out, encoding='latin-1')
        changed.append(rel)

# cpu_type mapping
def cpu_type(s):
    if 'c33*-*-*)\n\tcpu_type=c33' in s:
        return None
    return s.replace('v850*-*-*)\n\tcpu_type=v850\n\t;;\n',
                     'c33*-*-*)\n\tcpu_type=c33\n\t;;\n'
                     'v850*-*-*)\n\tcpu_type=v850\n\t;;\n', 1)

# target configuration
def target_block(s):
    if 'c33/c33.h' in s:
        return None
    return s.replace('v850*-*-*)\n\tcase ${target} in\n',
        'c33-*-*)\n'
        '\ttm_file="elfos.h newlib-stdint.h c33/c33.h"\n'
        '\tuse_collect2=no\n'
        '\tuse_gcc_stdint=wrap\n'
        '\t;;\n'
        'v850*-*-*)\n\tcase ${target} in\n', 1)

edit('gcc/config.gcc', cpu_type)
edit('gcc/config.gcc', target_block)

print('changed:', changed or '(already applied)')
