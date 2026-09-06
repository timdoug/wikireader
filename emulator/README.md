# wremu - WikiReader full-system emulator

`wremu` boots the WikiReader's Epson S1C33E07 firmware from ELF files or
through the serial-FLASH boot chain, attaches a FAT32 card image, and presents
the 240x208 touch display through SDL2.

The combined emulator, toolchain, firmware, and next-work status is in
[`../host-tools/toolchain-c33/HANDOFF.md`](../host-tools/toolchain-c33/HANDOFF.md).

## Status

- The mask-ROM effect, MBR, menu, file-loader, kernel, `init.app`, and
  `wiki.app` boot from the current FLASH and card images.
- Shipped GCC 3.3.2 and current GCC 16.2 firmware reach matching UI, search,
  article, and scrolling framebuffers for the exercised workflows.
- Kernel block reads use the documented SPI HSDMA/IDMA pipeline and wait in
  HALT for the channel-3 terminal-count cause. Pre-kernel reads and all card
  writes remain PIO.
- `make check` covers the decoder, core ISA, exceptions, interrupts, LCD,
  display input, SD, DMA, clocks, ADC, timers, watchdog, SDRAM, GPIO, and chip
  identification.
- Firmware and differential programs retire 57 of the 74 implemented PE
  operations. Focused core tests cover most remaining forms.
- Headless execution is deterministic and runs at about 80 million target
  instructions per host second.

Timing is derived from the documented 60 MHz MCLK and the programmed SPI,
DMA, timer, and SDRAM registers. Absolute SD-card latency and cross-bank
SDRAM overlap still require hardware calibration.

## Build and test

```sh
cd emulator
make
make check
```

SDL2 is required for the window (`brew install sdl2`). Generated C33 decode
tables are committed, so the cross-compiler is not required for a normal
emulator build.

`make check` runs all focused model tests. `make difftest` runs the generated
native-versus-C33 execution comparison; see
[`difftest/README.md`](difftest/README.md).

## Run

Direct kernel ELF boot is convenient for CPU and firmware debugging, but it
skips the file-loader's peripheral handoff:

```sh
./wremu -c images/wrcard.img images/grifo.elf
```

Use the serial-FLASH boot chain for the live panel. It performs the real LCD
controller initialization before loading the kernel:

```sh
./wremu -g -N 3,1000000 -e ../samo-lib/mbr/flash.rom \
    -c images/wrcard.img
```

With a window, the device initially appears powered off. Press `P` or click
the power symbol. Click the touch panel, use `1`, `2`, and `3` for the random,
search, and history buttons, and use `Q` or `Esc` to quit.

Typical boot output is:

```text
load: kernel.elf
Grifo starting
init starting
starting wiki app
VERSION: 20260823
```

Typing `LOVE` on the keyboard returns article titles from the attached wiki
data.

### Useful options

| Option | Purpose |
| --- | --- |
| `-g`, `-S N` | Open the SDL window at scale `N` (default 3). |
| `-c FILE` | Attach a FAT32 card image. |
| `-R` | Keep the card image read-only. |
| `-e FILE` | Attach serial FLASH and use the hardware boot path. |
| `-n N` | Stop after `N` target cycles/instructions; the GUI defaults to unlimited. |
| `-s` | Trace grifo syscalls with call sites and return values. |
| `-K cycle,TEXT` | Type text on the on-screen keyboard. |
| `-T x,y,cycle` | Tap a pixel; repeatable, up to eight. |
| `-G x,y0,y1,cycle` | Drag vertically; repeatable, up to eight. |
| `-N code,cycle` | Press random/search/history/power (`0`-`3`); repeatable, up to eight. |
| `-b ADDR`, `-W ADDR` | Break on execution or a write. |
| `-D ADDR -L N -O FILE` | Dump target memory. |
| `-m` | Trace unclaimed MMIO accesses. |
| `-P` | Report retired-opcode counts. |
| `-H` | Profile executed addresses. |
| `-X ADDR[,NAME]` | Count entries and report the longest gaps. |
| `-Y A,B`, `-y M,N` | Limit profiling by address or guest-time interval. |
| `-F FILE` | Write non-empty profile buckets: address, instructions, MCLK cycles, cycles waiting for the fetch, SDRAM row activations. |
| `-Z ADDR` | Rebase the scripted input timeline on the first hit of `ADDR`. |

`WREMU_SDRAM_TIMING=tRP,tRAS,tRC` (clocks) and `WREMU_SDRAM_AURCO=N`
replace every firmware write of the SDRAM controller's timing and refresh
registers, so one image can be timed under the boot loader's stock values
(`4,8,15` and `0x8c`) or the kernel's retimed ones (`2,4,6` and `0x120`).
`WREMU_SUSPEND_DIV=N` shortens the firmware's 120-second suspend interval
for testing without modifying the guest. `WREMU_HOLD_MS=N` changes how long
scripted taps and presses are held before release (default 33 ms).

The window has its own input path (SDL events, wall-clock timers, a power-on
reset when the device is switched on), so a bug seen only with the mouse may
not reproduce under `-T`/`-K`. `WREMU_GUI_CLICKS="x,y,at_ms[,hold_ms];..."`
pushes synthetic left clicks through that path at wall-clock milliseconds
after the window opens, in panel pixels; an item of the form
`d:x,y0,y1,at_ms[,ms]` is a drag from `y0` to `y1` taking `ms`. Combine with
`-g -N 3,1000000` to switch the device on and `-n` to end the run and write
`screen.pgm`. Scripted `-N` presses are timed in guest cycles, which advance
at roughly a third of wall-clock rate while the window idles.
`WREMU_TOUCH_TRACE=1` logs each packet the window hands to the touch panel
and each scripted drag step, and `WREMU_WALLCLOCK=1` gives a headless run the
window's wall-clock tick. `WREMU_DRAG_MS=N` spaces the sixteen steps of a
scripted `-G` drag N units apart instead of 5 (the unit is the `-G` cycle
count, which is instructions retired, so the guest time depends on the
firmware's work per instruction). `WREMU_LCD_TRACE=1` logs every write of the
LCD controller's framebuffer address with the MCLK time; while the firmware
scrolls by repointing that address, this is one line per displayed frame and
the cleanest way to measure scrolling frame rate, since it costs the guest
nothing (a serial trace inside the firmware stalls it for tens of ms a line).

### Benchmarking

Use `-Z` to align scripted input to a guest milestone. Absolute `-T`/`-K`
cycle numbers do not produce comparable interactions when two firmware builds
reach the UI at different rates. Use `-Y` or `-y` to exclude idle polling
from profiles.

A `-Y` window also reports the SDRAM controller's work inside it: wait
cycles, refreshes, queue hits and misses, and row activations by bank, by
access kind per bank, and by the kinds of the two accesses on either side of
each activation (`--- window sdram`, `--- window activations by bank`, and
the following lines). The `-F` profile carries cycles and activations per
2-byte bucket, so `addr2line` on an unstripped link turns it into a
per-source-line cost that includes memory stalls. Cycles, not instruction
counts, are what to look at on this core: the ZIM article load below runs at
3.5 to 4.7 cycles per instruction, and an instruction-count profile of it
ranks the wrong lines. The CPU has no cache; the model charges a row change
of about 2 tRP + waits on tRAS/tRC for every move to another 1 KiB row of a
bank (a bank is a contiguous quarter of the SDRAM), closes every bank at each
auto-refresh, and treats the two-slot 16-byte instruction queue as the only
fetch buffering.

ZIM reader article load, Simple English `Cat`, tap to first painted page,
run from the repository root with a card from `zim/make-card-image`:

```sh
./emulator/wremu -R -e samo-lib/mbr/flash.rom -c /tmp/card.dmg \
    -T 40,36,100000000 -K 300000000,CAT -T 30,40,500000000 \
    -Y 0x<retrieve_article>,0x<render_article_with_pcf> -F prof.txt -n 1000000000
```

(addresses from `zim/zim.map`; the first tap picks the reader on the
launcher menu, so `-Z` cannot be used):

| Firmware | Window | Instructions | Row activations |
| --- | ---: | ---: | ---: |
| 2026-09-05 morning, stock SDRAM timing | 1941.3 ms | 25,065,381 | 3,933,216 |
| software changes only, stock SDRAM timing forced | 1557.5 ms | 22,352,859 | |
| software changes and the kernel's SDRAM retiming | 1296.0 ms | 22,365,009 | 2,384,163 |

SD block payloads fell from 52,300 to 22,900 modeled cycles once the DMA
descriptor and dummy byte left SDRAM, and the first Wikivoyage `Paris`
photograph from 1342.5 to 1084.0 ms on the retiming alone. Scripted tap
release is delivered only when the emulator next idles, so a faster build can
show more `render_article_with_pcf` calls after the page appears; that is the
held touch, not extra work.

The summary separates executed instructions from fast-forwarded idle cycles:

```text
--- work: 219633709 instructions executed, 180366291 idle, 7973.0 ms guest ---
```

Current matched measurements are:

| Firmware/path | First stable screen | Executed work |
| --- | ---: | ---: |
| shipped GCC 3.3.2, PIO | 2363.4 ms | 61,364,514 |
| GCC 16.2, PIO | 2370.6 ms | 56,406,351 |
| GCC 16.2, DMA | 2374.3 ms | 40,340,531 |

The screen is held by a deliberate two-second firmware deadline, so DMA's
28.5% work reduction becomes idle time. The earlier first
`File_initialise` milestone is 905.9 ms, 893.6 ms, and 773.0 ms respectively.

Opening the first result for `LOVE` is a sustained-I/O comparison. Both
modern paths read the same 395 blocks:

| Kernel path | Article data and LZMA | Executed work |
| --- | ---: | ---: |
| PIO | 3831.4 ms | 64,395,028 |
| DMA | 3629.3 ms | 55,589,491 |

DMA saves 202.1 modeled ms (5.3%) and 13.7% of instructions. The final PIO,
DMA, and shipped-application framebuffers are byte-identical. The shipped
application is not a clean article timing baseline because it performs two
unmapped reads and 256 writes immediately above DSTRAM during this operation;
current firmware performs none.

The production DMA path sleeps until HSDMA3 reports terminal count instead of
polling its enable bit. In a fixed 300-million-cycle boot/search/article run,
that reduces executed work from 160,069,719 to 151,391,234 instructions and
modeled time from 6094.8 to 6015.4 ms. Both paths read 915 blocks, perform the
same 468,480 HSDMA and 467,565 IDMA transfers, and produce identical screens.

A separate clean full-FLASH A/B rebuilt the whole GCC 16 runtime stack at
`-O2` or `-Os`; the size-constrained MBR/menu/file-loader stayed at `-Os` in
both images. The installed file sizes are:

| Runtime file | `-O2` | `-Os` | `-Os` change |
| --- | ---: | ---: | ---: |
| `kernel.elf` | 49,876 B | 43,408 B | -13.0% |
| `init.app` | 2,368 B | 2,208 B | -6.8% |
| `wiki.app` | 162,928 B | 150,048 B | -7.9% |
| total | 215,172 B | 195,664 B | -9.1% |

The same `LOVE` workflow produced:

| Metric | `-O2` | `-Os` | `-Os` change |
| --- | ---: | ---: | ---: |
| reset to wiki main loop | 786.7 ms | 798.3 ms | +1.5% |
| article interval, instructions | 55,588,282 | 52,724,316 | -5.2% |
| article interval, modeled time | 3680.09 ms | 3796.75 ms | +3.2% |
| reset to article completion, instructions | 136,677,103 | 132,298,914 | -3.2% |
| reset to article completion, modeled time | 11860.6 ms | 12057.8 ms | +1.7% |
| SDRAM wait cycles | 343,337,791 | 361,692,295 | +5.3% |

Both variants repeated cycle-for-cycle and produced the same final framebuffer
(SHA-256 `5d024db6c27fd91b88099d21a002077b2d876893ba1af4a1dd07f8f2424529f0`).
`-Os` retires fewer instructions, but its extra modeled SDRAM/bus stalls make
the article interval 3.2% slower, so `-O2` remains the runtime default. Treat
that small ordering as medium confidence until physical timing calibration.

Build the matched modern kernel paths with `SD_DMA=YES` (default) or
`SD_DMA=NO`. Firmware Makefiles default to the original compiler, so select
the modern prefix explicitly when required:

```sh
make TOOLCHAIN_BIN="$(pwd)/host-tools/toolchain-c33/work/install/bin" \
    SD_DMA=YES <target>
```

## Hardware model

### CPU and memory

The core implements the documented C33 PE instruction set, `ext` composition,
delay slots, condition flags, special registers, strict natural alignment,
synchronous exceptions, debug exceptions, and prioritized interrupts. Cold
reset establishes TTBR `0x00c00000`, a PE IDIR type byte of `0x06`, and DBBR
`0x00060000`.

The generated decoder contains the union of Standard, Advanced, and PE
binutils tables. A separate PE-valid bitmap rejects the nine Standard
instructions removed from PE and the 18 Advanced-only operations. The
coprocessor forms are intentionally absent because the S1C33E07 has no
attached coprocessor.

SDRAM timing is active after firmware programs `SDON`, MRS, and `APPON`.
The model derives geometry and waits from SDRAMC registers, tracks active
rows, queue buffers, refresh, and self-refresh, and puts CPU and DMA accesses
on a shared MCLK timeline. It conservatively serializes command and data
phases across banks instead of modeling all documented bank interleaving.

### Storage and DMA

The SD card operates in SPI mode. Character completion follows live `BPT`,
`MCBR`, and `SPI_WAIT` values and updates `BSYF`, `TDEF`, `RDFF`, and `RDOF`
at the scheduled event.

The production read backend uses HSDMA channel 3 for SPI RX and IDMA channel
`0x24` to write dummy TX bytes. The model covers the dual-address, byte-wide,
single-transfer behavior used by firmware: request selection, priority,
counters, address updates, terminal enable clearing, descriptor writeback,
clock gating, global IDMA enable, and the terminal interrupt cause. One
512-byte block performs 512 HSDMA and 511 IDMA transfers. Unused DMA modes and
trigger sources are not modeled.

Known divergence: the model lets the channel-3 terminal-count cause wake a
HALTed core. A real WikiReader (stock 2009 flash) never woke, and a kernel
that slept on that cause hung on the boot splash. The kernel now polls the
flag, which works on both. Do not rely on the emulator to tell you which
interrupt causes wake HALT.

### Peripherals

The current model includes:

- interrupt-controller priorities and read/modify/write register behavior;
- LCD control, framebuffer capture, and SDL touch/front-button input;
- EFSIF UART, SPI status, and serial FLASH;
- GPIO selection, polarity, edge/level input, and the board's power control;
- six T16 channels, prescalers, comparison buffering, interrupts, and the
  timer-0-to-timer-5 cascade used by firmware;
- CMU protection, clock gates, oscillator/divider decoding, and derived MCLK;
- ADC sweep/status behavior with physically plausible board values;
- watchdog reset; and
- the S1C33E07 fixed chip-identification bytes.

The GUI uses wall-clock time for human input. Headless runs use deterministic
instruction- and event-derived guest time and fast-forward blocked intervals.

The RTC, unused alternate pins, unconnected inputs, unused timer waveforms,
and unused DMA triggers are intentionally out of scope because WikiReader
firmware does not use them.

## Validation basis

The model is checked against sources independent of the firmware being run:

- binutils disassembly agrees with the decoder on 65,605 instructions across
  the firmware images;
- all 90 opcode patterns extracted from the C33 PE Core manual agree with the
  generated table;
- instruction semantics, exception behavior, reset values, and modeled MMIO
  registers are covered by manual-derived focused tests; and
- 40 generated defined C programs at each of five optimization levels - 200
  target runs per compiler - match native execution under both GCC 3.3.2 and
  GCC 16.2.

See [`difftest/README.md`](difftest/README.md) for generated-program and ISA
coverage details.

## Boot images

Hardware-style boot reproduces the mask ROM's externally visible effect:
copy the first 512 bytes of serial FLASH to RAM, set the internal-RAM stack,
and enter the MBR. All later stages execute their real firmware:

| Stage | Location |
| --- | --- |
| mask-ROM effect | emulator startup |
| MBR | serial FLASH offset `0x1` |
| menu | serial FLASH offset `0x300` |
| file-loader | serial FLASH offset `0x2300` |
| kernel and applications | FAT32 card |

Build the FLASH image from the repository root with:

```sh
make AWK=awk mbr
```

A card directory contains `kernel.elf`, `init.app`, `wiki.app`, fonts,
`wiki.inf`, and a language data directory such as `enquote/`. On macOS a
512 MiB image can be made with:

```sh
hdiutil create -size 512m -fs "MS-DOS FAT32" -volname WIKIREADER \
    -layout NONE -format UDRW -srcfolder /path/to/wikireader-card -o wrcard
mv wrcard.dmg emulator/images/wrcard.img
```

## Decoder regeneration

Regeneration is needed only when the ISA tables change and requires a C33
binutils installation. Disassemble all 65,536 words once as the all-core raw
binary and once in a PE ELF, place the listings in `allinsn_full.txt` and
`allinsn_pe.txt`, then run:

```sh
make tables
make test
make test-manual
```

`tools/derive_fields.py` solves operand fields; `tools/fit_ext.py` and
`tools/fit_data_ext.py` verify prefix composition. Generated tables are
committed and are generation-time artifacts, not runtime dependencies.

## Remaining opportunities

1. Add independent runtime cases for implemented stack-special and indirect
   jump forms not retired by firmware or generated C tests.
2. Refine cross-bank SDRAM command/data overlap from the manual.
3. Calibrate absolute SD latency and the timing model on physical hardware.
4. Automate reproducible stock/modern/PIO/DMA card-image benchmarks.
5. Model illegal delay-slot unstable behavior if a real workload needs it.

Do not add unused SoC peripherals solely for completeness.

## Layout

| Path | Purpose |
| --- | --- |
| `src/c33.[ch]` | CPU execution, traps, and interrupt entry |
| `src/mem.[ch]` | memory map and MMIO dispatch |
| `src/elf.c` | ELF32 loader |
| `src/sdcard.c`, `src/dma.c` | SPI SD card and DMA |
| `src/sdramc.c` | SDRAM controller and timing |
| `src/lcd.c`, `src/display.c`, `src/touch.c` | panel and input |
| `src/itc.c`, `src/cmu.c`, `src/timer.c` | interrupts, clocks, and timers |
| `src/port.c`, `src/periph.c`, `src/wdt.c` | GPIO, ADC, and watchdog |
| `c33_forms.h`, `c33_pe_valid.h` | generated decode data |
| `tools/` | generators and focused model tests |
| `difftest/` | native-versus-C33 execution tests |

## Known boundaries

- Illegal instructions in delay slots do not have an explicit unstable-state
  model.
- Cross-bank SDRAM traffic is intentionally conservative.
- Absolute storage timing is a prediction until measured on hardware.
- Direct ELF boot skips the board's SDRAM initialization, so hardware timing
  comparisons should use the FLASH boot path.
- An emulator/firmware match alone is not proof of silicon behavior; the
  manual, binutils, focused model tests, and differential runs provide the
  independent checks above.
