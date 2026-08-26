# wremu -- WikiReader (Epson C33) full-system emulator

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter

# SDL2 for the live panel window
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
CFLAGS     += $(SDL_CFLAGS)
OBJS     = src/c33.o src/mem.o src/elf.o src/uart.o src/sdcard.o src/periph.o src/lcd.o src/display.o src/touch.o src/timer.o src/itc.o src/cmu.o src/port.o src/eeprom.o src/sdramc.o src/main.o

.PHONY: all clean tables test test-irq test-lcd test-adc test-cmu check difftest test-manual

all: wremu

wremu: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(SDL_LIBS)

# Header deps matter here: struct c33 is shared between translation units, so
# a stale object compiled against an older layout silently corrupts state.
src/%.o: src/%.c src/c33.h src/mem.h src/uart.h src/sdcard.h src/periph.h src/lcd.h src/display.h src/touch.h src/timer.h src/itc.h src/cmu.h src/port.h src/eeprom.h src/sdramc.h c33_forms.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Regenerate the decode tables from binutils' disassembler. Requires
# allinsn_full.txt, produced by disassembling all 65536 encodings with
# c33-epson-elf-objdump (see tools/gen_forms.py).
tables: allinsn_full.txt
	python3 tools/gen_forms.py allinsn_full.txt c33_forms.h

# Check the generated tables still agree with binutils on real firmware.
test: c33_forms.h
	$(CC) $(CFLAGS) -o /tmp/test_decode tools/test_decode.c
	cat ref-*.txt | /tmp/test_decode

# Interrupt priority (PSR.IL) rules, per the C33 PE Core manual.
test-irq: tools/test_irq.c src/c33.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_irq tools/test_irq.c src/c33.o
	/tmp/test_irq

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

# CMU protect gate and the 60 MHz the timebase depends on.
test-cmu: tools/test_cmu.c src/cmu.o src/mem.o
	$(CC) $(CFLAGS) -o /tmp/test_cmu tools/test_cmu.c src/cmu.o src/mem.o
	/tmp/test_cmu

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
check: test test-irq test-lcd test-adc test-cmu test-display test-sd

clean:
	rm -f wremu $(OBJS)
