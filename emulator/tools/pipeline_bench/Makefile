ROOT := $(abspath ../../..)
OUT ?= $(ROOT)/build/wr128/pipeline
PROGRAM ?= pipeline
SOURCE ?= pipeline.c
TOOLCHAIN_BIN ?= $(ROOT)/host-tools/toolchain-c33/work/install/bin
CC := $(TOOLCHAIN_BIN)/c33-epson-elf-gcc
STRIP := $(TOOLCHAIN_BIN)/c33-epson-elf-strip
CFLAGS := -O2 -mc33pe -mlong-calls -std=gnu99 -fgnu89-inline \
 -fno-builtin -ffunction-sections -fdata-sections -Wall -Wextra -Werror \
 -I$(ROOT)/samo-lib/grifo/include -I$(ROOT)/samo-lib/include \
 -I$(ROOT)/samo-lib/mini-libc/include -I$(ROOT)/samo-lib/drivers/include

.PHONY: all
all: $(OUT)/$(PROGRAM).app $(OUT)/$(PROGRAM).ico
$(OUT):
	mkdir -p $@
$(OUT)/$(PROGRAM).o: $(SOURCE) Makefile | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/$(PROGRAM).elf: $(OUT)/$(PROGRAM).o $(ROOT)/samo-lib/grifo/lds/application.lds $(ROOT)/samo-lib/grifo/lib/libgrifo.a $(ROOT)/samo-lib/mini-libc/lib/libc.a
	$(CC) -nostdlib -mc33pe -Wl,--gc-sections,-T,$(ROOT)/samo-lib/grifo/lds/application.lds,-Map,$(OUT)/$(PROGRAM).map \
	 -o $@ $< $(ROOT)/samo-lib/grifo/lib/libgrifo.a $(ROOT)/samo-lib/mini-libc/lib/libc.a -lgcc
$(OUT)/$(PROGRAM).app: $(OUT)/$(PROGRAM).elf
	$(STRIP) --strip-unneeded --strip-debug --remove-section=.lcd -o $@ $<
$(OUT)/$(PROGRAM).ico: Makefile | $(OUT)
	python3 -c 'import sys; rows = [0xffffffffffffffff] + [0x8000000000000001] * 62 + [0xffffffffffffffff]; sys.stdout.buffer.write(b"".join(r.to_bytes(8, "big") for r in rows))' > $@
