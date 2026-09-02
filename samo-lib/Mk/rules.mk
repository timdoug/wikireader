# Copyright (c) 2009 Openmoko Inc.
#
# Authors   Daniel Mack <daniel@caiaq.de>
#           Holger Hans Peter Freyther <zecke@openmoko.org>
#           Christopher Hall <hsw@openmoko.com>
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
#

# Debug format.  The 2002 toolchain emitted STABS; gcc 16 dropped STABS
# entirely, and both toolchains understand DWARF 2, so that is the default.
# Override if you need the original format.
DEBUG_CFLAGS ?= -gdwarf-2
DEBUG_ASFLAGS ?= --gdwarf-2

# gcc 3.3 used gnu89 "extern inline" semantics: a definition in a header was
# inline-only.  gcc 5 and later default to C99, where the same text emits an
# external definition and collides with the real one in the .c file --
# ctype.h and the src/string/is*.c pair being the case here.  Ask for the old
# semantics when the compiler understands the option.
GNU89_INLINE := $(shell $(CC) -fgnu89-inline -E -x c /dev/null >/dev/null 2>&1 && echo -fgnu89-inline)

# The core variant.  Kept separate because it also has to be passed to
# "gcc -print-libgcc-file-name" so that query picks the right multilib.
TARGET_ARCH_FLAGS ?= -mc33pe

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
CFLAGS += -Wall -Werror -I. $(DEBUG_CFLAGS) $(GNU89_INLINE) -mno-long-calls -fno-builtin $(OPT) $(TARGET_ARCH_FLAGS) $(INCLUDES)
ASFLAGS = -mc33pe --fatal-warnings

# protection in case some Makefile includes this too early
.PHONY: this-is-included-too-early
this-is-included-too-early:
	@echo This is rules.mk reporting an error
	@echo move the '"include"' to the bottom of the Makefile.
	@echo Otherwise the dependencies are not built in the correct order
	@exit 1

# just so that all the REQUIRED_xxx can be evaluated
.PHONY: requires
	true

# some debugging rules

# use this like: 'make print-PATH print-CFLAGS' to see the value of make variable PATH and CFLAGS
print-%:
	@echo $* is $($*)


# compilation rules
%.o: %.c
	$(GCC) -MM $(CFLAGS) -MT $@ $< > ${@:.o=.d}
	$(GCC) -E $(CFLAGS) $< > ${@:.o=.p}
	$(GCC) $(CFLAGS) -c -o $@ -Wa,-ahl=${@:.o=.asm33} $<

${BUILD_PREFIX}%.o: %.c
	$(GCC) -MM $(CFLAGS) -MT $@ $< > ${@:.o=.d}
	$(GCC) -E $(CFLAGS) $< > ${@:.o=.p}
	$(GCC) $(CFLAGS) -c -o $@ -Wa,-ahl=${@:.o=.asm33} $<

%.o: %.s
	${AS} -o $@ ${ASFLAGS} -ahlsm=${@:.o=.lst} $<

${BUILD_PREFIX}%.o: %.s
	${AS} -o $@ ${ASFLAGS} -ahlsm=${@:.o=.lst} $<


# convert XPM to binary ICO format
%.ico: %.xpm
	${GRIFO_SCRIPTS}/xpm2icon --icon="$@" "$<"

# convert PNG to C header file
%.h: %.png ${IMAGE2HEADER}
	${IMAGE2HEADER} --inverted --header-file="$@" --variable-name="$(notdir ${@:.h=_image})" "$<"

${BUILD_PREFIX}%.h: %.png ${IMAGE2HEADER}
	${IMAGE2HEADER} --inverted --header-file="$@" --variable-name="$(notdir ${@:.h=_image})" "$<"


-include $(wildcard *.d) dummy
-include $(wildcard ${BUILD_PREFIX}*.d) dummy
