############################################################################
# apps/interpreters/micropython/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = micropython
PRIORITY  = $(CONFIG_INTERPRETERS_MICROPYTHON_PRIORITY)
STACKSIZE = $(CONFIG_INTERPRETERS_MICROPYTHON_STACKSIZE)
MODULE    = $(CONFIG_INTERPRETERS_MICROPYTHON)

MP_VERSION = $(patsubst "%",%,$(strip $(CONFIG_INTERPRETERS_MICROPYTHON_VERSION)))
MP_TARBALL = v$(MP_VERSION).tar.gz
MP_URL     = https://github.com/micropython/micropython/archive/refs/tags
MP_DIR     = micropython
MP_BUILD   = $(CURDIR)/mpbuild

# The generated headers have to be made with the flags the sources will be
# compiled with, so hand NuttX's own over to MicroPython's make.

MP_CFLAGS = $(ARCHCPUFLAGS) $(ARCHOPTIMIZATION) $(ARCHINCLUDES) \
            -std=gnu11 -ffreestanding -D__NuttX__

$(MP_TARBALL):
	@echo "Downloading: $(MP_TARBALL)"
	$(Q) curl -O -L $(MP_URL)/$(MP_TARBALL)

$(MP_DIR): $(MP_TARBALL)
	$(Q) tar zxf $(MP_TARBALL)
	$(Q) mv micropython-$(MP_VERSION) $(MP_DIR)
	$(Q) touch $(MP_DIR)

# One command writes all of these, which is the thing make has no way to
# say: a rule per header would run the generator once per header, and a rule
# for only the first leaves the others with no rule at all -- which is what a
# build after "clean" ran into, since the compiler's own dependency files
# name every one of them.  So the command has a stamp, and the headers are
# made by whatever made the stamp.

MP_HEADERS = $(MP_BUILD)/genhdr/qstrdefs.generated.h \
             $(MP_BUILD)/genhdr/moduledefs.h \
             $(MP_BUILD)/genhdr/root_pointers.h \
             $(MP_BUILD)/genhdr/compressed.data.h \
             $(MP_BUILD)/genhdr/mpversion.h

$(MP_BUILD)/headers.stamp: $(MP_DIR) mpconfigport.h qstrdefsport.h \
                           micropython.mk Makefile
	$(Q) $(MAKE) -f micropython.mk headers \
		TOP=$(CURDIR)/$(MP_DIR) PORT_DIR=$(CURDIR) BUILD=$(MP_BUILD) \
		CC="$(CC)" CFLAGS_EXTRA="$(MP_CFLAGS)" PYTHON=python3 \
		CROSS_COMPILE=
	$(Q) touch $@

$(MP_HEADERS): $(MP_BUILD)/headers.stamp
	@:

context:: $(MP_BUILD)/headers.stamp
depend:: $(MP_BUILD)/headers.stamp

CFLAGS += -I$(CURDIR) -I$(CURDIR)/$(MP_DIR) -I$(MP_BUILD)
CFLAGS += -Wno-error -Wno-undef -Wno-shadow -Wno-unused-parameter
CFLAGS += -Wno-missing-field-initializers -Wno-sign-compare
CFLAGS += -DNDEBUG

MAINSRC = micropython_main.c
CSRCS   = mphalport.c

# Everything in py/ except the native emitter: there is no C33 backend for
# it and the bytecode virtual machine is what this is for.

CSRCS += $(filter-out $(MP_DIR)/py/emitnative.c, $(wildcard $(MP_DIR)/py/*.c))
CSRCS += $(MP_DIR)/shared/readline/readline.c
CSRCS += $(MP_DIR)/shared/runtime/pyexec.c
CSRCS += $(MP_DIR)/shared/runtime/gchelper_generic.c
CSRCS += $(MP_DIR)/shared/runtime/stdout_helpers.c

# The filesystem and the modules that reach it.  vfs_posix_file.c also brings
# sys.stdin and friends as file objects, which is why sys_stdio_mphal.c is
# not here: they would be two definitions of the same three names.

CSRCS += $(MP_DIR)/extmod/vfs.c
CSRCS += $(MP_DIR)/extmod/vfs_reader.c
CSRCS += $(MP_DIR)/extmod/vfs_posix.c
CSRCS += $(MP_DIR)/extmod/vfs_posix_file.c
CSRCS += $(MP_DIR)/extmod/modos.c
CSRCS += $(MP_DIR)/extmod/moductypes.c
CSRCS += $(MP_DIR)/extmod/modtime.c
CSRCS += $(MP_DIR)/shared/timeutils/timeutils.c

# The generated headers belong with the unpacked tarball rather than with the
# objects: "clean" does not re-run the context step that makes them, so
# deleting them there leaves a tree that cannot even work out its own
# dependencies -- the compiler reads them while answering what includes what.

distclean::
	$(call DELDIR, $(MP_BUILD))
	$(call DELDIR, $(MP_DIR))
	$(call DELFILE, $(MP_TARBALL))

include $(APPDIR)/Application.mk

# Application.mk is what names the objects, and every one of them includes
# the generated string table.  That table is a function of mpconfigport.h and
# of the source list above, so it has to be a prerequisite of the build and
# not only of the one-time context step: a table made for a different
# configuration is missing names that the sources are about to use, and the
# error it gives is a hundred undeclared MP_QSTR_ constants.

$(OBJS): $(MP_BUILD)/headers.stamp
