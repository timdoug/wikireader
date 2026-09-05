# Makefile
#
# Copyright (c) 2009 Openmoko Inc.
#
# Authors   Christopher Hall <hsw@openmoko.com>
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


FATFS_MODE = modern

# +++START_UPDATE_MAKEFILE: Start of auto included code
# The text between the +++ and --- tags is copied by the
# UpdateMakefiles script. Do not remove or change these tags.
# ---
# Autodetect root directory
define FIND_ROOT_DIR
while : ; do \
  d=$$(pwd) ; \
  [ -d "$${d}/samo-lib" ] && echo $${d} && exit 0 ; \
  [ X"/" = X"$${d}" ] && echo ROOT_DIRECTORY_NOT_FOUND && exit 1 ; \
  cd .. ; \
done
endef
ROOT_DIR := $(shell ${FIND_ROOT_DIR})
# Directory of Makefile includes
MK_DIR   := ${ROOT_DIR}/samo-lib/Mk
# Include the initial Makefile setup
include ${MK_DIR}/definitions.mk
# ---END_UPDATE_MAKEFILE: End of auto included code


INSTALL_GRIFO_SIMULATION ?= NO
BUILD_EXAMPLES ?= NO

$(call REQUIRED_BINARY, guile, guile)

INCLUDES += -Isrc
INCLUDES += -I-
INCLUDES += -I${SAMO_LIB_INCLUDE}
INCLUDES += -I${FATFS_INCLUDE} -I${FATFS_CONFIG_INCLUDE}
INCLUDES += -I${DRIVERS_INCLUDE}
INCLUDES += -I${MINI_LIBC_INCLUDE}
INCLUDES += -Ibuild -Icommon

LIBS += build/libinternal.a
LIBS += ${FATFS_LIB}
LIBS += ${MINI_LIBC_LIB}
LIBS += ${DRIVERS_LIB}
# Ask for the libgcc that matches the target core.  The cores are not
# link-compatible -- the assembler stamps the variant into e_flags and
# the linker refuses to mix them -- so this query needs the same -mc33pe
# the sources are built with, or it returns the base-core multilib.
LIBS += $(shell $(CC) $(TARGET_ARCH_FLAGS) -print-libgcc-file-name)

BUILD_PREFIX := build/

MAKE_SYSCALL := scripts/MakeSystemCalls

vpath %.c :src:common

TARGETS = build/syscall.table
TARGETS += build/libinternal.a
TARGETS += grifo.elf
TARGETS += lib/libgrifo.a

# list of modules for creating libinternal.a
OBJECTS += analog.o
OBJECTS += button.o
OBJECTS += CMU.o
OBJECTS += CTP.o
OBJECTS += delay.o
OBJECTS += elf32.o
OBJECTS += event.o
OBJECTS += file.o
OBJECTS += graphics.o
OBJECTS += LCD.o
OBJECTS += main.o
OBJECTS += memory.o
OBJECTS += serial.o
OBJECTS += suspend.o
OBJECTS += syscall.o
OBJECTS += system.o
OBJECTS += timer.o
OBJECTS += vector.o
OBJECTS += watchdog.o
#OBJECTS +=

BUILD_OBJECTS = $(addprefix ${BUILD_PREFIX},${OBJECTS})

# FatFs exFAT uses 64-bit file sizes and requires C99 declarations.  Keep the
# setting local to its sole consumer so the historical startup code retains
# the dialect it was written for.
build/file.o: CFLAGS += -std=gnu99

# SD block payloads by HSDMA/IDMA (SD_DMA=YES) or by the MMC driver's own
# byte-at-a-time SPI loop (NO).  The DMA path runs in the emulator but on a
# real board (stock 2009 flash, 8 GB card, 2026-09-05) the kernel never got
# as far as init.app with it, and swapping in a SD_DMA=NO kernel booted, so
# the emulator's DMA model is forgiving something the silicon is not.  Keep
# PIO the default until the DMA driver has been debugged on hardware.
SD_DMA ?= NO
ifeq (YES,$(strip ${SD_DMA}))
SD_DMA_OBJECT = build/sd_dma.o
CFLAGS += -DSD_DMA_ENABLED=1
else
SD_DMA_OBJECT =
CFLAGS += -DSD_DMA_ENABLED=0
endif

.PHONY: all
all:  ${TARGETS}
	${MAKE} -C applications
ifeq (YES,$(strip ${BUILD_EXAMPLES}))
	${MAKE} -C examples
endif

# SuspendCode runs from A0RAM after the SDRAM holding the stack has been
# switched off, so any stack reference in it reads or writes nothing on the
# hardware.  gcc 3.3 kept every local in a register; gcc 16 spilled the saved
# clock registers and resumed with garbage clocks.  Refuse to link such a
# build rather than find out on the device.
build/stamp-suspend-stack: build/suspend.o
	@if ${OBJDUMP} -d -j .suspend_text $< | grep -q '%sp'; then \
	  echo "error: SuspendCode uses the stack (see src/suspend.c)"; \
	  ${OBJDUMP} -d -j .suspend_text $< | grep '%sp'; \
	  exit 1; \
	fi
	@touch "$@"

build/libinternal.a: stamp-build ${BUILD_OBJECTS} build/stamp-suspend-stack
	${RM} "$@"
	${AR} r "$@" ${BUILD_OBJECTS}

include/grifo.h stubs/stamp-source build/syscall.table: stamp-include stamp-build stamp-stubs src/syscall.list ${MAKE_SYSCALL} src/*.h
	${MAKE_SYSCALL} --prefix=stubs/ \
	  --header=include/grifo.h \
	  --list=src/syscall.list \
	  --table=build/syscall.table \
	  --verbose
	${TOUCH} stubs/stamp-source

stubs/stamp-objects: stubs/stamp-source
	# Expand after syscall generation so new stubs are included on a clean build.
	${MAKE} $(addsuffix .o, $(basename $(wildcard stubs/*.s)))
	${TOUCH} "$@"

stubs/stamp: stamp-stubs stubs/stamp-objects
	${TOUCH} "$@"

# this should depend on lib, but make always rebuilds in that case; why?
lib/libgrifo.a: stamp-lib stubs/stamp stubs/stamp-source
	${RM} "$@"
	${AR} r "$@" stubs/*.o
	${TOUCH} "$@"

grifo.elf: build/main.o ${SD_DMA_OBJECT} lds/grifo.lds ${LIBS}
	$(LD) -o $@ ${LDFLAGS} build/main.o ${SD_DMA_OBJECT} ${LIBS} -T lds/${@:.elf=.lds} -Map ${@:.elf=.map}
	${STRIP} --strip-unneeded --strip-debug \
		--remove-section=.internal --remove-section=.lcd \
		--remove-section=.user_area --remove-section=.descriptor "$@"
	${OBJDUMP} --section-headers "$@"
	${OBJDUMP} -D "$@" > "${@:.elf=.dump}"

stamp-build:
	${MKDIR} "${@:stamp-%=%}"
	${TOUCH} "$@"

stamp-lib:
	${MKDIR} "${@:stamp-%=%}"
	${TOUCH} "$@"

stamp-stubs:
	${MKDIR} "${@:stamp-%=%}"
	${TOUCH} "$@"

stamp-include:
	${MKDIR} "${@:stamp-%=%}"
	${TOUCH} "$@"


.PHONY: install
install: all
	@if [ ! -d "${DESTDIR}" ] ; then echo DESTDIR: "'"${DESTDIR}"'" is not a directory ; exit 1; fi
ifneq (YES,$(strip ${INSTALL_GRIFO_SIMULATION}))
	${COPY} grifo.elf "${DESTDIR}"/kernel.elf
endif
	${MAKE} -C applications install INSTALL_GRIFO_SIMULATION="${INSTALL_GRIFO_SIMULATION}"
ifeq (YES,$(strip ${BUILD_EXAMPLES}))
	${COPY} init.ini "${DESTDIR}"/
	${MAKE} -C examples install INSTALL_GRIFO_SIMULATION="${INSTALL_GRIFO_SIMULATION}"
endif

.PHONY: clean
clean:
	${RM} -r ${TARGETS} build include lib stubs
	${RM} -r *.o *.elf *.d *.map *.asm33 *.dump *.ico stamp-*
	${MAKE} -C applications clean
	${MAKE} -C examples clean


include ${MK_DIR}/rules.mk
