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
all: $(OUT)/membench.app $(OUT)/membench.ico
$(OUT):
	mkdir -p $@
$(OUT)/membench.o: membench.c Makefile | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/membench.elf: $(OUT)/membench.o $(ROOT)/samo-lib/grifo/lds/application.lds $(ROOT)/samo-lib/grifo/lib/libgrifo.a $(ROOT)/samo-lib/mini-libc/lib/libc.a
	$(CC) -nostdlib -mc33pe -Wl,--gc-sections,-T,$(ROOT)/samo-lib/grifo/lds/application.lds,-Map,$(OUT)/membench.map \
	 -o $@ $< $(ROOT)/samo-lib/grifo/lib/libgrifo.a $(ROOT)/samo-lib/mini-libc/lib/libc.a -lgcc
$(OUT)/membench.app: $(OUT)/membench.elf
	$(STRIP) --strip-unneeded --strip-debug --remove-section=.lcd -o $@ $<
# A framed square with a diagonal: 64x64, one bit a pixel, MSB first, 1 = black.
$(OUT)/membench.ico: Makefile | $(OUT)
	python3 -c 'import sys; rows = [(0xffffffffffffffff if y in (0, 63) else 0x8000000000000001 | (1 << (63 - y))) for y in range(64)]; sys.stdout.buffer.write(b"".join(r.to_bytes(8, "big") for r in rows))' > $@
