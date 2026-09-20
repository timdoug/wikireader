ROOT_DIR := $(abspath ..)
MK_DIR := $(ROOT_DIR)/samo-lib/Mk
TOOLCHAIN_BIN ?= $(ROOT_DIR)/host-tools/toolchain-c33/work/install/bin
include $(MK_DIR)/definitions.mk
include $(GRIFO_APPLICATION_PRE)

PROGRAM = llama
SOURCES = llama.c model.c tokenizer.c runner.c fixed.c
CFLAGS += -std=gnu11

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
