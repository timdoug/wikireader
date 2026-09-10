ROOT := $(abspath ../../..)
OUT ?= $(ROOT)/build/wr128/mem-dma
TOOLCHAIN_BIN ?= $(ROOT)/host-tools/toolchain-c33/work/install/bin
CC := $(TOOLCHAIN_BIN)/c33-epson-elf-gcc
STRIP := $(TOOLCHAIN_BIN)/c33-epson-elf-strip
CFLAGS := -O2 -mc33pe -mlong-calls -std=gnu99 -fgnu89-inline -falign-loops=16 \
 -fno-builtin -ffunction-sections -fdata-sections \
 -I$(ROOT)/samo-lib/grifo/include -I$(ROOT)/samo-lib/include \
 -I$(ROOT)/samo-lib/mini-libc/include -I$(ROOT)/samo-lib/drivers/include

.PHONY: all
all: $(OUT)/membench.app
$(OUT):
	mkdir -p $@
$(OUT)/membench.o: membench.c Makefile | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/membench.elf: $(OUT)/membench.o $(ROOT)/samo-lib/grifo/lds/application.lds $(ROOT)/samo-lib/grifo/lib/libgrifo.a $(ROOT)/samo-lib/mini-libc/lib/libc.a
	$(CC) -nostdlib -mc33pe -Wl,--gc-sections,-T,$(ROOT)/samo-lib/grifo/lds/application.lds,-Map,$(OUT)/membench.map \
	 -o $@ $< $(ROOT)/samo-lib/grifo/lib/libgrifo.a $(ROOT)/samo-lib/mini-libc/lib/libc.a -lgcc
$(OUT)/membench.app: $(OUT)/membench.elf
	$(STRIP) --strip-unneeded --strip-debug --remove-section=.lcd -o $@ $<
