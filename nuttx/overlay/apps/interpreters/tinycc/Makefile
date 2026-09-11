# SPDX-License-Identifier: LGPL-2.0-or-later
include $(APPDIR)/Make.defs
PROGNAME = tcc
PRIORITY = 100
STACKSIZE = 65536
MODULE = $(CONFIG_INTERPRETERS_TCC)
MAINSRC = tcc_main.c
CSRCS = tcc_library.c
ASRCS = c33_setjmp.S
# The compiler sources are a separate checkout: this application is only the
# NuttX side of the port.  A relative path is taken from the apps directory.

TCCDIR := $(subst ",,$(CONFIG_INTERPRETERS_TCC_SRCDIR))

ifeq ($(TCCDIR),)
TCCDIR := ../tinycc
endif

ifeq ($(filter /%,$(TCCDIR)),)
TCCROOT := $(realpath $(APPDIR)/$(TCCDIR))
else
TCCROOT := $(realpath $(TCCDIR))
endif

ifeq ($(TCCROOT),)
$(error No TinyCC checkout at $(TCCDIR); set CONFIG_INTERPRETERS_TCC_SRCDIR)
endif
CFLAGS += -I$(CURDIR) -I$(CURDIR)/compat -I$(TCCROOT)
CFLAGS += -Wno-error -Wno-unused-function -Wno-unused-parameter
CFLAGS += -Wno-shadow -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
CFLAGS += -Wno-format -Wno-implicit-fallthrough -Wno-undef
include $(APPDIR)/Application.mk

$(OBJS): $(wildcard $(TCCROOT)/*.c $(TCCROOT)/*.h) tccdefs_.h tcc_files.h
tcc_files.h: $(wildcard include/*.h include/sys/*.h examples/*.c $(TCCROOT)/*.c $(TCCROOT)/*.h) $(TCCROOT)/stab.def embed.py bootstrap.c config.h runtime.h tccdefs_.h
	python3 embed.py $(TCCROOT)
tccdefs_.h: $(TCCROOT)/include/tccdefs.h $(TCCROOT)/conftest.c
	$(HOSTCC) -DC2STR $(TCCROOT)/conftest.c -o c2str
	./c2str $< $@

# The two generated headers belong to the context phase, and the apps
# directory's .context stamp keeps that from running a second time.  Removing
# them in clean:: therefore leaves a tree that cannot build: nothing
# regenerates them, and Make.dep still names them as prerequisites -- relative
# or absolute depending on how the preprocessor reached each one, so no rule
# here matches either.  They go with the configuration instead.
clean::
	$(RM) c2str
distclean::
	$(RM) c2str tccdefs_.h tcc_files.h
context:: tccdefs_.h tcc_files.h
