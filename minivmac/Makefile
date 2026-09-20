# Mini vMac for the WikiReader, using a pinned upstream source tree.
# SPDX-License-Identifier: GPL-3.0-or-later

ROOT_DIR := $(abspath ..)
HERE := $(abspath .)
WORK := $(HERE)/work/minivmac
UPSTREAM_SRC := $(WORK)/src
UPSTREAM_CFG := $(WORK)/cfg
MK_DIR := $(ROOT_DIR)/samo-lib/Mk
TOOLCHAIN_BIN ?= $(ROOT_DIR)/host-tools/toolchain-c33/work/install/bin
TARGET_ARCH_FLAGS ?= -mc33pe
JIT ?= 1

include $(MK_DIR)/definitions.mk
include $(GRIFO_APPLICATION_PRE)

PROGRAM = minivmac
ICON = YES
SOURCES = minivmac.c

CORE_NAMES = GLOBGLUE M68KITAB MINEM68K VIAEMDEV IWMEMDEV SCCEMDEV \
	RTCEMDEV ROMEMDEV SCSIEMDV SONYEMDV SCRNEMDV MOUSEMDV KBRDEMDV PROGMAIN
CORE_OBJECTS = $(addprefix build/core/,$(addsuffix .o,$(CORE_NAMES)))

INCLUDES += -I$(HERE)/cfg -I$(UPSTREAM_CFG) -I$(UPSTREAM_SRC)
CFLAGS += -std=gnu11 -fwrapv -fno-strict-aliasing
CFLAGS += -DMINIVMAC_JIT=$(JIT)
LIBS := lib/libminivmac.a $(LIBS)

build/minivmac.o: CFLAGS += -Wno-error -Wno-unused-function -Wno-unused-variable
build/core/MINEM68K.o: CFLAGS += -mlong-calls

include $(GRIFO_APPLICATION_POST)

PREBUILD_TARGETS += context
minivmac.app: context
$(BUILD_OBJECTS): | context build

.PHONY: context fetch
context: $(WORK)/cfg/CNFUDPIC.h
fetch: context

$(WORK)/cfg/CNFUDPIC.h: revision fetch.sh patches/wikireader-fast-m68k.patch
	./fetch.sh

build/core:
	$(MKDIR) "$@"

# Upstream intentionally supports old compilers and many hosts.  Keep its
# warnings informative without making a new GCC warning break this port.
$(CORE_OBJECTS): build/core/%.o: | context build/core
	$(GCC) -MM $(CFLAGS) -Wno-error -MT $@ $(UPSTREAM_SRC)/$*.c > ${@:.o=.d}
	$(GCC) $(CFLAGS) -Wno-error -Wno-unused-function -Wno-unused-variable \
		-Wno-missing-prototypes -c -o $@ $(UPSTREAM_SRC)/$*.c

lib/libminivmac.a: $(CORE_OBJECTS) | lib
	$(RM) "$@"
	$(AR) r "$@" $(CORE_OBJECTS)

.PHONY: realclean
realclean: clean
	rm -rf work

-include $(wildcard build/core/*.d)
