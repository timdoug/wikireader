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
- Kernel block reads use the documented SPI HSDMA/IDMA pipeline. Pre-kernel
  reads and all card writes remain PIO.
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

Direct kernel ELF boot is convenient for debugging:

```sh
./wremu -g -c images/wrcard.img images/grifo.elf
```

Hardware-style boot uses the serial FLASH image:

```sh
./wremu -g -e ../samo-lib/mbr/flash.rom -c images/wrcard.img
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
| `-T x,y,cycle` | Tap a pixel. |
| `-G x,y0,y1,cycle` | Drag vertically. |
| `-N code,cycle` | Press random/search/history/power (`0`-`3`). |
| `-b ADDR`, `-W ADDR` | Break on execution or a write. |
| `-D ADDR -L N -O FILE` | Dump target memory. |
| `-m` | Trace unclaimed MMIO accesses. |
| `-P` | Report retired-opcode counts. |
| `-H` | Profile executed addresses. |
| `-X ADDR[,NAME]` | Count entries and report the longest gaps. |
| `-Y A,B`, `-y M,N` | Limit profiling by address or guest-time interval. |
| `-F FILE` | Write non-empty profile buckets for comparison. |
| `-Z ADDR` | Rebase the scripted input timeline on the first hit of `ADDR`. |

`WREMU_SUSPEND_DIV=N` shortens the firmware's 120-second suspend interval
for testing without modifying the guest.

### Benchmarking

Use `-Z` to align scripted input to a guest milestone. Absolute `-T`/`-K`
cycle numbers do not produce comparable interactions when two firmware builds
reach the UI at different rates. Use `-Y` or `-y` to exclude idle polling
from profiles.

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
clock gating, and global IDMA enable. One 512-byte block performs 512 HSDMA
and 511 IDMA transfers. Unused DMA modes and trigger sources are not modeled.

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
