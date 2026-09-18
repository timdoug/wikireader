ROOT_DIR := $(abspath ..)
MK_DIR := $(ROOT_DIR)/samo-lib/Mk
TOOLCHAIN_BIN ?= $(ROOT_DIR)/host-tools/toolchain-c33/work/install/bin
include $(MK_DIR)/definitions.mk
include $(GRIFO_APPLICATION_PRE)

PROGRAM = riscv
ICON = YES
SOURCES = riscv.c rv32.c console.c jit_probe.s
CFLAGS += -std=gnu11 -fwrapv -fno-strict-aliasing
LDFLAGS += -T memory.lds

# The three placement switches, all on by default: FAST puts code in the
# chip's zero-wait A0 RAM, where it escapes both the SDRAM fetch cost and
# the 27-byte loop residency window; STATE puts the guest register file
# there; ASM runs the hand-written hot path.  compare.py turns them off one
# at a time to price each.
FAST ?= 1
STATE ?= 1
ASM ?= 1

ifeq (1,$(strip $(FAST)))
CFLAGS += -DRV32_FASTCODE
endif

# STATE=1 additionally places the guest register file in that RAM.
ifeq (1,$(strip $(STATE)))
CFLAGS += -DRV32_FASTSTATE
endif

# JIT=1 translates instead of interpreting: the code cache goes in SDRAM,
# which is where jit_probe.s measured a well-translated loop running fastest,
# and only the entry and the exits need the fast memory.  It replaces the
# assembly hot path rather than joining it -- the two want the same A0 RAM,
# and what the translator cannot do the C interpreter still does.
JIT ?= 0
JITSKIP ?= 0
ifeq (1,$(strip $(JIT)))
CFLAGS += -DRV32_JIT -DJIT_SKIP=$(strip $(JITSKIP))
SOURCES += rv32_jit.c rv32_jitrt.s
ASM = 0
else
SOURCES += rv32_hot.s
endif

# ASM=1 runs the hand-written hot path (rv32_hot.s) and leaves the C
# interpreter in SDRAM as the fallback for what it declines.
ifeq (1,$(strip $(ASM)))
CFLAGS += -DRV32_ASM
endif

EXTRA_TARGETS += build/rvbench.bin rvlinux.ico rvjit.ico

include $(GRIFO_APPLICATION_POST)

$(BUILD_OBJECTS): | build
riscv.app: memory.lds

# Internal RAM is at 0xc00 and 0x81a00, SDRAM at 0x10040000, far outside the
# reach of the C33's short call.  Every call into or out of those regions has
# to take the long form -- including the ones the compiler makes to libgcc
# for the guest's divide, which memory.lds places in the window buffer for
# every build.  rules.mk appends -mno-long-calls to CFLAGS after this file is
# read, so the override has to be per target.
build/rv32.o build/riscv.o build/console.o build/rv32_jit.o: CFLAGS += -mlong-calls

# The guest image.  A separate architecture with a separate toolchain, so it
# is built here rather than through the C33 rules.
RISCV_PREFIX ?= riscv64-unknown-elf-
SCALE ?= 1
RVCFLAGS = -DSCALE=$(SCALE) -march=rv32ima_zicsr -mabi=ilp32 -O2 -g -ffreestanding -nostdlib \
           -fno-builtin -fno-tree-loop-distribute-patterns -Wall -Wextra

build/rvbench.elf: guest/start.S guest/bench.c guest/bench.lds Makefile | build
	$(RISCV_PREFIX)gcc $(RVCFLAGS) -T guest/bench.lds \
	    guest/start.S guest/bench.c -o $@

build/rvbench.bin: build/rvbench.elf
	$(RISCV_PREFIX)objcopy -O binary $< $@

# A guest that echoes what it is given, for testing the panel console and
# the keyboard without waiting out a Linux boot.
build/echo.elf: guest/start.S guest/echo.c guest/bench.lds | build
	$(RISCV_PREFIX)gcc $(RVCFLAGS) -T guest/bench.lds \
	    guest/start.S guest/echo.c -o $@

build/echo.bin: build/echo.elf
	$(RISCV_PREFIX)objcopy -O binary $< $@

.PHONY: echo
echo: build/echo.bin

# Host-side checks: the interpreter against a native build of the same
# kernels, which is what makes a cycle measurement worth taking.
HOST_CC ?= cc

.PHONY: test
test: build/rvbench.bin | build
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror \
	    -Wno-initializer-overrides -fsanitize=address,undefined tests/host_run.c rv32.c -o build/host_run
	$(HOST_CC) -std=c11 -O2 -w -fwrapv tests/ref.c -o build/ref
	./build/ref > build/ref.txt
	./build/host_run build/rvbench.bin > build/guest.txt 2> build/guest.log
	sed -n '2,11p' build/guest.txt | diff -u build/ref.txt -
	@echo "checksums match; guest instruction counts:"
	@sed -n '/^kernel/,$$p' build/guest.txt

.PHONY: bench
bench: riscv.app build/rvbench.bin
	python3 run.py

# What a guest actually executes, on the build machine and at native speed:
# a Linux boot is thirty-eight million instructions, which is nine minutes
# under the full-system emulator and under a second here.
.PHONY: mix
mix: | build
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -fwrapv \
	    tests/host_mix.c rv32.c -o build/host_mix
	./build/host_mix --image linux/Image --dtb linux/wr-sh.dtb \
	    --until '# ' --quiet

# Every instruction the translator's emitter can produce, against what the
# toolchain's assembler makes of the same thing.  A wrong field is a different
# instruction, and a different instruction is a guest that misbehaves minutes
# later and somewhere else.
.PHONY: jitenc
jitenc: | build
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fwrapv \
	    tests/host_enc.c -o build/host_enc
	python3 jitenc.py

# What the translator emits, read back through the toolchain's disassembler.
# A wrong field lands as some other instruction and shows up minutes into a
# boot; here it is visible in a second.
.PHONY: jitdis
jitdis: | build
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fwrapv \
	    tests/host_jit.c -o build/host_jit
	./build/host_jit > build/jitdis.s
	$(TOOLCHAIN_BIN)/c33-epson-elf-as -mc33pe -o build/jitdis.o build/jitdis.s
	$(TOOLCHAIN_BIN)/c33-epson-elf-objdump -d build/jitdis.o
