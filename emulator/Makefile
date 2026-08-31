# wremu -- WikiReader (Epson C33) full-system emulator

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter

# SDL2 for the live panel window
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
CFLAGS     += $(SDL_CFLAGS)
OBJS     = src/c33.o src/mem.o src/elf.o src/uart.o src/sdcard.o src/periph.o src/lcd.o src/display.o src/touch.o src/timer.o src/wdt.o src/itc.o src/cmu.o src/port.o src/eeprom.o src/sdramc.o src/main.o

.PHONY: all clean tables test test-isa test-irq test-exception test-lcd test-adc test-cmu test-timer test-sdramc test-port check difftest test-manual

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
test: c33_forms.h
	$(CC) $(CFLAGS) -o /tmp/test_decode tools/test_decode.c
	cat ref-*.txt | /tmp/test_decode

# Interrupt priority (PSR.IL) rules, per the C33 PE Core manual.
test-irq: tools/test_irq.c src/c33.o src/itc.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_irq tools/test_irq.c src/c33.o src/itc.o src/mem.o
	/tmp/test_irq

# Core instruction semantics not exercised by the firmware images.
test-isa: tools/test_isa.c src/c33.o
	$(CC) $(CFLAGS) -o /tmp/test_isa tools/test_isa.c src/c33.o
	/tmp/test_isa

# Synchronous exception entry, saved PCs, IDIR, and PE-removed opcodes.
test-exception: tools/test_exception.c src/c33.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_exception tools/test_exception.c src/c33.o
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
test-sd: tools/test_sd.c src/sdcard.o src/mem.o src/port.o src/eeprom.o src/itc.o src/c33.o
	$(CC) $(CFLAGS) -o /tmp/test_sd tools/test_sd.c src/sdcard.o src/mem.o src/port.o src/eeprom.o src/itc.o src/c33.o $(SDL_LIBS)
	/tmp/test_sd

# Watchdog, and above all the clock gate that keeps it quiet in suspend.
test-wdt: tools/test_wdt.c src/wdt.o src/cmu.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_wdt tools/test_wdt.c src/wdt.o src/cmu.o src/mem.o $(SDL_LIBS)
	/tmp/test_wdt

# CMU protect gate and the 60 MHz the timebase depends on.
test-cmu: tools/test_cmu.c src/cmu.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_cmu tools/test_cmu.c src/cmu.o src/mem.o
	/tmp/test_cmu

# Cascaded timer 0/5 reads and the per-channel count-pause register.
test-timer: tools/test_timer.c src/timer.o src/c33.o src/itc.o src/cmu.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_timer tools/test_timer.c src/timer.o src/c33.o src/itc.o src/cmu.o src/mem.o
	/tmp/test_timer

# SDRAM controller reset values, status flags, and register masks.
test-sdramc: tools/test_sdramc.c src/sdramc.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_sdramc tools/test_sdramc.c src/sdramc.o src/mem.o
	/tmp/test_sdramc

# GPIO reset masks and the modeled key/power interrupt inputs.
test-port: tools/test_port.c src/port.o src/itc.o src/c33.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_port tools/test_port.c src/port.o src/itc.o src/c33.o src/mem.o
	/tmp/test_port

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
check: test test-isa test-irq test-exception test-lcd test-adc test-cmu test-timer test-sdramc test-port test-display test-sd test-wdt

clean:
	rm -f wremu $(OBJS) $(OBJS:.o=.d)
