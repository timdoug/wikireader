# wremu -- WikiReader (Epson C33) full-system emulator

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter

# SDL2 for the live panel window
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
CFLAGS     += $(SDL_CFLAGS)
OBJS     = src/c33.o src/mem.o src/elf.o src/uart.o src/sdcard.o src/periph.o src/lcd.o src/display.o src/touch.o src/timer.o src/itc.o src/main.o

.PHONY: all clean tables test test-irq test-lcd test-adc

all: wremu

wremu: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(SDL_LIBS)

# Header deps matter here: struct c33 is shared between translation units, so
# a stale object compiled against an older layout silently corrupts state.
src/%.o: src/%.c src/c33.h src/mem.h src/uart.h src/sdcard.h src/periph.h src/lcd.h src/display.h src/touch.h src/timer.h src/itc.h c33_forms.h
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

clean:
	rm -f wremu $(OBJS)
