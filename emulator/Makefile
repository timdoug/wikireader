# wremu -- WikiReader (Epson C33) full-system emulator

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter

# SDL2 for the live panel window
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
CFLAGS     += $(SDL_CFLAGS)
OBJS     = src/c33.o src/mem.o src/elf.o src/uart.o src/sdcard.o src/periph.o src/lcd.o src/display.o src/touch.o src/timer.o src/wdt.o src/itc.o src/cmu.o src/port.o src/eeprom.o src/sdramc.o src/dma.o src/model.o src/main.o

.PHONY: all clean tables test test-isa test-irq test-exception test-lcd test-adc test-cmu test-timer test-sdramc test-chipid test-port test-dma test-gui-dma test-gui-boot check difftest test-manual

all: wremu

wremu: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(SDL_LIBS)

# Header dependencies come from the compiler. They used to be a hand-written
# list of every header, which is a list that goes stale: wdt.h was missing
# from it, so changing a struct layout left objects compiled against the old
# one and main.c read a different field than the device wrote. Nothing warns
# about that -- the numbers are just wrong.
src/%.o: src/%.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

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
	    $(CC) $(CFLAGS) -o /tmp/test_decode tools/test_decode.c && \
	    cat ref-*.txt | /tmp/test_decode ; \
	fi

# Interrupt priority (PSR.IL) rules, per the C33 PE Core manual.
test-irq: tools/test_irq.c src/c33.o src/model.o src/itc.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_irq tools/test_irq.c src/c33.o src/model.o src/itc.o src/mem.o
	/tmp/test_irq

# Core instruction semantics not exercised by the firmware images.
test-isa: tools/test_isa.c src/c33.o src/model.o
	$(CC) $(CFLAGS) -o /tmp/test_isa tools/test_isa.c src/c33.o src/model.o
	/tmp/test_isa

# Synchronous exception entry, saved PCs, IDIR, and PE-removed opcodes.
test-exception: tools/test_exception.c src/c33.o src/model.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_exception tools/test_exception.c src/c33.o src/model.o
	/tmp/test_exception

# LCD sub-window (PIP) compositing, against grifo's own window arithmetic.
test-lcd: tools/test_lcd.c src/lcd.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_lcd tools/test_lcd.c src/lcd.o src/mem.o $(SDL_LIBS)
	/tmp/test_lcd

# ADC scaling, checked by running grifo's own conversions over the counts.
test-adc: tools/test_adc.c src/periph.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_adc tools/test_adc.c src/periph.o src/mem.o
	/tmp/test_adc

# Window events: the path from an SDL click to the touch panel, which
# scripted runs bypass entirely by calling touch_post() directly.
test-display: tools/test_display.c src/display.o src/lcd.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_display tools/test_display.c src/display.o src/lcd.o src/mem.o $(SDL_LIBS)
	/tmp/test_display

# SD writes: the path that makes the guest's history outlive a run.
test-sd: tools/test_sd.c src/sdcard.o src/mem.o src/port.o src/eeprom.o src/itc.o src/c33.o src/model.o
	$(CC) $(CFLAGS) -o /tmp/test_sd tools/test_sd.c src/sdcard.o src/mem.o src/port.o src/eeprom.o src/itc.o src/c33.o src/model.o $(SDL_LIBS)
	/tmp/test_sd

.PHONY: test-sd-timing
test-sd-timing: tools/test_sd_timing.c src/sdcard.o src/mem.o src/port.o src/eeprom.o src/itc.o src/c33.o src/model.o
	$(CC) $(CFLAGS) -o /tmp/test_sd_timing $^ $(SDL_LIBS)
	/tmp/test_sd_timing

.PHONY: test-fat32-fixture
test-fat32-fixture:
	python3 tools/test_fat32_fixture.py

.PHONY: test-mmc-power
test-mmc-power:
	$(CC) $(CFLAGS) -Itools/mmc_mock -I../samo-lib/drivers/include \
		-I../samo-lib/fatfs/src -I../samo-lib/fatfs/config/c33/read-write \
		-o /tmp/test_mmc_power tools/test_mmc_power.c
	/tmp/test_mmc_power

# Timed event waits use the real kernel queue with deterministic IRQ races.
.PHONY: test-event-wait
test-event-wait:
	$(CC) $(CFLAGS) -I../samo-lib/grifo/common \
		-o /tmp/test_event_wait tools/test_event_wait.c
	/tmp/test_event_wait

# Watchdog, and above all the clock gate that keeps it quiet in suspend.
test-wdt: tools/test_wdt.c src/wdt.o src/cmu.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_wdt tools/test_wdt.c src/wdt.o src/cmu.o src/mem.o $(SDL_LIBS)
	/tmp/test_wdt

# CMU protect gate and the 60 MHz the timebase depends on.
test-cmu: tools/test_cmu.c src/cmu.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_cmu tools/test_cmu.c src/cmu.o src/mem.o
	/tmp/test_cmu

# Cascaded timer 0/5 reads and the per-channel count-pause register.
test-timer: tools/test_timer.c src/timer.o src/c33.o src/model.o src/itc.o src/cmu.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_timer tools/test_timer.c src/timer.o src/c33.o src/model.o src/itc.o src/cmu.o src/mem.o
	/tmp/test_timer

# SDRAM controller reset values, status flags, and register masks.
test-sdramc: tools/test_sdramc.c src/sdramc.o src/model.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_sdramc tools/test_sdramc.c src/sdramc.o src/model.o src/mem.o
	/tmp/test_sdramc

# Fixed CPU/product/model/version bytes at the documented identification area.
test-chipid: tools/test_chipid.c src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_chipid tools/test_chipid.c src/mem.o
	/tmp/test_chipid

# GPIO reset masks and the modeled key/power interrupt inputs.
test-port: tools/test_port.c src/port.o src/itc.o src/c33.o src/model.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_port tools/test_port.c src/port.o src/itc.o src/c33.o src/model.o src/mem.o
	/tmp/test_port

# Documented SPI DMA pipeline, including representative failure modes.
test-dma: tools/test_dma.c src/dma.o src/sdcard.o src/cmu.o src/itc.o src/mem.o src/port.o src/eeprom.o src/c33.o src/model.o
	$(CC) $(CFLAGS) -o /tmp/test_dma tools/test_dma.c src/dma.o src/sdcard.o src/cmu.o src/itc.o src/mem.o src/port.o src/eeprom.o src/c33.o src/model.o
	/tmp/test_dma

# Software-triggered memory DMA, independent of the fitted SPI allowance.
.PHONY: test-mem-dma
test-mem-dma: tools/test_mem_dma.c src/dma.o src/sdcard.o src/cmu.o src/itc.o src/mem.o src/port.o src/eeprom.o src/c33.o src/model.o
	$(CC) $(CFLAGS) -o /tmp/test_mem_dma tools/test_mem_dma.c src/dma.o src/sdcard.o src/cmu.o src/itc.o src/mem.o src/port.o src/eeprom.o src/c33.o src/model.o
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

# Direct kernel boot reaches init promptly even though the CPU sleeps between
# SPI DMA bytes. This catches accidentally stretching each byte to a GUI poll.
# images/ holds a card image and kernel that are built locally, not shipped, so
# skip where they are absent rather than failing a fresh checkout.
test-gui-dma: wremu
	@if [ ! -f images/wrcard.img ] || [ ! -f images/grifo.elf ] ; then \
	    echo "test-gui-dma: no images/wrcard.img or images/grifo.elf, skipping" ; \
	else \
	    SDL_VIDEODRIVER=dummy ./wremu -g -R -N 3,1000000 -n 5000000 \
		-c images/wrcard.img images/grifo.elf > /tmp/test_gui_dma.log 2>&1 && \
	    grep -q 'init starting' /tmp/test_gui_dma.log ; \
	fi

# Exercise the same boot chain used by the live GUI. Continue through the
# loader/kernel handoff and the kernel's first DMA reads: SDRAM descriptor
# traffic can move MCLK past an SPI deadline, which must not add a GUI poll
# delay to every byte.
test-gui-boot: wremu
	@if [ ! -f images/wrcard.img ] || [ ! -f ../samo-lib/mbr/flash.rom ] ; then \
	    echo "test-gui-boot: no images/wrcard.img or mbr/flash.rom, skipping" ; \
	else \
	    SDL_VIDEODRIVER=dummy ./wremu -g -R -N 3,1000000 -n 25000000 \
		-e ../samo-lib/mbr/flash.rom -c images/wrcard.img \
		> /tmp/test_gui_boot.log 2>&1 && \
	    grep -q 'Grifo starting' /tmp/test_gui_boot.log && \
	    grep -q 'init starting' /tmp/test_gui_boot.log && \
	    grep -Eq -- '--- lcd: [1-9][0-9]* register writes' /tmp/test_gui_boot.log && \
	    grep -Eq -- '--- display: [0-9]+ update calls, [2-9][0-9]* presents' \
		/tmp/test_gui_boot.log ; \
	fi

# Differential test against the real cross compiler: the same C source
# compiled by c33-epson-elf-gcc 3.3.2 and by the host compiler, run both
# ways, outputs diffed. Needs host-tools/toolchain-install.
difftest: wremu
	difftest/run.sh 1 20

# Decode table against the documented encodings in the C33 PE Core manual,
# an authority independent of binutils. Needs the PDF and pdftotext.
test-manual:
	python3 tools/check_manual_encodings.py ../s1c33.pdf

# Everything that can be checked without hardware.
check: test-uart test test-isa test-irq test-exception test-lcd test-adc test-cmu test-timer test-sdramc test-chipid test-port test-display test-sd test-sd-timing test-fat32-fixture test-mmc-power test-event-wait test-wdt test-dma test-gui-dma test-gui-boot

clean:
	rm -f wremu $(OBJS) $(OBJS:.o=.d)

.PHONY: test-uart
test-uart: tools/test_uart.c src/uart.o src/itc.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_uart $^ $(SDL_LIBS)
	/tmp/test_uart
