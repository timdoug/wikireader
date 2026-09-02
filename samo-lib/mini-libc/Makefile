# Copyright (c) 2009 Openmoko Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.


CROSS=c33-epson-elf-
GCC=$(CROSS)gcc
AS=$(CROSS)gcc
LD=$(CROSS)ld
AR=$(CROSS)ar
HOSTCC=gcc
CC=$(GCC)
OBJCOPY=$(CROSS)objcopy
RM = rm -f
MKDIR = mkdir -p
TOUCH = touch


LDFLAGS=-static -s --no-gc-sections -N -L$(LIBDIR)
# See Mk/rules.mk: DWARF 2 rather than STABS, which gcc 16 no longer emits.
DEBUG_CFLAGS ?= -gdwarf-2
DEBUG_ASFLAGS ?= --gdwarf-2

# gcc 3.3 used gnu89 "extern inline" semantics: a definition in a header was
# inline-only.  gcc 5 and later default to C99, where the same text emits an
# external definition and collides with the real one in the .c file --
# ctype.h and the src/string/is*.c pair being the case here.  Ask for the old
# semantics when the compiler understands the option.
GNU89_INLINE := $(shell $(CC) -fgnu89-inline -E -x c /dev/null >/dev/null 2>&1 && echo -fgnu89-inline)

# Optimisation level, overridable so the toolchain work can A/B it.
#
# Keep -O2 after a current full-FLASH gcc 16 A/B.  -Os makes the installed
# kernel/init/wiki files 9.1% smaller and retires 5.2% fewer instructions in
# article retrieval, but the current SDRAM/bus model predicts 3.2% more time.
# Both variants repeat exactly and render identically.  See
# host-tools/toolchain-c33/HANDOFF.md for measurements and hardware caveats.
OPT ?= -O2

# scall reaches +/-4MB and the largest image here is 158kB, so -mlong-calls
# only bought two ext prefixes per direct call.  Dropping it is better on
# both toolchains and on every axis measured -- gcc 3.3.2: boot -2.6%,
# kernel -2.3%, wiki.app -2.2%; gcc 16 at -O2: boot -0.6%, wiki.app -1.7%.
# Rendering is byte-identical either way.
CFLAGS=-Wall -I. -Iinclude $(DEBUG_CFLAGS) $(GNU89_INLINE) -mno-long-calls -fno-builtin $(OPT) -mc33pe
ASFLAGS=-c -xassembler-with-cpp -Wa,$(DEBUG_ASFLAGS) -medda32 -mc33pe -mc33_ext

C_STRING_OPERATIONS = \
    src/string/bcmp.o \
    src/string/bcopy.o \
    src/string/bzero.o \
    src/string/ffs.o \
    src/string/index.o \
    src/string/isalnum.o \
    src/string/isalpha.o \
    src/string/isascii.o \
    src/string/isblank.o \
    src/string/iscntrl.o \
    src/string/isdigit.o \
    src/string/islower.o \
    src/string/isprint.o \
    src/string/ispunct.o \
    src/string/isspace.o \
    src/string/isupper.o \
    src/string/isxdigit.o \
    src/string/memccpy.o \
    src/string/memchr.o \
    src/string/memcmp.o \
    src/string/memcpy.o \
    src/string/memmove.o \
    src/string/memset.o \
    src/string/rindex.o \
    src/string/strcasecmp.o \
    src/string/strcat.o \
    src/string/strchr.o \
    src/string/strcmp.o \
    src/string/strcpy.o \
    src/string/strcspn.o \
    src/string/strdup.o \
    src/string/strlcat.o \
    src/string/strlcpy.o \
    src/string/strlen.o \
    src/string/strncasecmp.o \
    src/string/strncat.o \
    src/string/strncmp.o \
    src/string/strncpy.o \
    src/string/strpbrk.o \
    src/string/strrchr.o \
    src/string/strsep.o \
    src/string/strspn.o \
    src/string/strstr.o \
    src/string/strtok.o \
    src/string/swab.o \
    src/string/toascii.o \
    src/string/tolower.o \
    src/string/toupper.o

# qsort only: bsd/strtol and bsd/strtoul are older copies of the stdlib
# ones with their ldiv() path compiled in -- and ldiv is declared but
# defined nowhere, so whichever link picked those members failed.  The
# archive also carried both copies (and two atol.o) because of "ar q".
C_BSD_OPERATIONS = \
    src/bsd/qsort.o

C_STD_LIB_OPERATIONS = \
    src/stdlib/abort.o \
    src/stdlib/abs.o \
    src/stdlib/atoi.o \
    src/stdlib/atol.o \
    src/stdlib/bsearch.o \
    src/stdlib/errno.o \
    src/stdlib/itoa.o \
    src/stdlib/labs.o \
    src/stdlib/ltoa.o \
    src/stdlib/printf.o \
    src/stdlib/puts.o \
    src/stdlib/rand.o \
    src/stdlib/snprintf.o \
    src/stdlib/sprintf.o \
    src/stdlib/strtol.o \
    src/stdlib/strtoul.o \
    src/stdlib/ultoa.o \
    src/stdlib/uprintf.o \
    src/stdlib/utoa.o \
    src/stdlib/vprintf.o \
    src/stdlib/vsnprintf.o \
    src/stdlib/vsprintf.o \
    src/stdlib/vuprintf.o


TARGETS=lib/libc.a

.PHONY: all
all: $(TARGETS)

.c.o:
	$(GCC) -M $(CFLAGS) $< > $(<:.c=.d)
	$(GCC) $(CFLAGS) -c $< -o $(<:.c=.o)

# rebuild from scratch with a symbol index: "ar q $^" appended the
# stamp-lib marker file as an archive member and re-appended every
# object on each rebuild
lib/libc.a: stamp-lib $(C_STRING_OPERATIONS) $(C_BSD_OPERATIONS) $(C_STD_LIB_OPERATIONS)
	$(RM) lib/libc.a
	$(AR) rc lib/libc.a $(filter %.o,$^)

stamp-lib:
	${MKDIR} "${@:stamp-%=%}"
	${TOUCH} "${@}"

# just so that all the REQUIRED_xxx can be evaluated
.PHONY: requires
	true

.PHONY: clean
clean:
	${RM} $(TARGETS) *.o *.elf *.d
	${RM} src/string/*.d src/string/*.o
	${RM} src/stdlib/*.d src/stdlib/*.o
	${RM} src/bsd/*.d src/bsd/*.o
	${RM} stamp-*
	${RM} -r lib

-include $(wildcard *.d) dummy
