# romdump: copy the WikiReader's boot FLASH and the S1C33E07 boot ROM to the card.
# SPDX-License-Identifier: GPL-3.0-or-later

ROOT_DIR := $(abspath ..)
HERE := $(abspath .)
MK_DIR := $(ROOT_DIR)/samo-lib/Mk
TOOLCHAIN_BIN ?= $(ROOT_DIR)/host-tools/toolchain-c33/work/install/bin
include $(MK_DIR)/definitions.mk
include $(GRIFO_APPLICATION_PRE)

PROGRAM = romdump
ICON = YES
SOURCES = romdump.c
CFLAGS += -std=gnu11

include $(GRIFO_APPLICATION_POST)
