ROOT_DIR := $(abspath ..)
MK_DIR := $(ROOT_DIR)/samo-lib/Mk
TOOLCHAIN_BIN ?= $(ROOT_DIR)/host-tools/toolchain-c33/work/install/bin
include $(MK_DIR)/definitions.mk
include $(GRIFO_APPLICATION_PRE)

PROGRAM = llama
SOURCES = llama.c model.c tokenizer.c runner.c fixed.c
CFLAGS += -std=gnu11

# There is no instruction cache on this part and the fetch window is 27
# bytes, so a loop runs out of the queue only if its offset within the line
# plus its body fits. matmul's inner loop is fourteen bytes and is where
# the entire model's arithmetic happens; whether it lands at offset 2 or
# offset 14 of a line is decided by the linker, and changes whenever an
# unrelated edit moves anything ahead of it.
#
# Measured, by editing the screen code in llama.c and nothing else: the
# loop's fetch stall went from 4.2% of its cycles to 71.2%, and a token
# from 130 ms to 330. Aligning it is not a micro-optimization, it is the
# difference between the measurement being reproducible and being luck.
build/model.o: CFLAGS += -falign-loops=16

include $(GRIFO_APPLICATION_POST)

$(BUILD_OBJECTS): | build

HOST_CC ?= cc
HOST_CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror

# The host build is the same model, tokenizer and generation loop with
# malloc and stdio behind them, so correctness can be checked in a second
# rather than in a minute under the emulator.
.PHONY: host
host: build/wrllama

build/wrllama: host.c model.c tokenizer.c runner.c fixed.c $(wildcard *.h) | build
	$(HOST_CC) $(HOST_CFLAGS) -o $@ host.c model.c tokenizer.c runner.c fixed.c

.PHONY: test
test: build/wrllama
	python3 tests/run_tests.py
