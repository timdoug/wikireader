# SPDX-License-Identifier: GPL-2.0
KBUILD_DEFCONFIG := wikireader_defconfig

KBUILD_CFLAGS += -pipe -D__linux__ -D__ELF__ -mc33pe -medda32
KBUILD_CFLAGS += -fno-builtin -fno-optimize-sibling-calls
KBUILD_AFLAGS += -mc33pe -medda32

libs-y += arch/c33/lib/ $(shell $(CC) $(KBUILD_CFLAGS) -print-libgcc-file-name)

all: vmlinux

define archhelp
  echo  '* vmlinux        - ELF image loadable by Grifo'
endef
