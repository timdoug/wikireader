# wremu - WikiReader full-system emulator

`wremu` boots the WikiReader's Epson S1C33E07 firmware from ELF files or
through the serial-FLASH boot chain, attaches a FAT32 card image, and presents
the 240x208 touch display through SDL2.

Build the firmware with the [modern C33 toolchain](../host-tools/toolchain-c33/README.md).
See the [ZIM reader guide](../zim/README.md) for archive and card setup.

## Status

- The mask-ROM effect, MBR, menu, file-loader, kernel, `init.app`, and
  `wiki.app` boot from the current FLASH and card images.
- Shipped GCC 3.3.2 and current GCC 16.2 firmware reach matching UI, search,
  article, and scrolling framebuffers for the exercised workflows.
- Kernel block reads use the documented SPI HSDMA/IDMA pipeline with bounded
  completion polling; HALT-based DMA completion did not wake on hardware.
  Pre-kernel reads and all card writes remain PIO.
- `make check` covers the decoder, core ISA, exceptions, interrupts, LCD,
  display input, SD, DMA, clocks, ADC, timers, watchdog, SDRAM, GPIO, and chip
  identification.
- Firmware and differential programs retire 57 of the 74 implemented PE
  operations. Focused core tests cover most remaining forms.
- Headless execution is deterministic and runs at about 80 million target
  instructions per host second.

Timing follows the programmed clocks and memory/storage registers, with
additional costs calibrated on hardware. See "Calibration" for the fitted
parameters and the limits of absolute timing predictions.

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

`make test-sd-dma-driver` additionally compiles the production SD receive
backend as C33 code and runs both `SD_DMA_BITS=8` and `32`. It covers alignment,
byte order, CRC boundaries, partial-transfer recovery, overflow, profiling,
busy-register access, and GPIO/interrupt restoration. It requires the project
C33 toolchain (or `TOOLCHAIN_BIN=/path/to/bin`) and creates a temporary 512-byte
card image; it does not access attached cards or archives.
Both `SD_DMA_TX=IDMA` and the default `HSDMA` configuration are tested,
including one-word payloads and stopping TX with a word queued behind the
shift register. There are 122 C33 cases across the four configurations.

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
| `-X ADDR[,NAME]` | Count entries, report the longest gaps, and list the eight most frequent callers (the return address at entry; resolve with `addr2line`). |
| `-Y A,B`, `-y M,N` | Limit profiling by address or guest-time interval. |
| `-F FILE` | Write non-empty profile buckets: address, instructions, MCLK cycles, cycles waiting for the fetch, SDRAM row activations. Buckets cover 2 MB of SDRAM and, separately, the internal RAMs, so code in A0 RAM or IVRAM is never confused with the kernel or application at the same SDRAM offset. |
| `-Z ADDR` | Rebase the scripted input timeline on the first hit of `ADDR`. |

`WREMU_MODEL=name=value,...` overrides the fitted timing parameters listed
under "Calibration" for an experiment; the summary's `--- model` line shows
the set in use. `WREMU_SDRAM_TIMING=tRP,tRAS,tRC` (clocks) and `WREMU_SDRAM_AURCO=N`
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

### Profiling and timing

Use `-Z` to align scripted input to a guest milestone. Absolute `-T`/`-K`
cycle numbers do not produce comparable interactions when two firmware builds
reach the UI at different rates. Use `-Y` or `-y` to exclude idle polling
from profiles.

A `-Y` window also reports the SDRAM controller's work inside it: wait
cycles, refreshes, queue hits and misses, and row activations by bank, by
access kind per bank, and by the kinds of the two accesses on either side of
each activation (`--- window sdram`, `--- window activations by bank`, and
the following lines). With `WREMU_ROWHIST=1` the window also lists the 24
most activated 1 KiB rows and the 24 most frequent row-to-row transitions
within a bank (`--- window activations by row` and `by row pair`), which
tells which objects alternate; a same-row pair is a bank closed by refresh
and reopened. `WREMU_ROWTRACE=0xADDR` prints the PC and access kind behind
the first 48 activations of that address's row inside the window, after
skipping `WREMU_ROWTRACE_SKIP` of them. `WREMU_WINDOW_REPEAT=1` reopens a
`-Y` window at every later hit of its start address and adds the intervals
up, for a phase that recurs once per block. The summary's `--- dstram stack`
line gives the lowest stack pointer seen inside DSTRAM, where the ZIM reader
runs its decoder loops on a private 1 KB stack. The `-F` profile carries cycles and activations per
2-byte bucket, so `addr2line` on an unstripped link turns it into a
per-source-line cost that includes memory stalls. Cycles, not instruction
counts, are what to look at on this core: the ZIM article load below runs at
3.5 to 4.7 cycles per instruction, and an instruction-count profile of it
ranks the wrong lines. The CPU has no cache; the model charges a row change
of about 2 tRP + waits on tRAS/tRC for every move to another 1 KiB row of a
bank (the WikiReader measurements use 4 MiB bank strides), closes every bank at each
auto-refresh, and treats the two-slot 16-byte instruction queue as the only
fetch buffering.

ZIM reader article load, Simple English `Cat`, retrieval to render entry,
run from the repository root with a card from `zim/make-card-image`:

```sh
./emulator/wremu -R -e samo-lib/mbr/flash.rom -c /tmp/card.dmg \
    -T 40,36,100000000 -K 300000000,CAT -T 30,40,500000000 \
    -Y 0x<retrieve_article>,0x<render_article_with_pcf> -F prof.txt -n 1000000000
```

Use addresses from the matching `zim/zim.map`. The first tap picks the reader
on the launcher menu, so `-Z` cannot be used. Scripted tap release is
delivered when the emulator next idles; a faster build can therefore render
extra frames while the scripted touch remains held.

### Calibration

The defaults in `src/model.c` were fitted on 2026-09-07 to an early 32 MB
WikiReader running the retimed kernel (`WREMU_BOARD_REV=7`). Parameters
can be overridden with `WREMU_MODEL=name=value,...`.

| Parameter | Default | Meaning |
| --- | ---: | --- |
| `branch_taken` | 5 | taken branch cycles with an SDRAM target |
| `branch_taken_iram` | 4 | taken branch cycles with an internal-RAM target |
| `iqb_first` | 3 | extra SDCLK ticks before an instruction-queue fill |
| `iqb_word_gap` | 2 | extra ticks between words of that fill |
| `dq_extra` | 1 | extra ticks on a data-queue fill |
| `wr_ticks` | 0 | fixed CPU write ticks; zero uses transfer timing |
| `wr_rd_turn` | 3 | extra ticks for an SDRAM read after a write |
| `dma_extra` | 30 | extra MCLK cycles per HSDMA or IDMA transfer |
| `sd_read_latency` | 60000 | cycles from a read command to the data token |
| `sd_init_latency` | 0 | cycles from the first ACMD41/CMD1 until the card becomes ready |
| `sd_read_gap` | 0 | cycles before each subsequent CMD18 block token |
| `sd_write_latency` | 0 | programming busy cycles after a written block |
| `iram_fetch_wait` | 0 | extra cycles per internal-RAM instruction fetch |
| `dq_iram_extra` | 2 | extra ticks for data access from internal-RAM code |
| `dq_hit` | 1 | extra ticks for a data-queue hit |

Calibration microbenchmarks agreed within 12%, most within 5%. In the
measured article phases the model was 16-33% faster than the device. Use it
to identify expensive work and compare candidates, then confirm improvements
on hardware. Historical
calibration data and its retired harness remain in Git at `7aa4ee84`.

The three separate card waits were added on 2026-09-09. Their defaults are
zero because the current card has not yet been calibrated by boot phase;
they are mechanisms for fitting measured waits, not measured defaults.
They use MCLK cycles (60,000 cycles/ms at 60 MHz). Initialization polls
return idle until ready, streamed reads delay only the next data token,
and programming busy survives chip deselection. CPU and DMA costs retain
their previous calibration.

Match filesystem state as well as firmware before comparing boot times.
A generated FAT32 fixture with unknown FSInfo hints and no existing
`dma.txt` spent 647 ms creating its first diagnostic; its next boot took
26 ms for that step. This was allocation work, not a measured card write
delay. Builders should provide valid free-cluster and allocation hints.
For an existing **synthetic** fixture, run:

```sh
python3 emulator/tools/fat32_fixture.py /tmp/generated-card.img
make -C emulator test-sd-timing test-fat32-fixture
```

The tool bounds its FAT read and accepts regular image files only. Do not
apply it to captured physical metadata: the original allocation state is
part of the evidence. Also include the card's boot files, directory order,
existing logs, and history. The 2026-09-09 logical boot-file snapshot brought
the exact installed firmware's kernel-to-reader estimate from 1.840 s to
1.315 s, versus 1.398 s on hardware. That is 6% error instead of 32%, but
the fixture still does not reproduce physical fragmentation or deleted
directory slots. See [reader performance](../zim/PERFORMANCE.md) for the
comparison and scope.

The diagnostic kernel/app pair adds `KERNELBOOT` lines to `zimboot.log`:
kind 1 is mounting, kind 2 is the diagnostic checkpoint, and kind 3 is an
ELF load (`init.app`, then `zim.app`). These RAM snapshots include elapsed
time, read counts, read/DMA time, and errors, and are saved after the
keyboard is drawn. Install both binaries: the getter uses new syscall 120.
They will distinguish missing card waits from different amounts of work.

The summary separates executed instructions from fast-forwarded idle cycles:

```text
--- work: 219633709 instructions executed, 180366291 idle, 7973.0 ms guest ---
```

The `--- idle power: SD supply on ... ms, off ... ms ---` line divides
skipped HALT time by the board's P32 supply-enable state. It distinguishes
an idle card with power still applied from one whose supply is disabled;
P33 (the buffer enable), chip select, and SPI clock gating are not the
supply switch. It covers both headless and window runs, including DMA HALTs
in older firmware and the new full-clock timed event waits. SD-on time in
those short waits does not imply KEEP during deep suspend. This is
GPIO-state residency, not a current or battery
model; it excludes active execution and does not simulate card startup
current or card-specific standby behavior. See [power management](../zim/BATTERY.md)
for firmware comparisons and measurement limits.

Runtime firmware defaults to `-O2`: a full-FLASH stock-reader comparison
found `-Os` smaller but 3.2% slower in the modeled article interval, with
identical screens. That ordering has not been verified on hardware.
Boot stages retain `-Os` to fit their internal-RAM limits.

Kernel builds select the working DMA path with `SD_DMA=YES` (default), or
PIO with `SD_DMA=NO`. Production DMA completion polls with a bound: the
older HALT-based completion wait passed the emulator but never woke on
the real device.

`SD_DMA_TX=HSDMA` is the hardware-tested default for aligned word payloads.
Byte payloads still use RX-paced IDMA; `SD_DMA_TX=IDMA` selects the previous
word path for comparison. See [reader performance](../zim/PERFORMANCE.md).

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

The default read backend uses HSDMA3 for SPI RX and HSDMA2 to feed word TX.
Byte payloads and the IDMA comparison build use IDMA channel `0x24` to write
dummy TX data after RX completion. The model covers the dual-address
transfers used by firmware: request selection, priority, counters, address
updates, terminal enable clearing, descriptor writeback, clock gating, global
IDMA enable, and the terminal interrupt cause. A word block performs 128
HSDMA3 transfers plus either 127 IDMA transfers or 127 HSDMA2 transfers.
Byte mode uses 512/511 RX/IDMA transfers. Per-channel counters identify the
active pipeline in the summary.

Manual V.2.5 defines separate TXD and shift registers: TDEF/TXDE occurs at
shift start, while RDFF/RXDE occurs at completion. One TX word can queue
while another shifts; SPI_WAIT delays consuming that queued word. The model
keeps those events on the wire timeline, serializes DMA bus use, and retains
disabled-channel trigger flags until accepted or explicitly cleared. TXDE
and RXDE independently gate their request sources. SPI characters carry bytes
MSB first. Focused tests check byte/halfword/word payloads, the wire-duration
formula, TX terminal count preceding the final two RX completions, stale
requests, and disabled request sources.

The fitted `dma_extra=30` cost is retained for both engines. It is an
empirical per-transfer allowance, not a measured arbitration waveform for
the new pipeline. The physical startup test measured 2.182731 s of file DMA
wait against the model's 2.119200 s, with no read errors or fallback. Total
startup was 3.511686 s on hardware versus 3.309662 s in the model. One
recorded boot supports this comparison; other workloads still need validation.
Software-triggered HSDMA also supports single, successive and block transfers,
fixed/incrementing/decrementing addresses, and address restoration at the end
of a successive transfer or each block. Each unit performs a read followed by
a write through the SDRAM timing model. With unlimited sequential access the
CPU's bus access stalls for the whole trigger. Limited sequential access,
other hardware triggers, preemption within a DMA unit, and cycle-level
CPU/DMA arbitration remain unmodeled. Nonzero access-time limits on multi-unit
transfers are rejected rather than silently timed as unlimited transfers.

Memory DMA uses the separate, uncalibrated `dma_mem_extra` parameter (default
zero additional MCLK cycles per unit); the fitted SPI `dma_extra=30` is not
applied to it. The CPU-only internal-code data-read allowance is also excluded
from DMA accesses. The [memory-copy benchmark](tools/mem_dma_bench/README.md)
compares the same app on the model and physical hardware, with an unchanged
kernel. `make test-mem-dma` checks the added controller semantics independently.

ITC reset uses zero cause flags as a deterministic choice; the hardware manual
marks them indeterminate. The reader's first integrated memory-copy test found
`FDMA=0x17` with all channels disabled: its guard treated HSDMA0's reset cause
as an outstanding transfer and skipped every copy. The C33 `test-zim-copy`
suite exercises both set and cleared causes, including initialization of an
unconfigured channel and preservation of a configured owner's completion.
See the [hardware diagnosis](../zim/PERFORMANCE.md#dma-selection-diagnostics).

SPI interrupt-enable and receive-mask registers are retained, and the receive
mask is applied to received data. The summary's `spi config` line counts
control-register access while busy and disabling ENA with nonzero SPI_INT,
both forbidden by manual V.2.8. These checks diagnose invalid driver sequences
without assigning a malfunction to undefined operations. SPI CPU interrupt
delivery is not modeled. The C33 driver regression starts with the physical
loader's `SPI_INT=0x14` and requires both violation counts to remain zero.

The physical sector probe found a one-bit advance in the card's response for
each SPI ENA cycle, including unchanged-width CPU reads. The model reproduces
that observed net effect on disable when the card is selected, P67 is muxed
to SPI, and CPOL=0. Holding P67 at idle as GPIO prevents the advance. The
`spi clock` summary counts unclamped disables. The exact physical edge,
command/write bit assembly, and electrical pin-mux transients remain
unmodeled. The corrected kernel reached the keyboard in 4.536 seconds on
hardware versus 4.224 seconds in the emulator; see the [hardware findings and
phase measurements](../zim/PERFORMANCE.md#spi-width-transition-fix).

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
3. Refine storage latency and article-phase costs against hardware.
4. Model illegal delay-slot unstable behavior if a real workload needs it.

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
- Storage timing is fitted to one card; wake latency remains simplified.
- Direct ELF boot skips the board's SDRAM initialization, so hardware timing
  comparisons should use the FLASH boot path.
- An emulator/firmware match alone is not proof of silicon behavior; the
  manual, binutils, focused model tests, and differential runs provide the
  independent checks above.

## UART0 console input

`--uart-input FILE` feeds bytes to the console UART through its four-byte RX
FIFO and interrupt controller. Use `-` for stdin. `--uart-start N` delays the
first byte until cycle N (default 1,000,000); `--uart-gap N` spaces bytes by
N cycles (default 50,000). File input is backpressured when the FIFO is full.
Newlines are passed unchanged. These options are independent of `-K`, which
types on the original firmware's touch keyboard.

For an NSH image, for example:

```sh
./wremu --uart-input commands.txt --uart-start 1000000 -n 100000000 nuttx
```

`make test-uart` checks FIFO ordering, overflow, receive/error interrupt
priority, flag reassertion while data remains, and UART reset. TX remains an
immediately completed transfer in the model.

Scripted `-T` taps, `-N` buttons and `-G` drags each accept up to 256 events.
Malformed events and scripts exceeding this limit are rejected instead of
silently dropping input. This accommodates full NuttX soft-keyboard commands.
