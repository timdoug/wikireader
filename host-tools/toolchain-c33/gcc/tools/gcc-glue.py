#!/usr/bin/env python3
"""Register the Seiko Epson C33 target in a modern GCC tree.  Idempotent.

Everything C33-specific lives in gcc/files/; this script does the handful of
edits to *shared* files that a new target needs, so the tree stays a pristine
upstream release plus a drop-in.
"""
import sys, pathlib

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '.')
changed = []

# Use the kernel's target ABI definition, never the build host's asm headers.
# Both compiler build entry points run this script before invoking make.
tls_uapi = pathlib.Path(__file__).resolve().parents[4] / 'linux/overlay/arch/c33/include/uapi/asm/tls.h'
tls_target = ROOT / 'gcc/config/c33/c33-linux-tls.h'
tls_text = tls_uapi.read_text()
if not tls_target.exists() or tls_target.read_text() != tls_text:
    tls_target.write_text(tls_text)
    changed.append('gcc/config/c33/c33-linux-tls.h')

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

def linux_block(s):
    """No-MMU Linux with uClibc-ng: c33-linux-uclibc.

    The generic *-*-linux* section has already added linux.o, glibc-c.o and
    the t-linux/t-glibc fragments; this case has to come before c33-*-*,
    which would otherwise match first.
    """
    if 'c33/linux.h' in s:
        return None
    return s.replace('c33-*-*)\n\ttm_file="elfos.h newlib-stdint.h c33/c33.h"\n',
        'c33-*-linux*)\n'
        '\ttm_file="elfos.h c33/c33.h gnu-user.h linux.h glibc-stdint.h c33/linux.h"\n'
        '\ttmake_file="${tmake_file} c33/t-linux"\n'
        '\tc_target_objs="${c_target_objs} c33-c.o"\n'
        '\tcxx_target_objs="${cxx_target_objs} c33-c.o"\n'
        '\t;;\n'
        'c33-*-*)\n\ttm_file="elfos.h newlib-stdint.h c33/c33.h"\n', 1)

edit('gcc/config.gcc', cpu_type)
edit('gcc/config.gcc', target_block)
edit('gcc/config.gcc', linux_block)

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

def libgcc_linux(s):
    """No-MMU Linux with uClibc-ng, FDPIC.  crtbegin.o and crtend.o
    use program-header unwind discovery (see c33/linux.h); crtbeginT.o keeps
    explicit registration for static links. Shared CRT supplies __dso_handle.

    Floating point is soft-fp (c33/sfp-machine.h) rather than fp-bit:
    fp-bit unpacks every operand into a structure and packs the result,
    and a double add or multiply cost several times what soft-fp's word
    arithmetic does.  The bare-metal target keeps fp-bit for now."""
    block = ('c33-*-linux*)\n\ttmake_file="${tmake_file} c33/t-c33 t-softfp-sfdf t-softfp t-crtstuff-pic"\n'
             '\textra_parts="crtbegin.o crtend.o crtbeginS.o crtendS.o crtbeginT.o"\n'
             '\tmd_unwind_header=c33/linux-unwind.h\n\t;;\n')
    if block in s:
        return None
    for old in ('c33-*-linux*)\n\ttmake_file="${tmake_file} c33/t-c33 t-softfp-sfdf t-softfp t-crtstuff-pic"\n'
                '\textra_parts="crtbegin.o crtend.o crtbeginS.o crtendS.o"\n'
                '\tmd_unwind_header=c33/linux-unwind.h\n\t;;\n',
                'c33-*-linux*)\n\ttmake_file="${tmake_file} c33/t-c33 t-softfp-sfdf t-softfp t-crtstuff-pic"\n'
                '\textra_parts="crtbegin.o crtend.o crtbeginS.o crtendS.o"\n\t;;\n',
                'c33-*-linux*)\n\ttmake_file="${tmake_file} c33/t-c33 t-softfp-sfdf t-softfp"\n'
                '\textra_parts="crtbegin.o crtend.o"\n\t;;\n',
                'c33-*-linux*)\n\ttmake_file="${tmake_file} c33/t-c33 t-fdpbit"\n'
                '\textra_parts="crtbegin.o crtend.o"\n\t;;\n',
                'c33-*-linux*)\n\ttmake_file="${tmake_file} c33/t-c33 t-fdpbit"\n'
                '\textra_parts=""\n\t;;\n'):
        if old in s:
            return s.replace(old, block, 1)
    return s.replace('c33-*-*)\n\ttmake_file="${tmake_file} c33/t-c33 t-fdpbit"\n\t;;\n',
                     block +
                     'c33-*-*)\n\ttmake_file="${tmake_file} c33/t-c33 t-fdpbit"\n\t;;\n', 1)

edit('libgcc/config.host', libgcc_cpu_type)
edit('libgcc/config.host', libgcc_target)
edit('libgcc/config.host', libgcc_linux)

print('changed:', changed or '(already applied)')
