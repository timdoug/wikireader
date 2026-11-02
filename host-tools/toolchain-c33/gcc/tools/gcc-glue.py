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

# ------------------------------------------------------------ gcc testsuite

# The PE core has no divide instructions; libgcc divides, and a zero divisor
# does not trap.  The test already says so for csky, or1k and the like.
def no_division_trap(s):
    if '__c33__' in s:
        return None
    anchor = '#elif defined (__pru__)\n'
    return s.replace(anchor,
                     '#elif defined (__c33__)\n'
                     '  /* C33 PE divides in libgcc, which does not trap.  */\n'
                     '# define DO_TEST 0\n' + anchor, 1)


edit('gcc/testsuite/gcc.c-torture/execute/20101011-1.c', no_division_trap)

# GCC's probes for exception-table garbage collection link an absolute
# address in .text, which an FDPIC link refuses; the linker collects the
# tables correctly.  Answered as upstream does for hppa-linux, so that a
# COMDAT function's table goes in its group.
def eh_gc_sections(s):
    if 'c33-*-*linux*)\n    # FDPIC' in s:
        return None
    s = s.replace(
        '  hppa*-*-linux*)\n    # ??? This apparently exposes a binutils bug',
        '  c33-*-*linux*)\n    # FDPIC refuses the probe; the linker is right.\n'
        '    gcc_cv_ld_eh_gc_sections=yes\n    ;;\n'
        '  hppa*-*-linux*)\n    # ??? This apparently exposes a binutils bug', 1)
    return s.replace(
        'if test x$gcc_cv_ld_eh_gc_sections_bug = xyes; then',
        'case "$target" in\n  c33-*-*linux*)\n    # FDPIC refuses the probe; the linker is right.\n'
        '    gcc_cv_ld_eh_gc_sections_bug=no\n    ;;\nesac\n'
        'if test x$gcc_cv_ld_eh_gc_sections_bug = xyes; then', 1)


edit('gcc/configure', eh_gc_sections)
edit('gcc/configure.ac', eh_gc_sections)

# The rest give C33 the per-target expectations upstream gives other ports.
DG = 'gcc/testsuite/gcc.dg/'


def add_after_first(line_start, new):
    """Insert NEW after the first line starting with LINE_START."""
    def fn(s):
        if new in s:
            return None
        i = s.index(line_start)
        eol = s.index('\n', i) + 1
        return s[:eol] + new + s[eol:]
    return fn


def replace_once(old, new):
    return lambda s: None if new in s else s.replace(old, new, 1)


# No MMU: a store through a null pointer does not fault.
for t in ('cleanup-8', 'cleanup-9', 'cleanup-10', 'cleanup-11'):
    edit(DG + t + '.c', add_after_first('/* { dg-do run',
         '/* { dg-skip-if "no MMU: a null store does not fault" { c33-*-*linux* } } */\n'))
# No MMU: mprotect cannot make the guard page.
edit(DG + 'strncmp-2.c', add_after_first('/* { dg-do run',
     '/* { dg-skip-if "no MMU: no guard page" { c33-*-*linux* } } */\n'))
# uClibc-ng has no _FORTIFY_SOURCE functions.
edit(DG + 'strlenopt-2f.c', add_after_first('/* { dg-do run',
     '/* { dg-skip-if "uClibc-ng has no __memcpy_chk" { c33-*-*linux* } } */\n'))
# uClibc-ng's libm sets errno only when built with UCLIBC_HAS_FENV.
for t in ('errno-2', 'errno-3', 'errno-4'):
    edit(DG + t + '.c', add_after_first('/* { dg-do run',
         '/* { dg-skip-if "uClibc-ng libm sets errno only with fenv" { c33-*-*linux* } } */\n'))
# FDPIC code is always PIC, as ARM's.
edit(DG + '20020312-2.c', replace_once(
    '{ arm*-*-uclinuxfdpiceabi }', '{ arm*-*-uclinuxfdpiceabi c33-*-*linux* }'))
# No conditional moves.
edit(DG + 'ifcvt-4.c', replace_once('pru-*-* loongarch32-*-*', 'pru-*-* loongarch32-*-* c33-*-*'))
# The frame adds the 16-byte %r0-%r3 save block.
edit(DG + 'stack-usage-1.c', replace_once(
    '#elif defined (__loongarch__)\n',
    '#elif defined (__c33__)\n#  define SIZE 240 /* 256 - 16 bytes for %r0-%r3 */\n'
    '#elif defined (__loongarch__)\n'))
# The widening multiply is dumped as " w* ".
edit(DG + 'pr87954.c', replace_once(r'{ \*w? |WIDEN_MULT_PLUS_EXPR}',
                                    r'{ \*w? | w\* |WIDEN_MULT_PLUS_EXPR}'))
# A binary64 long double overflows at 1e4000L, as it should.
edit(DG + 'pr126464.c', add_after_first('/* { dg-options',
     '/* { dg-additional-options "-Wno-overflow" { target { ! large_long_double } } } */\n'))

GXX = 'gcc/testsuite/g++.dg/'
for t in ('cleanup-8', 'cleanup-9', 'cleanup-10', 'cleanup-11'):
    edit(GXX + 'ext/' + t + '.C', add_after_first('/* { dg-do run',
         '/* { dg-skip-if "no MMU: a null store does not fault" { c33-*-*linux* } } */\n'))
# Four and five megabytes in one piece, on the stack or in .bss: more than
# a no-MMU process of 32 MB is given.
for t in ('array15', 'array16'):
    edit(GXX + 'init/' + t + '.C', add_after_first('// { dg-do run',
         '// { dg-skip-if "no MMU: 4-5 MB in one piece" { c33-*-*linux* } }\n'))
# uClibc-ng does not predefine __STDC_ISO_10646__, as glibc does.
def iso_10646(s):
    if 'c33-*-*linux*' in s:
        return None
    return ''.join(
        l.replace('{ xfail { ! *-*-linux* } }',
                  '{ xfail { { ! *-*-linux* } || c33-*-*linux* } }')
        if '__STDC_ISO_10646__' in l else l
        for l in s.splitlines(keepends=True))


for t in ('dr2581-1', 'dr2581-2'):
    edit(GXX + 'DRs/' + t + '.C', iso_10646)
# -msep-data keeps the constant pool in .data.
edit(GXX + 'cpp0x/initlist-const1.C', replace_once(
    'xfail hppa*-*-hpux* powerpc-ibm-aix* }', 'xfail hppa*-*-hpux* powerpc-ibm-aix* c33-*-*linux* }'))

print('changed:', changed or '(already applied)')
