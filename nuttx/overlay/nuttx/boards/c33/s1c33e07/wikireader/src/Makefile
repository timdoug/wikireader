# SPDX-License-Identifier: Apache-2.0
include $(TOPDIR)/Make.defs
CSRCS = wikireader_boot.c
CFLAGS += -I$(TOPDIR)/arch/c33/src/s1c33e07/hardware
ifeq ($(CONFIG_WIKIREADER_GRIFO_APP),y)
CSRCS += wikireader_grifo.c
endif
ifeq ($(CONFIG_WIKIREADER_LCD),y)
CSRCS += wikireader_lcd.c
endif
ifeq ($(CONFIG_WIKIREADER_TOUCH),y)
CSRCS += wikireader_touch.c
endif
ifeq ($(CONFIG_WIKIREADER_TERMINAL),y)
CSRCS += wikireader_terminal.c
endif
include $(TOPDIR)/boards/Board.mk
