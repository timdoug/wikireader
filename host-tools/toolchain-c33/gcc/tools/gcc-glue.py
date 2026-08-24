#!/usr/bin/env python3
"""Register the Seiko Epson C33 target in a modern GCC tree.  Idempotent.

Everything C33-specific lives in gcc/files/; this script does the handful of
edits to *shared* files that a new target needs, so the tree stays a pristine
upstream release plus a drop-in.
"""
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

# ---------------------------------------------------------------- gcc/config.gcc

def cpu_type(s):
    """Map the target triple to the config/c33 directory."""
    if 'c33*-*-*)\n\tcpu_type=c33' in s:
        return None
    return s.replace('v850*-*-*)\n\tcpu_type=v850\n\t;;\n',
                     'c33*-*-*)\n\tcpu_type=c33\n\t;;\n'
                     'v850*-*-*)\n\tcpu_type=v850\n\t;;\n', 1)

def target_block(s):
    """The target's own configuration.

    c33-c.o carries the pragma handling; it has to be named in both
    c_target_objs and cxx_target_objs or the link fails with undefined
    references from c33.cc.  t-c33 is the target fragment.
    """
    if 'c33/c33.h' in s:
        return None
    return s.replace('v850*-*-*)\n\tcase ${target} in\n',
        'c33-*-*)\n'
        '\ttm_file="elfos.h newlib-stdint.h c33/c33.h"\n'
        '\ttmake_file="${tmake_file} c33/t-c33"\n'
        '\tuse_collect2=no\n'
        '\tc_target_objs="c33-c.o"\n'
        '\tcxx_target_objs="c33-c.o"\n'
        '\tuse_gcc_stdint=wrap\n'
        '\t;;\n'
        'v850*-*-*)\n\tcase ${target} in\n', 1)

edit('gcc/config.gcc', cpu_type)
edit('gcc/config.gcc', target_block)

# ------------------------------------------------------------- libgcc/config.host

def libgcc_cpu_type(s):
    if 'c33*-*-*)\n\tcpu_type=c33' in s:
        return None
    return s.replace('v850*-*-*)\n\tcpu_type=v850\n\t;;\n',
                     'c33*-*-*)\n\tcpu_type=c33\n\t;;\n'
                     'v850*-*-*)\n\tcpu_type=v850\n\t;;\n', 1)

def libgcc_target(s):
    """libgcc for the C33 is entirely the generic C code.

    No LIB1ASMFUNCS: the V850 needed hand-written assembly for its
    out-of-line register save/restore helpers and callt, neither of which
    this target has.

    c33/t-c33 adds the single-word integer division routines, which libgcc2.c
    does not supply.  t-fdpbit brings in the soft-float routines -- __addsf3,
    __adddf3 and the rest -- which this target needs because it has no FPU.
    """
    if 'c33-*-*)\n\ttmake_file' in s:
        return None
    return s.replace('v850*-*-*)\n\ttmake_file="${tmake_file} v850/t-v850 t-fdpbit"\n\t;;\n',
                     'c33-*-*)\n\ttmake_file="${tmake_file} c33/t-c33 t-fdpbit"\n\t;;\n'
                     'v850*-*-*)\n\ttmake_file="${tmake_file} v850/t-v850 t-fdpbit"\n\t;;\n', 1)

edit('libgcc/config.host', libgcc_cpu_type)
edit('libgcc/config.host', libgcc_target)

print('changed:', changed or '(already applied)')
