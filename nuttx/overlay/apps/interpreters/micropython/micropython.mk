# The MicroPython half of the build.
#
# MicroPython works out its own string-interning tables by preprocessing
# every source it is going to compile, so that step has to run with the same
# compiler and flags NuttX will use, or the tables describe a different
# program.  This is a port makefile in MicroPython's own shape, run from the
# application's Makefile for the generated headers only; NuttX then compiles
# the sources itself.

include $(TOP)/py/mkenv.mk

QSTR_DEFS = $(PORT_DIR)/qstrdefsport.h
MICROPY_ROM_TEXT_COMPRESSION ?= 1

include $(TOP)/py/py.mk

INC += -I$(PORT_DIR) -I$(TOP) -I$(BUILD)
CFLAGS += $(INC) $(CFLAGS_EXTRA)

SRC_C = $(PORT_DIR)/mphalport.c $(PORT_DIR)/micropython_main.c
SRC_QSTR += $(TOP)/shared/readline/readline.c $(TOP)/shared/runtime/pyexec.c
SRC_QSTR += $(TOP)/extmod/vfs.c $(TOP)/extmod/vfs_reader.c
SRC_QSTR += $(TOP)/extmod/vfs_posix.c $(TOP)/extmod/vfs_posix_file.c
SRC_QSTR += $(TOP)/extmod/modos.c $(TOP)/extmod/modtime.c
SRC_QSTR += $(TOP)/extmod/moductypes.c
SRC_QSTR += $(PORT_DIR)/micropython_main.c $(PORT_DIR)/mphalport.c

headers: $(BUILD)/genhdr/qstrdefs.generated.h $(BUILD)/genhdr/moduledefs.h \
         $(BUILD)/genhdr/root_pointers.h $(BUILD)/genhdr/compressed.data.h

include $(TOP)/py/mkrules.mk
