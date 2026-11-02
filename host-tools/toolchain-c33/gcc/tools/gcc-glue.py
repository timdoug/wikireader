#!/usr/bin/env python3
"""Register the Seiko Epson C33 target in a modern GCC tree.  Idempotent.

Everything C33-specific lives in gcc/files/; this script does the handful of
edits to *shared* files that a new target needs, so the tree stays a pristine
upstream release plus a drop-in.
"""
import re, sys, pathlib

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '.')
changed = []

# The TLS slot is part of the Linux ABI, defined in the port
# (c33-linux-tls.h) and in the kernel's <asm/tls.h>.  Where this tree also
# holds the kernel, check that they agree.
tls_uapi = (pathlib.Path(__file__).resolve().parents[4]
            / 'linux/overlay/arch/c33/include/uapi/asm/tls.h')
tls_port = (pathlib.Path(__file__).resolve().parents[1]
            / 'files/gcc/config/c33/c33-linux-tls.h')
if tls_uapi.exists():
    def tls_slot(path):
        m = re.search(r'#define\s+C33_TLS_SLOT_ADDRESS\s+(\S+)', path.read_text())
        return int(m.group(1), 0) if m else None
    if tls_slot(tls_uapi) != tls_slot(tls_port):
        raise SystemExit(f'C33_TLS_SLOT_ADDRESS differs: {tls_uapi} and {tls_port}')

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

# ------------------------------------------------------------------- gcc/doc

def put_between(before, after, block, marker):
    """Make BLOCK the text between BEFORE and the AFTER that follows it.

    Upstream has nothing there, so the first run inserts BLOCK; a later run
    replaces an older version of it, recognised by MARKER, so that editing
    BLOCK here updates a glued tree.  Any other text there, or a missing
    anchor, stops the script rather than guess."""
    def fn(s):
        i = s.find(before)
        if i < 0:
            raise SystemExit(f'gcc-glue: anchor not found: {before!r}')
        i += len(before)
        j = s.find(after, i)
        if j < 0:
            raise SystemExit(f'gcc-glue: anchor not found: {after!r}')
        current = s[i:j]
        if current == block:
            return None
        if current and marker not in current:
            raise SystemExit(f'gcc-glue: unexpected text before {after!r}')
        return s[:i] + block + s[j:]
    return fn


# invoke.texi: the option summary, the menu and the section, each placed
# between Blackfin and C6X as the alphanumeric order requires.
edit('gcc/doc/invoke.texi', put_between(
    '-micplb}\n\n', '@emph{C6X Options} (@ref{C6X Options})',
    '''@emph{C33 Options} (@ref{C33 Options})
@gccoptlist{-mcore=@var{core}  -mc33  -mc33adv  -mc33pe
-mlong-calls  -medda32  -msep-data  -mfdpic
-memcpy  -mbig-switch  -mjump-tables-in-data-section
-msda=@var{n}}

''', '@ref{C33 Options}'))

edit('gcc/doc/invoke.texi', put_between(
    '* Blackfin Options::\n', '* C6X Options::\n',
    '* C33 Options::\n', 'C33 Options'))

C33_OPTIONS = '''@node C33 Options
@subsection C33 Options
@cindex C33 Options

These @samp{-m} options are defined for the Seiko Epson S1C33 (C33)
family of processors.

@table @gcctabopt
@opindex mcore
@item -mcore=@var{core}
Generate code for the C33 core @var{core}: @samp{c33} for the standard
core, @samp{c33adv} for the Advanced core, or @samp{c33pe} for the PE
core.  The default is @samp{c33}, except for
@samp{c33-*-linux-uclibc}, where it is @samp{c33pe}.

GCC passes the core to the assembler, which records it in the object
file; the linker does not link objects built for different cores.  For
the Advanced and PE cores GCC uses @code{swaph} for 16-bit byte swaps
and defines the preprocessor macro @code{__c33adv__} or
@code{__c33pe__}.  For the PE core GCC expands atomic exchange and
compare-and-swap operations inline, with interrupts disabled around the
memory access; for the other cores they are calls to the
@code{__atomic} library functions.

@opindex mc33
@opindex mc33adv
@opindex mc33pe
@item -mc33
@itemx -mc33adv
@itemx -mc33pe
These are the same as @option{-mcore=c33}, @option{-mcore=c33adv} and
@option{-mcore=c33pe}.

@opindex mlong-calls
@opindex mno-long-calls
@item -mlong-calls
@itemx -mno-long-calls
Make direct calls and sibling calls with @code{xcall} and @code{xjp},
whose two @code{ext} prefixes reach the whole 32-bit address space,
rather than with @code{scall} and @code{sjp}, whose one @code{ext}
prefix reaches 2 megabytes in either direction.  The default is
@option{-mno-long-calls}; the linker reports a call that is out of
range.  For @samp{c33-*-linux-uclibc} a call to another module goes
through the calling module's own @code{.plt}, so only a program or
shared library larger than 2 megabytes needs @option{-mlong-calls}.

@opindex medda32
@opindex mno-edda32
@item -medda32
@itemx -mno-edda32
@option{-medda32} reaches static and global variables at their absolute
addresses, which take two @code{ext} prefixes to load.
@option{-mno-edda32} reaches them through the default data area
instead: at an offset from @code{%r15} given by a pair of @code{ext}
prefixes (@code{doff_hi} and @code{doff_lo}), which a single load or
store can use.  The startup code must then set @code{%r15} to the
address of the symbol @code{__dp}.  The default is @option{-medda32},
and the preprocessor macro @code{__C33_EDDA32__} is defined when it is
in effect.

@option{-msep-data} and @option{-mfdpic} turn @option{-medda32} off;
giving @option{-medda32} together with either of them is an error.  For
@samp{c33-*-linux-uclibc}, @option{-medda32} also turns off the default
@option{-mfdpic} and @option{-msep-data}.

@opindex msep-data
@opindex mno-sep-data
@item -msep-data
@itemx -mno-sep-data
Generate code whose text contains no absolute addresses, so that one
copy of it can be shared by processes that each have their own data
segment, on a system without an MMU@.  @code{%r15} holds the address of
the data segment, the symbol @code{__dp}.  Writable variables in data
and bss sections are reached at offsets from @code{%r15}.  Every other
address, of a function, a string literal, read-only data or a label, is
loaded from the constant pool, which this option places in the data
segment.  Read-only data that holds addresses goes in
@code{.data.rel.ro}, and switch tables hold offsets rather than
addresses, so they stay in the text.

The preprocessor macro @code{__C33_SEP_DATA__} is defined.  The default
is @option{-mno-sep-data}, except for @samp{c33-*-linux-uclibc}.

@opindex mfdpic
@opindex mno-fdpic
@item -mfdpic
@itemx -mno-fdpic
Generate code for the FDPIC ABI, which no-MMU Linux uses for programs
and shared libraries.  FDPIC code is @option{-msep-data} code in which
each module, a program or a shared library, has its own data segment
and its own value of @code{%r15}.  A function pointer is the address of
a function descriptor, which holds the function's entry point and the
@code{%r15} of the module that defines it.  Data that another module
may define is reached through a word in the constant pool that the
dynamic linker fills in, and a call that may enter another module
restores the caller's @code{%r15} afterwards.

@option{-mfdpic} implies @option{-msep-data}, and defines the
preprocessor macros @code{__FDPIC__} and @code{__C33_FDPIC__}.  It is
the default for @samp{c33-*-linux-uclibc}, whose startup files and
libraries are FDPIC: code compiled with @option{-mno-fdpic} or
@option{-medda32} there is for linking by other means, such as a kernel
or a boot loader, not with the C library.

@opindex memcpy
@item -memcpy
This option is accepted for compatibility with older C33 compilers.  It
has no effect.

@opindex mbig-switch
@item -mbig-switch
This option is accepted for compatibility with older C33 compilers.  It
does not change the object code: switch tables always have 4-byte
entries, aligned to 4 bytes.

@opindex mjump-tables-in-data-section
@opindex mno-jump-tables-in-data-section
@item -mjump-tables-in-data-section
@itemx -mno-jump-tables-in-data-section
Put the tables for switch statements in the read-only data section
rather than in the text section.  The default is
@option{-mno-jump-tables-in-data-section}.

@opindex msda
@item -msda=@var{n}
Put static and global variables of @var{n} bytes or less in the
@code{.sdata} and @code{.sbss} sections, or @code{.rosdata} for
constants.  @var{n} can be at most 65536, and the default is 0.  This
changes where the variables are placed, not how they are addressed.
@end table

'''

edit('gcc/doc/invoke.texi', put_between(
    'are enabled; for standalone applications the default is off.\n'
    '@end table\n\n',
    '@node C6X Options\n', C33_OPTIONS, '@node C33 Options'))

# md.texi: Machine Constraints, between Blackfin and C-SKY.
edit('gcc/doc/md.texi', put_between(
    '@item H\n@itemx Q\nAny SYMBOL_REF.\n@end table\n\n',
    '@item C-SKY---@file{config/csky/constraints.md}',
    '''@item C33---@file{config/c33/constraints.md}
@table @code
@item c
A callee-saved register, @code{%r0} to @code{%r3}.

@item f
The stack pointer, @code{%sp}.

@item Z
Register @code{%r14}, which holds the target of an indirect sibling
call.

@item I
Integer constant 0.

@item J
A signed 5-bit integer constant, @minus{}16 to 15.

@item K
A signed 16-bit integer constant.

@item L
A signed 32-bit integer constant whose low 16 bits are zero.

@item M
An unsigned 16-bit integer constant.

@item N
An integer constant from 0 to 31, a shift count.

@item O
An integer constant from @minus{}255 to 255.

@item G
A floating-point zero.

@item Q
A memory operand that one two-byte instruction can reach:
@code{[%r@var{n}]} or @code{[%r@var{n}]+}.

@item Y
A memory operand that the bit instructions can reach: a general
register plus an unsigned 26-bit displacement, or an absolute address.
@end table

@ifset INTERNALS
The C33 back end also defines these constraints, which are not intended
for inline assembler.  Only @code{S} is used by an instruction pattern.

@table @code
@item S
A symbol other than one in the zero data area: the target of a direct
call.

@item b
A general register or @code{%sp}, the registers usable as a memory
base.

@item e
A general register; the same as @code{r}.

@item R
A symbol in the small, tiny or zero data area, optionally plus a signed
16-bit offset.

@item U
A symbol in the zero data area, optionally plus an offset.

@item W
An integer constant from 0x8000 to 0x3fffff.

@item H
@itemx P
@itemx T
These never match.
@end table
@end ifset

''', 'config/c33/constraints.md'))

# install.texi: host/target specific notes, after Blackfin.
edit('gcc/doc/install.texi', put_between(
    '@uref{#bfin,,Blackfin}\n', '@item\n@uref{#cris,,cris}',
    '@item\n@uref{#c33-x-elf,,c33-*-elf}\n'
    '@item\n@uref{#c33-x-linux-uclibc,,c33-*-linux-uclibc}\n',
    '#c33-x-elf'))

edit('gcc/doc/install.texi', put_between(
    'are available at @uref{https://sourceforge.net/projects/adi-toolchain/}.\n\n',
    '@html\n<hr />\n@end html\n@anchor{cris}',
    '''@html
<hr />
@end html
@anchor{c33-x-elf}
@heading c33-*-elf
Seiko Epson S1C33 (C33) processors, without an operating system.  The
standard, Advanced and PE cores are supported.
@ifnothtml
@xref{C33 Options,, C33 Options, gcc, Using the GNU Compiler
Collection (GCC)},
@end ifnothtml
@ifhtml
See ``C33 Options'' in the main manual
@end ifhtml
for a list of C33-specific options.

@html
<hr />
@end html
@anchor{c33-x-linux-uclibc}
@heading c33-*-linux-uclibc
No-MMU Linux on the C33 PE core, with uClibc-ng as the C library.
Programs and shared libraries are FDPIC ELF: the compiler defaults to
@option{-mcore=c33pe} and @option{-mfdpic}.

''', '@anchor{c33-x-elf}'))

print('changed:', changed or '(already applied)')
