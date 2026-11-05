# wremu -- WikiReader (Epson C33) full-system emulator

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
LTO     ?= 1
PGO_GENERATE ?=
PGO_USE      ?=

ifneq ($(strip $(PGO_GENERATE)),)
ifneq ($(strip $(PGO_USE)),)
$(error PGO_GENERATE and PGO_USE are mutually exclusive)
endif
PGO_FLAGS := -fprofile-instr-generate=$(abspath $(PGO_GENERATE))/wremu-%m-%p.profraw
endif
ifneq ($(strip $(PGO_USE)),)
ifeq ($(wildcard $(PGO_USE)),)
$(error PGO_USE profile does not exist: $(PGO_USE))
endif
PGO_FLAGS := -fprofile-instr-use=$(abspath $(PGO_USE)) -Werror=profile-instr-out-of-date
endif

# SDL2 for the live panel window
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
override CFLAGS += $(if $(filter 1,$(LTO)),-flto) $(PGO_FLAGS) $(SDL_CFLAGS)
# Test programs have their own main(), absent from the emulator profile.
# Exercise the optimized objects without applying that profile to test code.
TEST_CFLAGS = $(filter-out -fprofile-instr-use=% -Werror=profile-instr-out-of-date,$(CFLAGS))
C33_OBJS = src/c33.o src/frontend.o
OBJS     = $(C33_OBJS) src/mem.o src/elf.o src/uart.o src/sdcard.o src/periph.o src/lcd.o src/display.o src/touch.o src/timer.o src/wdt.o src/itc.o src/cmu.o src/port.o src/eeprom.o src/sdramc.o src/dma.o src/model.o src/main.o
# Hash the profile as well as its path: merging new data into the same file
# must invalidate objects, even on make versions with coarse timestamps.
PGO_PROFILE_ID := $(if $(PGO_USE),$(shell cksum '$(PGO_USE)' 2>/dev/null))
FLAGS_CHANGED := $(shell printf '%s\n' '$(CC)' '$(CFLAGS)' '$(SDL_LIBS)' '$(PGO_PROFILE_ID)' | cmp -s - .build-flags || echo FORCE)

.PHONY: all clean tables test test-isa test-irq test-exception test-lcd test-adc test-cmu test-timer test-sdramc test-chipid test-port test-dma test-gui-boot check difftest test-manual test-sleigh FORCE

all: wremu

wremu: $(OBJS) .build-flags $(FLAGS_CHANGED)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(SDL_LIBS)

# Header dependencies come from the compiler. They used to be a hand-written
# list of every header, which is a list that goes stale: wdt.h was missing
# from it, so changing a struct layout left objects compiled against the old
# one and main.c read a different field than the device wrote. Nothing warns
# about that -- the numbers are just wrong.
src/%.o: src/%.c .build-flags $(FLAGS_CHANGED) $(PGO_USE)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# Rebuild when optimization/compiler/link flags change, including LTO=0.
# Preserve the stamp's timestamp when nothing changed so normal make is idle.
# Invalidate all objects even for a partial test build. FORCE also handles
# make versions whose timestamps cannot distinguish two builds in one second.
.build-flags: FORCE
	@printf '%s\n' '$(CC)' '$(CFLAGS)' '$(SDL_LIBS)' '$(PGO_PROFILE_ID)' > $@.tmp
	@cmp -s $@.tmp $@ || { mv $@.tmp $@; rm -f $(OBJS) wremu; }
	@rm -f $@.tmp

-include $(OBJS:.o=.d)

# Regenerate the decode tables from binutils' disassembler. Requires
# allinsn_full.txt, produced by disassembling all 65536 encodings with
# c33-epson-elf-objdump (see tools/gen_forms.py).
tables: allinsn_full.txt allinsn_pe.txt
	python3 tools/gen_forms.py allinsn_full.txt c33_forms.h
	python3 tools/gen_pe_valid.py allinsn_pe.txt c33_pe_valid.h

# Check the generated tables still agree with binutils on real firmware.
# ref-*.txt are local objdump captures of firmware, not in the repository, so
# skip rather than fail where they have not been made.  See tools/gen_forms.py.
test: c33_forms.h
	@if ! ls ref-*.txt >/dev/null 2>&1 ; then \
	    echo "test: no ref-*.txt present, skipping the binutils decode comparison" ; \
	else \
	    $(CC) $(TEST_CFLAGS) -o /tmp/test_decode tools/test_decode.c && \
	    cat ref-*.txt | /tmp/test_decode ; \
	fi

# Interrupt priority (PSR.IL) rules, per the C33 PE Core manual.
test-irq: tools/test_irq.c $(C33_OBJS) src/model.o src/itc.o src/mem.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_irq tools/test_irq.c $(C33_OBJS) src/model.o src/itc.o src/mem.o
	/tmp/test_irq

.PHONY: test-itc-select
test-itc-select: tools/test_itc_select.c src/itc.o src/mem.o src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_itc_select $^
	/tmp/test_itc_select

# Core instruction semantics not exercised by the firmware images.
test-isa: tools/test_isa.c $(C33_OBJS) src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_isa tools/test_isa.c $(C33_OBJS) src/model.o
	/tmp/test_isa

# Synchronous exception entry, saved PCs, IDIR, and PE-removed opcodes.
test-exception: tools/test_exception.c $(C33_OBJS) src/model.o src/mem.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_exception tools/test_exception.c $(C33_OBJS) src/model.o
	/tmp/test_exception

# LCD sub-window (PIP) compositing, against grifo's own window arithmetic.
test-lcd: tools/test_lcd.c src/lcd.o src/mem.o src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_lcd tools/test_lcd.c src/lcd.o src/mem.o src/model.o $(SDL_LIBS)
	/tmp/test_lcd

# ADC scaling, checked by running grifo's own conversions over the counts.
test-adc: tools/test_adc.c src/periph.o src/cmu.o src/mem.o src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_adc tools/test_adc.c src/periph.o src/cmu.o src/mem.o src/model.o
	/tmp/test_adc

# Window events: the path from an SDL click to the touch panel, which
# scripted runs bypass entirely by calling touch_post() directly.
test-display: tools/test_display.c src/display.o src/lcd.o src/mem.o src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_display tools/test_display.c src/display.o src/lcd.o src/mem.o src/model.o $(SDL_LIBS)
	/tmp/test_display

# SD writes: the path that makes the guest's history outlive a run.
test-sd: tools/test_sd.c src/sdcard.o src/cmu.o src/mem.o src/port.o src/eeprom.o src/itc.o $(C33_OBJS) src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_sd tools/test_sd.c src/sdcard.o src/cmu.o src/mem.o src/port.o src/eeprom.o src/itc.o $(C33_OBJS) src/model.o $(SDL_LIBS)
	/tmp/test_sd

.PHONY: test-sd-timing
test-sd-timing: tools/test_sd_timing.c src/sdcard.o src/cmu.o src/mem.o src/port.o src/eeprom.o src/itc.o $(C33_OBJS) src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_sd_timing $^ $(SDL_LIBS)
	/tmp/test_sd_timing

.PHONY: test-fat32-fixture
test-fat32-fixture:
	python3 tools/test_fat32_fixture.py

.PHONY: test-mmc-power
test-mmc-power:
	$(CC) $(TEST_CFLAGS) -Itools/mmc_mock -I../samo-lib/drivers/include \
		-I../samo-lib/fatfs/src -I../samo-lib/fatfs/config/c33/read-write \
		-o /tmp/test_mmc_power tools/test_mmc_power.c
	/tmp/test_mmc_power

# Timed event waits use the real kernel queue with deterministic IRQ races.
.PHONY: test-event-wait
test-event-wait:
	$(CC) $(TEST_CFLAGS) -I../samo-lib/grifo/common \
		-o /tmp/test_event_wait tools/test_event_wait.c
	/tmp/test_event_wait

# Watchdog, and above all the clock gate that keeps it quiet in suspend.
test-wdt: tools/test_wdt.c src/wdt.o src/cmu.o src/mem.o src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_wdt tools/test_wdt.c src/wdt.o src/cmu.o src/mem.o src/model.o $(SDL_LIBS)
	/tmp/test_wdt

# A timeout with RESEN restarts the machine rather than ending the run. That
# is how the firmware reboots -- System_reboot() arms the watchdog and halts,
# which is where NSH's "reboot" on the NuttX application ends up -- so a
# timeout that stopped the emulator could not be told from a crash.
#
# The guest is assembly and needs the cross assembler, which building the
# emulator otherwise does not.
C33_BIN ?= $(if $(TOOLCHAIN_BIN),$(TOOLCHAIN_BIN),../host-tools/toolchain-c33/work/install/bin)

.PHONY: test-wdt-reset
test-wdt-reset: wremu
	python3 tools/test_wdt_reset.py --toolchain "$(C33_BIN)"

# Follow event_wait into real Grifo's clock-gated sleep, then wake by button.
.PHONY: test-suspend
test-suspend: wremu
	python3 tools/test_suspend.py --toolchain "$(C33_BIN)"

# CMU protect gate and the 60 MHz the timebase depends on.
test-cmu: tools/test_cmu.c src/cmu.o src/mem.o src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_cmu tools/test_cmu.c src/cmu.o src/mem.o src/model.o
	/tmp/test_cmu

# Cascaded timer 0/5 reads and the per-channel count-pause register.
test-timer: tools/test_timer.c src/timer.o $(C33_OBJS) src/model.o src/itc.o src/cmu.o src/mem.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_timer tools/test_timer.c src/timer.o $(C33_OBJS) src/model.o src/itc.o src/cmu.o src/mem.o
	/tmp/test_timer

# SDRAM controller reset values, status flags, and register masks.
test-sdramc: tools/test_sdramc.c src/sdramc.o src/model.o src/mem.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_sdramc tools/test_sdramc.c src/sdramc.o src/model.o src/mem.o
	/tmp/test_sdramc

.PHONY: test-sdramc-fetch
test-sdramc-fetch: tools/test_sdramc_fetch.c src/sdramc.c src/sdramc.h $(C33_OBJS) src/model.o src/mem.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_sdramc_fetch tools/test_sdramc_fetch.c $(C33_OBJS) src/model.o src/mem.o
	/tmp/test_sdramc_fetch

# Fixed CPU/product/model/version bytes at the documented identification area.
test-chipid: tools/test_chipid.c src/mem.o src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_chipid tools/test_chipid.c src/mem.o src/model.o
	/tmp/test_chipid

# GPIO reset masks and the modeled key/power interrupt inputs.
test-port: tools/test_port.c src/port.o src/eeprom.o src/itc.o $(C33_OBJS) src/model.o src/mem.o src/sdramc.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_port tools/test_port.c src/port.o src/eeprom.o src/itc.o $(C33_OBJS) src/model.o src/mem.o src/sdramc.o
	/tmp/test_port

# Documented SPI DMA pipeline, including representative failure modes.
test-dma: tools/test_dma.c src/dma.o src/sdcard.o src/cmu.o src/itc.o src/mem.o src/port.o src/eeprom.o $(C33_OBJS) src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_dma tools/test_dma.c src/dma.o src/sdcard.o src/cmu.o src/itc.o src/mem.o src/port.o src/eeprom.o $(C33_OBJS) src/model.o
	/tmp/test_dma

.PHONY: test-cpu-dma-order
test-cpu-dma-order: tools/test_cpu_dma_order.c src/dma.o src/sdcard.o src/sdramc.o src/cmu.o src/itc.o src/mem.o src/port.o src/eeprom.o $(C33_OBJS) src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_cpu_dma_order $^ $(SDL_LIBS)
	/tmp/test_cpu_dma_order

.PHONY: test-fetch-accounting
test-fetch-accounting: tools/test_fetch_accounting.c $(C33_OBJS) src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_fetch_accounting $^
	/tmp/test_fetch_accounting

.PHONY: test-idma-deadline
test-idma-deadline: test-dma-timeline

.PHONY: test-dma-timeline
test-dma-timeline: tools/test_dma_timeline.c src/dma.o src/sdcard.o src/cmu.o src/itc.o src/mem.o src/port.o src/eeprom.o $(C33_OBJS) src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_dma_timeline $^ $(SDL_LIBS)
	/tmp/test_dma_timeline

# Software-triggered memory DMA, independent of the fitted SPI allowance.
.PHONY: test-mem-dma
test-mem-dma: tools/test_mem_dma.c src/dma.o src/sdcard.o src/cmu.o src/itc.o src/mem.o src/port.o src/eeprom.o $(C33_OBJS) src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_mem_dma tools/test_mem_dma.c src/dma.o src/sdcard.o src/cmu.o src/itc.o src/mem.o src/port.o src/eeprom.o $(C33_OBJS) src/model.o
	/tmp/test_mem_dma

# Requires the built Grifo kernel, libc and the local C33 toolchain.
.PHONY: test-zim-copy
test-zim-copy: wremu
	python3 tools/test_zim_copy.py
	python3 tools/test_zim_copy.py --fault

# Execute the production C33 backend, including failed DMA recovery. Requires
# the local C33 toolchain; the test creates its own one-sector card image.
.PHONY: test-sd-dma-driver
test-sd-dma-driver: wremu
	python3 tools/test_sd_dma_driver.py --bits 8 $(if $(TOOLCHAIN_BIN),--toolchain "$(TOOLCHAIN_BIN)")
	python3 tools/test_sd_dma_driver.py --bits 32 $(if $(TOOLCHAIN_BIN),--toolchain "$(TOOLCHAIN_BIN)")
	python3 tools/test_sd_dma_driver.py --bits 8 --tx hsdma $(if $(TOOLCHAIN_BIN),--toolchain "$(TOOLCHAIN_BIN)")
	python3 tools/test_sd_dma_driver.py --bits 32 --tx hsdma $(if $(TOOLCHAIN_BIN),--toolchain "$(TOOLCHAIN_BIN)")

# The GUI build booted the device's way, through MBR, the boot loader, grifo
# and the launcher menu, on a generated card (tools/test_gui_boot.py).
test-gui-boot: wremu
	python3 tools/test_gui_boot.py

# Differential test against the real cross compiler: the same C source
# compiled by c33-epson-elf-gcc 3.3.2 and by the host compiler, run both
# ways, outputs diffed. Needs host-tools/toolchain-install.
difftest: wremu
	difftest/run.sh 1 20

# Decode table against the documented encodings in the C33 PE Core manual,
# an authority independent of binutils. Needs the PDF and pdftotext.
test-manual:
	python3 tools/check_manual_encodings.py ../s1c33.pdf

# Optional third decoder oracle.  GMMan's source must first be compiled with
# Ghidra's support/sleigh tool, and Python needs the pypcode package.
S1C33_SLEIGH ?= ../s1c33_sleigh
test-sleigh:
	python3 tools/check_sleigh.py "$(S1C33_SLEIGH)"

# Everything that can be checked without hardware.
check: test-frontend-cpu test-frontend-native test-dma-write-deadline test-dma-timeline test-cpu-dma-order test-fetch-accounting test-a0-timing test-score-model test-uart test test-isa test-irq test-itc-select test-exception test-lcd test-adc test-cmu test-timer test-sdramc test-sdramc-fetch test-chipid test-port test-display test-sd test-sd-timing test-fat32-fixture test-mmc-power test-event-wait test-wdt test-wdt-reset test-dma test-gui-boot

.PHONY: test-frontend-cpu
check: test-sdramc-deferred test-suspend

.PHONY: test-sdramc-deferred
test-sdramc-deferred: tools/test_sdramc_deferred.c src/mem.o src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_sdramc_deferred $^
	/tmp/test_sdramc_deferred

test-frontend-cpu: tools/test_frontend_cpu.c $(C33_OBJS) src/mem.o src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_frontend_cpu $^
	/tmp/test_frontend_cpu

.PHONY: test-frontend-native
test-frontend-native: tools/test_frontend_native.c tools/test_frontend_native.py src/frontend.c src/frontend.h
	$(CC) $(TEST_CFLAGS) -o /tmp/test_frontend_native tools/test_frontend_native.c src/frontend.c
	python3 tools/test_frontend_native.py /tmp/test_frontend_native

.PHONY: test-dma-write-deadline
test-dma-write-deadline: tools/test_dma_write_deadline.c $(C33_OBJS) src/mem.o src/model.o src/cmu.o src/itc.o src/port.o src/sdcard.o src/eeprom.o src/dma.o src/sdramc.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_dma_write_deadline $^ $(SDL_LIBS)
	/tmp/test_dma_write_deadline

.PHONY: test-a0-timing
test-a0-timing: tools/test_a0_timing.c src/mem.o src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_a0_timing $^
	/tmp/test_a0_timing

.PHONY: test-score-model
test-score-model:
	python3 tools/test_score_model.py
	python3 tools/test_accuracy_report.py
	python3 -m unittest discover -s tools/port_bench -p 'test_*.py'
	python3 -m unittest discover -s tools/fetch_bench -p 'test_*.py'
	python3 -m unittest discover -s tools/experiments -p test_frontend.py
	python3 -m unittest discover -s tools/experiments -p test_port_predictions.py
	python3 tools/bus_bench/test_collect.py
	python3 tools/workflow_bench/test_report.py

clean:
	rm -f wremu $(OBJS) $(OBJS:.o=.d) .build-flags .build-flags.tmp

.PHONY: test-uart
test-uart: tools/test_uart.c src/uart.o src/cmu.o src/itc.o src/mem.o src/model.o
	$(CC) $(TEST_CFLAGS) -o /tmp/test_uart $^ $(SDL_LIBS)
	/tmp/test_uart
