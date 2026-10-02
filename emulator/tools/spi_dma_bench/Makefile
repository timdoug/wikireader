ROOT := $(abspath ../../..)
OUT ?= $(ROOT)/build/wr128/spi-dma
TOOLCHAIN_BIN ?= $(ROOT)/host-tools/toolchain-c33/work/install/bin
CC := $(TOOLCHAIN_BIN)/c33-epson-elf-gcc
STRIP := $(TOOLCHAIN_BIN)/c33-epson-elf-strip
CFLAGS := -O2 -mc33pe -mlong-calls -std=gnu99 -fgnu89-inline -falign-loops=16 \
 -fno-builtin -ffunction-sections -fdata-sections -Wall -Werror \
 -I$(ROOT)/samo-lib/grifo/include -I$(ROOT)/samo-lib/include \
 -I$(ROOT)/samo-lib/mini-libc/include -I$(ROOT)/samo-lib/drivers/include

.PHONY: all
all: $(OUT)/spibench.app $(OUT)/spibench.ico
$(OUT):
	mkdir -p $@
$(OUT)/spibench.o: spibench.c Makefile | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/spibench.elf: $(OUT)/spibench.o $(ROOT)/samo-lib/grifo/lds/application.lds $(ROOT)/samo-lib/grifo/lib/libgrifo.a $(ROOT)/samo-lib/mini-libc/lib/libc.a
	$(CC) -nostdlib -mc33pe -Wl,--gc-sections,-T,$(ROOT)/samo-lib/grifo/lds/application.lds,-Map,$(OUT)/spibench.map \
	 -o $@ $< $(ROOT)/samo-lib/grifo/lib/libgrifo.a $(ROOT)/samo-lib/mini-libc/lib/libc.a -lgcc
$(OUT)/spibench.app: $(OUT)/spibench.elf
	$(STRIP) --strip-unneeded --strip-debug --remove-section=.lcd -o $@ $<
# A framed square: 64x64, one bit a pixel, MSB first, 1 = black.
$(OUT)/spibench.ico: Makefile | $(OUT)
	python3 -c 'import sys; rows = [0xffffffffffffffff] + [0x8000000000000001] * 62 + [0xffffffffffffffff]; sys.stdout.buffer.write(b"".join(r.to_bytes(8, "big") for r in rows))' > $@
