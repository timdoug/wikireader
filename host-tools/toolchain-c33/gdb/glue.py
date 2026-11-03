#!/usr/bin/env python3
"""Glue the C33 port into a pristine GDB source tree.

    glue.py TREE

GDB carries its own copy of BFD, opcodes and include: the C33's files and
edits for those come from ../binutils, as binutils' build applies them
(its glue.py skips the gas, ld and binutils files GDB's tree lacks).  The
GDB target itself is files/gdb, and the edits below add it to GDB's
configuration.
"""
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(sys.argv[1])
HERE = pathlib.Path(__file__).resolve().parent
BINUTILS = HERE.parent / "binutils"

for part in ("bfd", "opcodes", "include"):
    shutil.copytree(BINUTILS / "files" / part, ROOT / part, dirs_exist_ok=True)
subprocess.run([sys.executable, str(HERE.parent / "tools/glue.py"), str(ROOT)],
               check=True, stdout=subprocess.DEVNULL)


def edit(relpath, old, new, guard):
    path = ROOT / relpath
    text = path.read_text()
    if guard in text:
        return
    if text.count(old) != 1:
        raise SystemExit(f"{relpath}: anchor not found once: {old!r}")
    path.write_text(text.replace(old, new))


edit("gdb/configure.tgt", "cris*)\n", """c33-*-linux*)
	# Target: Epson S1C33, no-MMU FDPIC GNU/Linux
	gdb_target_obs="c33-tdep.o c33-linux-tdep.o linux-tdep.o \\
			solib-c33.o"
	;;

c33-*-*)
	# Target: Epson S1C33
	gdb_target_obs="c33-tdep.o"
	;;

cris*)
""", "c33-tdep.o")
edit("gdb/Makefile.in", "\tcris-linux-tdep.o \\\n",
     "\tc33-linux-tdep.o \\\n\tc33-tdep.o \\\n\tcris-linux-tdep.o \\\n",
     "c33-tdep.o")
edit("gdb/Makefile.in", "\tsolib-dsbt.o \\\n",
     "\tsolib-c33.o \\\n\tsolib-dsbt.o \\\n", "solib-c33.o")
edit("gdb/Makefile.in", "\tcris-tdep.h \\\n",
     "\tc33-tdep.h \\\n\tcris-tdep.h \\\n", "c33-tdep.h")
edit("gdb/Makefile.in", "\tsolib-frv.h \\\n",
     "\tsolib-c33.h \\\n\tsolib-frv.h \\\n", "solib-c33.h")

# gdbserver for C33 Linux.
edit("gdbserver/configure.srv", "  csky*-*linux*)", """  c33*-*-linux*)	srv_regobj="c33-linux.o"
			srv_tgtobj="$srv_linux_obj linux-c33-low.o"
			srv_linux_regsets=yes
			;;
  csky*-*linux*)""", "linux-c33-low.o")

# GDB's no-MMU path (the ptrace feature probe's CLONE_VM child) assumes
# uClibc; a no-MMU kernel's compiler defines __uClinux__ whatever the C
# library, and musl's fork() fails without an MMU.  fork_inferior already
# uses vfork.
edit("gdb/nat/linux-ptrace.h", """#ifdef __UCLIBC__
#if !(defined(__UCLIBC_HAS_MMU__) || defined(__ARCH_HAS_MMU__))
/* PTRACE_TEXT_ADDR and friends.  */
#include <asm/ptrace.h>
#define HAS_NOMMU
#endif
#endif
""", """#ifdef __UCLIBC__
#if !(defined(__UCLIBC_HAS_MMU__) || defined(__ARCH_HAS_MMU__))
/* PTRACE_TEXT_ADDR and friends.  */
#include <asm/ptrace.h>
#define HAS_NOMMU
#endif
#elif defined(__uClinux__)
#define HAS_NOMMU
#endif
""", "#elif defined(__uClinux__)")
edit("gdb/nat/linux-ptrace.c",
     "#if defined(__UCLIBC__) && defined(HAS_NOMMU)\n#define STACK_SIZE",
     "#if defined(HAS_NOMMU)\n#define STACK_SIZE", "#if defined(HAS_NOMMU)\n#define STACK_SIZE")
