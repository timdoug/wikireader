ROOT := $(abspath ../../..)
OUT ?= $(ROOT)/build/wr128/pc-read
TOOLCHAIN_BIN ?= $(ROOT)/host-tools/toolchain-c33/work/install/bin
CC := $(TOOLCHAIN_BIN)/c33-epson-elf-gcc
STRIP := $(TOOLCHAIN_BIN)/c33-epson-elf-strip
CFLAGS := -O2 -mc33pe -mlong-calls -std=gnu99 -fgnu89-inline -falign-loops=16 -ftrampolines \
 -fno-builtin -ffunction-sections -fdata-sections -Wall -Werror \
 -I$(ROOT)/samo-lib/grifo/include -I$(ROOT)/samo-lib/include \
 -I$(ROOT)/samo-lib/mini-libc/include -I$(ROOT)/samo-lib/drivers/include

.PHONY: all
all: $(OUT)/pctest.app $(OUT)/pctest.ico
$(OUT):
	mkdir -p $@
$(OUT)/pctest.o: pctest.c Makefile | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/pctest.elf: $(OUT)/pctest.o $(ROOT)/samo-lib/grifo/lds/application.lds $(ROOT)/samo-lib/grifo/lib/libgrifo.a $(ROOT)/samo-lib/mini-libc/lib/libc.a
	$(CC) -nostdlib -mc33pe -Wl,--gc-sections,-T,$(ROOT)/samo-lib/grifo/lds/application.lds,-Map,$(OUT)/pctest.map \
	 -o $@ $< $(ROOT)/samo-lib/grifo/lib/libgrifo.a $(ROOT)/samo-lib/mini-libc/lib/libc.a -lgcc
$(OUT)/pctest.app: $(OUT)/pctest.elf
	$(STRIP) --strip-unneeded --strip-debug --remove-section=.lcd -o $@ $<
# A framed square: 64x64, one bit a pixel, MSB first, 1 = black.
$(OUT)/pctest.ico: Makefile | $(OUT)
	python3 -c 'import sys; rows = [0xffffffffffffffff] + [0x8000000000000001 | (0x00ff000000000000 if 24 <= y < 40 else 0) for y in range(1, 63)] + [0xffffffffffffffff]; sys.stdout.buffer.write(b"".join(r.to_bytes(8, "big") for r in rows))' > $@
