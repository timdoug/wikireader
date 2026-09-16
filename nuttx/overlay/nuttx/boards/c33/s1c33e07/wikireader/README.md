# WikiReader C33 port

NuttX support for the Epson S1C33E07 (C33 PE), using the modern
`c33-epson-elf-` GCC toolchain in `~/wikireader`. NSH runs on the 240x208 LCD
with a touch keyboard, on a physical WikiReader since 2026-09-12 and in wremu.
Direct ELF boot and the complete emulated FLASH/menu/card-loader handoff both
pass. Except where a section says otherwise, the timings in this file are
emulator measurements; `../../../../../../nuttx/README.md` records what the
device found that the emulator could not.

The `lcd` configuration provides a scrolling NXTerm terminal with a 6x9 font
(40 columns, about 13 rows) and four keyboard rows. `nsh` remains a serial-only
configuration, and `ostest` runs the configured NuttX OS tests.

The `app` configuration is `tcc` relinked to run as a Grifo application, which
is how the WikiReader's own launcher starts it, and is what `~/wikireader/nuttx`
builds by default. It is described under *Run from the launcher* below.

## Compile C at the NSH prompt

The experimental `tcc` configuration adds a native TinyCC C33 compiler to the
LCD terminal. The application lives in `apps/interpreters/tinycc`, like every
other interpreter; the compiler sources it builds against are a separate
TinyCC checkout, found through `CONFIG_INTERPRETERS_TCC_SRCDIR`, which
defaults to `tinycc/` beside `apps/` in the NuttX root:

```sh
export PATH="$PWD/.venv/bin:$HOME/wikireader/host-tools/toolchain-c33/work/install/bin:$PATH"
tools/configure.sh -e -m -a apps wikireader:tcc KCONFIG_OLDDEFCONFIG=olddefconfig
gmake -j8
$(python3 boards/c33/s1c33e07/wikireader/tools/wr_boot.py --image nuttx)
```

That last line builds a card holding `nuttx` as `init.app` under grifo, and a
FLASH image, and prints the emulator command that boots them. The emulator
refuses a bare ELF: booting one skips the loader, so the SDRAM controller and
the clocks are whatever it invents for them.

In NSH, run `tcc -run /tmp/hello.c` or `tcc -run /tmp/fib.c`. The command creates
those examples and minimal headers on its first invocation. Source files can
be written with `echo` and redirection; `tcc -c file.c -o file.o` saves an ELF
object, and `tcc -run file.o` links and executes it later. `/tmp` is tmpfs, so
these files are lost at reboot.

The compiler runs inside NuttX and emits native C33 instructions. Integer C,
64-bit arithmetic, structs, variadic functions and recursion pass emulator
tests, including cross-calls with GCC. Software floating point and compiling
TinyCC itself are supported. Run `tcc -selfhost`, then use the resulting compiler
with `tcc -run /tmp/tcc.o -run /tmp/fib.c`. The bundled compiler source is at
`/tmp/tcc/src/bootstrap.c`. This configuration enables
exit callbacks for compiler cleanup and uses 256-character shell lines.
See `apps/interpreters/tinycc/README.md` for the application, its Kconfig
options and how to drive it, and `tinycc/README.c33.md` for the code
generator, the ABI and the target's limits.

Run `python3 boards/c33/s1c33e07/wikireader/tools/test_tinycc.py` to exercise
native compilation, object save/reload, error recovery and Ctrl-C. Run
`python3 boards/c33/s1c33e07/wikireader/tools/test_selfhost.py` to build three
native compiler generations and compare the generated compiler objects. Run
`python3 tinycc/tests/c33/run-abi.py` for GCC/TinyCC interoperability tests.

### Bootstrap speed

`tcc -selfhost` and then rebuilding the compiler with the compiler it just
produced runs 600 million instructions; the complete three-generation
`test_selfhost.py` run is 1,547 million and the generated compiler object is
613,897 bytes.

**Quote the FLASH/card boot times.** A direct ELF boot never programs the
SDRAM controller, so the emulator keeps a flat zero-wait window and every
guest time measured that way is optimistic by about a factor of four. Through
the complete FLASH/card boot, where the SDRAM model is engaged, stage 1 (the
GCC-built compiler) takes 23.7 s and stage 2 (the compiler TinyCC built for
itself) 51.5 s.

Those stages spend far longer stalled on SDRAM than executing, and there is
no data cache on this part: the controller's data queue is a single word and
hits 8% of the time, against 83% for the two-line instruction queue, so
essentially every data read is a full SDRAM access, and half of all row
activations are an instruction fetch trading places with a data access. Fewer
spills, not faster instructions, is what the remaining time waits on. Times
come from wremu probe timestamps around `tcc_main`.

Two things pay for most of that. The code generator, described under *Backend
notes* in `tinycc/README.c33.md`, reaches locals through a biased frame
register instead of computing an address, carries one extension prefix on a
branch instead of two, and loads word-sized scalar arguments in memory
straight into the ABI registers. And the string routines: the generic
`memcpy`, `memmove`, `memset`, `strlen` and `memcmp` move a byte per pass,
five C33 instructions each, and were about a third of stage 1's instructions,
mostly TinyCC copying its own value-stack entries. `libs/libc/machine/c33`
replaces them with word loops using the post-incrementing addressing mode,
selected by `CONFIG_C33_STRING_FUNCTION` in all four board configurations.
They are assembly, so a host build cannot run them; instead they are linked
bare metal and checked against byte-loop oracles at every size to 70 and
every source and destination alignment, including the overlapping `memmove`
directions:

```sh
python3 boards/c33/s1c33e07/wikireader/tools/test_string.py
```

The copies are not worth handing to HSDMA. The only DMA this port issues is
the card driver's block transfers, and the bootstrap's 415,941 `memcpy` calls
move 18.3 MB, a mean of **44 bytes** each. The device measurements in
`../wikireader/emulator/tools/mem_dma_bench/README.md` put the first size at
which DMA beats a libc copy at 1 KiB, about 23 times that mean, and DMA loses
to a CPU batch until 4 KiB. `memcpy` is 4.8% of the bootstrap, so even an
instantaneous one is a ceiling of under 5%. Unlimited-bus DMA also stalls the
CPU rather than overlapping with it, a channel is one shared resource that a
libc `memcpy` called from interrupt context would have to lock, and the
firmware records a part that does not wake from HALT on DMA completion. Where
those numbers do favour DMA is large copies and fills -- 49.8% less time than
libc at 512 KiB and 19.4% for a fill -- which is the SD transport the card
driver already uses.

## Build on this Mac

The current workspace already has `apps/`, `.venv/`, `kconfig-tweak`, and
Homebrew `flock` installed. From the NuttX root:

```sh
export PATH="$PWD/.venv/bin:$HOME/wikireader/host-tools/toolchain-c33/work/install/bin:$PATH"
tools/configure.sh -e -m -a apps wikireader:lcd KCONFIG_OLDDEFCONFIG=olddefconfig
gmake -j8
python3 boards/c33/s1c33e07/wikireader/tools/test_terminal.py
```

The result is the ELF file `nuttx`. The terminal test saves its exact image
and SHA-256 in `build/wikireader/lcd/`, along with startup/final PNG screenshots,
raw emulator output, a console transcript and the command used for the test.
To use the terminal interactively, click the keys in the SDL window:

```sh
$(python3 boards/c33/s1c33e07/wikireader/tools/wr_boot.py \
    --image build/wikireader/lcd/kernel.elf)
```

`Sh` applies to the next character. `123` switches to numbers and punctuation;
`Sh` on that page provides more symbols. `Ctl` modifies the next character,
including `Ctl`, `c` for Ctrl-C. Backspace, Tab completion and left/right cursor
keys support shell editing. Sliding off a pressed key cancels it. `exit`
starts a fresh shell while keeping the display service running. The same
session is mirrored to UART0, which also accepts debug input.

The keyboard remembers each key's displayed label and highlight. Ordinary
typing repaints only the pressed/released key's interior. Modifier or page
changes repaint only keys whose appearance changes; NX exposure events force
a repaint of the affected keys, including their borders.

The PTY sends ordinary output in chunks, preserving CR/LF processing and
foreground signals, rather than splitting every output byte into its own pipe
write. Packed glyph copies update destination bytes with masks instead of
individual pixels.

`CONFIG_NXTERM_BATCH` composes each terminal write in a private 1bpp bitmap
and sends the changed rectangle to NX once, including cursor updates and any
scrolling. This uses 3,600 bytes for the 240x120 text area plus bookkeeping.
The bitmap retains existing pixels under spaces and short glyphs; source
glyphs are copied immediately, and the final NX bitmap operation completes
before the terminal lock is released and the buffer can be reused. Resizing
preserves overlapping pixels and leaves the old buffer intact on allocation
failure. The option supports monochrome framebuffer configurations only.

Scrolling copies byte-aligned framebuffer rows with overlap-safe byte moves,
preserving stride padding, and compacts saved characters in one pass per
scroll.

Together those take a `ps` to 71 terminal writes and about 103 ms of emulated
output time, and three consecutive `help` listings (62 scrolls) to 10.66
million instructions and about 0.224 seconds. Both regressions check the
console output and the screen pixels as well as the budget. These are guest
times under the emulator, not wall-clock or device measurements, and they were
taken when the port thought MCLK was 48 MHz rather than 60, so each is about a
quarter long.

The tested LCD image occupies 220,364 bytes of text/rodata, 1,048 bytes of
initialized data and 15,488 bytes of BSS/reserved stack, before runtime heap
allocations. The framebuffer occupies another 6,656 bytes in internal VRAM.

For serial-only NSH, configure `wikireader:nsh`, build, then run:

```sh
python3 boards/c33/s1c33e07/wikireader/tools/test_emulator.py
```

That test retains `build/wikireader/nsh/kernel.elf` and exercises console RX/TX,
`uname`, `ps`, `free`, a background task and timed sleeps through the emulator's
`--uart-input` support.

To build the OS test configuration, save any local configuration changes
before switching targets:

```sh
tools/configure.sh -e -m -a apps wikireader:ostest KCONFIG_OLDDEFCONFIG=olddefconfig
gmake -j8
python3 boards/c33/s1c33e07/wikireader/tools/test_emulator.py --ostest
```

`ostest` must report status 0 and return to NSH. The test runner also checks
the final shell marker and scans for guest assertions/errors; wremu's exit
status alone does not establish success. This configuration uses two prime
search runs over 1,000 integers and `TEST_LOOP_SCALE=10` to limit emulator
runtime. A longer run with range 10,000 and scale 100 also passed during
bring-up. Only tests enabled by this configuration are covered.

Use `-e` when switching configurations; save any custom configuration first. The make-variable override
above avoids the Kconfig warning filter's `/dev/fd` process substitution,
which fails in the restricted macOS workspace. Configuration warnings remain
visible. Linux hosts should use `-l` instead of `-m`.

### Dependencies for a fresh workspace

Tested versions:

* NuttX base: `88c8623ced`.
* nuttx-apps: `2f76986584ddd3881335e5897bbd80e12ae431a2`.
* C33 GCC: 16.2.0; binutils: 2.47.20260726.
* Python Kconfiglib: 14.1.0; GNU make; `flock`.
* wremu with the UART changes and 256-event tap scripts in `~/wikireader/emulator`.

For a fresh workspace, install the usual NuttX host dependencies and clone
`https://github.com/apache/nuttx-apps.git` as `apps/` at the revision above.
The local dependency directories are ignored by this repository. The small
Kconfig setup used here is:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install kconfiglib==14.1.0
git clone https://github.com/patacongo/tools.git /tmp/wikireader-kconfig-tools
git -C /tmp/wikireader-kconfig-tools checkout 9484147c12d051014f854852d59c21d75a9616bd
sed 's/@CONFIG_@/CONFIG_/g' /tmp/wikireader-kconfig-tools/kconfig-frontends/utils/kconfig-tweak.in > .venv/bin/kconfig-tweak
chmod +x .venv/bin/kconfig-tweak
```

This supplies the same `kconfig-tweak` utility referenced by NuttX's
[host installation guide](../../../../Documentation/quickstart/install.rst),
without building the optional native menu frontends. On macOS, GNU make and
`flock` are available through Homebrew. The existing wremu build uses SDL2:

```sh
make -C "$HOME/wikireader/emulator" -j8 wremu test-uart
```

## Implemented

* GCC C33 PE ABI, startup, SDRAM linker layout and aligned task stacks.
* Exception vectors, full integer/ALR/AHR context save/restore, interrupt
  masking, task switches, exits, cancellation and signal delivery.
* Periodic timer 2 scheduler tick, divided from the 60 MHz MCLK Grifo's PLL
  leaves running (`CONFIG_S1C33E07_MCLK`). A loader that never starts the PLL
  leaves the 48 MHz crystal instead, and the tick then runs a quarter fast.
* UART0 console at 57,600 baud, 8N1, with interrupt-driven RX/TX and NuttX's
  serial upper half; `/dev/console` and `/dev/ttyS0`.
* LCD framebuffer at `0x00080000`, 1 bit per pixel, MSB first, 32-byte stride;
  `/dev/fb0`, NX windows, NXTerm output, and a POSIX PTY for NSH.
* UART1 touch at 9,600 baud with packet state retained across IRQs. A worker
  publishes down/move/up samples through `/dev/input0`. Invalid/error releases
  cancel key selection. A bounded queue preserves release under overload.
* An NSH supervisor, NX server, event listener and terminal/input bridge.
  Ctrl-C interrupts NSH's own sleeps and is delivered to foreground apps.
* Procfs mounted at `/proc`, so `ps` and `free` work.
* The `vi` work-alike from `apps/system/vi`, editing files on the device's
  own screen. See "Full-screen editing" below.
* Word-at-a-time `memcpy`, `memmove`, `memset`, `strlen` and `memcmp` in
  `libs/libc/machine/c33`, selected by `CONFIG_C33_STRING_FUNCTION`.
* `setjmp()`/`longjmp()` in `arch/c33/src/common/c33_setjmp.S`, under
  `CONFIG_ARCH_HAVE_SETJMP`/`CONFIG_ARCH_SETJMP_H`. The buffer holds the four
  callee-saved registers `%r0`-`%r3`, the caller's stack pointer and the
  return address; `%r4`-`%r13` are caller-saved, `%r15` is fixed, and the PSR
  is not part of the calling convention, so nothing else has to be kept.
  `ostest` covers it, `longjmp(env, 0)` returning 1 included.
* `arch/c33/include/syscall.h`, empty because this is a flat build with no
  system call interface, and present because `<sys/syscall.h>` includes it
  unconditionally on every architecture.
* Heap bounded by configured RAM and the loader's SDRAM geometry. Direct ELF
  emulator boot omits SDRAM initialization and uses `CONFIG_RAM_SIZE`.
* Running as a Grifo application, started from the WikiReader's icon menu,
  with `poweroff` and `reboot` going back through the kernel, under
  `CONFIG_WIKIREADER_GRIFO_APP`. See "Run from the launcher" below.

The interrupt frame lives on the interrupted task's stack. Trap 0 switches
tasks, trap 1 delivers signals, and trap 2 captures diagnostic registers.
GCC expects a 16-byte-aligned outgoing argument area; CALL pushes a four-byte
return address. Native assembly SP offsets are in words, while `xld.w`
pseudo-instruction offsets are in bytes. Keeping those conventions explicit
is essential when changing the exception entry code.

The kernel is linked entirely into SDRAM starting at `0x10000000`, leaving
the loader's internal RAM alone. The ELF loader must load initialized
sections; NuttX clears its BSS on entry. The idle stack size and RAM region
come from the NuttX configuration.

## Validation and limits

Serial shell commands and the configured `ostest` suite passed in wremu,
including task restart, pthread cancellation, mutexes, semaphores, message
queues, nested signals, POSIX timers, watchdog timers, round-robin scheduling,
barriers and scheduler locks. Emulator UART, CPU instruction, IRQ, exception,
LCD, ADC, clock, timer, SDRAM, chip-ID, GPIO, display, SD, DMA and event-wait
unit/regression targets also passed.

A synthetic emulator entry shim with initialized 16 MiB SDRAM geometry also
passed the shell test; `free` reported a 16,511,200-byte heap with the OS-test
image. This checked the heap cap before the full FLASH boot was available.

The aggregate emulator `make check` completes. Three targets skip on a bare
checkout and say so: the binutils decode comparison wants `ref-*.txt` objdump
captures, and the two GUI boot targets want
`images/wrcard.img`/`images/grifo.elf`.

The terminal regression uses 215 actual emulated panel taps and a drag. It
checks letters, Shift, quoted punctuation, Backspace, cursor movement, Tab
completion, Ctrl-C during both `sleep 30` and a separate `sh -c` task,
cancellation of an unfinished command,
`exit`/restart, `ps`, `free`, and terminal scrolling. A separate UART input run
repeats the foreground interrupt and prompt cancellation checks. The test
checks that input is echoed and that scrolling leaves the keyboard's pixels
unchanged. It passes with both direct ELF and FLASH/card boot. See the
generated screenshots.

## Full-screen editing

`vi` edits a file on the LCD, which is what makes writing C on the device
practical. `CONFIG_SYSTEM_VI` and `CONFIG_SERIAL_TERMIOS` enable it; without
the latter the editor cannot put the console into raw mode and reads whole
lines instead of keys. The on-screen keyboard has no ESC key, but Ctrl and
`[` are both on it and the bridge masks that pair to `0x1b`.

Two things had to be built for this to work at all. NxTerm previously
recognised three escape sequences - erase to end of line, and cursor left and
right - and, worse, drew the bytes of anything else as text, so a
cursor-addressed redraw filled the screen with garbage rather than being
ignored. `graphics/nxterm/nxterm_vt100.c` now parses control sequences
properly: cursor positioning and movement, erase in line and in display,
save/restore, and silent consumption of well-formed sequences it does not
implement, such as colour on a monochrome panel. Addressing the cursor also
means a cell can be overwritten, so `nxterm_addchar()` replaces the glyph
already at a position instead of appending a second one at the same place.
The cursor shows the character it is on in reverse video rather than drawing
a cursor glyph over it: a glyph is rendered across its whole bounding box,
background included, so the old cursor erased exactly the character being
edited. Only the one glyph under the cursor is inverted, into a scratch
buffer sized for a single glyph, rather than keeping a second font cache.

The second piece is the window size. `TIOCGWINSZ` on `/dev/nxterm0` reports
the window in whole character cells - 39x13 here, at 6x9 pixels in a 240x120
region - and the PTY carries a size for its pair, which the terminal bridge
publishes at startup. Without it an editor has to fall back to probing the
terminal with a cursor position report and reading the reply, which on this
board swallowed the first keystroke after startup.

The editor test decodes the framebuffer back into characters using the same
font the terminal renders with, so a redraw that lands in the wrong cells
fails rather than merely looking wrong, then edits a file through the on-LCD
session and checks the result:

```sh
python3 boards/c33/s1c33e07/wikireader/tools/test_vi.py
```

Replacing a glyph in place costs a search of the saved character list. The
search is skipped for any write at or past the position after the last
character added, which is every write a scrolling terminal makes, so ordinary
console output does not pay for it -- searching unconditionally costs 59% on
the scrolling regression below.

The redraw regression compares actual NX drawing calls for touch and UART
input producing the same character. A normal tap adds two interior fills and
two glyph draws, for press and release. A Ctrl tap paints only its own
highlight and retains it on release. The test also checks the resulting
pixels:

```sh
python3 boards/c33/s1c33e07/wikireader/tools/test_keyboard_redraw.py
```

The scrolling regression saves the tested ELF, instruction profile, screen,
console transcript and timing metrics. It checks that all three listings and
the final prompt complete within a 25-million-instruction budget. An optional
earlier output directory enables exact console/pixel comparisons and a timing
comparison:

```sh
python3 boards/c33/s1c33e07/wikireader/tools/test_scrolling.py
python3 boards/c33/s1c33e07/wikireader/tools/test_scrolling.py \
  --baseline build/wikireader/scroll-before
```

The `ps` regression checks command completion, output fields, and budgets of
120 terminal writes, 160 bitmap submissions and 8 million total executed
instructions. Its optional
baseline comparison allows the printed pthread entry addresses to change
when linking a new ELF:

```sh
python3 boards/c33/s1c33e07/wikireader/tools/test_ps_output.py
python3 boards/c33/s1c33e07/wikireader/tools/test_ps_output.py \
  --baseline build/wikireader/ps-before
```

To compare host emulator speed, retain the older `wremu` executable and run:

```sh
python3 boards/c33/s1c33e07/wikireader/tools/test_emulator_speed.py \
  --before build/wikireader/emulator-before/wremu
```

This alternates three runs of each executable, reports median command-to-prompt
wall time, and requires identical guest work, console output and screen pixels.
The emulator's interrupt regression checks its fast rejection path against
20,000 register states, including individual flag/enable combinations, reserved
bits, zero priorities and simultaneous causes, and `make test-timer` compares
16,000 timer states reached by frequent polls against larger time jumps,
including fractional ticks, clock gating, pauses, comparison buffering and the
timer 0/5 cascade.

Five host tests exercise the actual C implementation with ASan/UBSan:

```sh
python3 boards/c33/s1c33e07/wikireader/tools/test_touch.py
python3 boards/c33/s1c33e07/wikireader/tools/test_packed_graphics.py
python3 boards/c33/s1c33e07/wikireader/tools/test_nxterm_scroll.py
python3 boards/c33/s1c33e07/wikireader/tools/test_nxterm_batch.py
python3 boards/c33/s1c33e07/wikireader/tools/test_pty_output.py
```

The batching test compares 384,000 buffered drawing operations against a
pixel oracle, covering both bit orders, clipping, overlapping moves, resize
preservation, allocation/submission failure and glyph buffers freed before
the final update. It verifies that drawing operations make no server calls
until flush, and that each nonempty batch needs only one bitmap submission.

The first models a four-byte UART FIFO and covers packet splits, header
resynchronization, unchanged coordinates, range checks, contact IDs, all three
UART receive errors and queue pressure. wremu's legacy touch model delivers
whole packets into a larger FIFO; the host test covers that fidelity gap.
The graphics test compares 180,000 randomized fill/copy/overlapping-move cases
at 1/2/4 bpp, in both packing orders, with a pixel oracle and guarded buffers.
The original NX packed framebuffer code fails this test. Its byte masks,
source bit alignment and overlapping copies are corrected by this port.
It also checks tightly allocated glyphs at every source/destination bit
alignment to catch reads past the last byte, and full-width 240-pixel
scrolling in both directions, including untouched stride padding and
keyboard rows. The NXTerm test checks 8,000
empty/full/mixed character caches, retained character order and coordinates,
and display operation bounds with readable and write-only displays and two
line-separation settings.
The PTY test covers 12,800 binary/CR/LF translation cases across four signal
configurations, verifies bulk writes, exercises partial/error retries, and
checks control-character delivery to the opposite endpoint's foreground PID.

### Complete emulated FLASH/card boot

The MBR copies **7,424 bytes** of a boot program and no more, which a
file-loader can outgrow: past that its filename table falls outside the copied
region, FLASH offset `0x4000` already belongs to the next application header,
and the symptom is an empty filename before NuttX is ever entered.
`samo-lib/mbr/Makefile` now fails the build when a program exceeds it.

The fixture tool builds a **separate kernel-only loader**, preserving the GPL
notice in the generated source. It uses the existing firmware libraries and
linker script, rejects a payload larger than the slot/copy limit, and patches
only `0x2300..0x3fff` in a copy of `flash.rom`. The source checkout is not
changed.

```sh
python3 boards/c33/s1c33e07/wikireader/tools/make_boot_fixture.py
python3 boards/c33/s1c33e07/wikireader/tools/test_terminal.py \
  --flash build/wikireader/boot-fixture/flash-nuttx.rom \
  --card build/wikireader/boot-fixture/nuttx-card.img \
  --out build/wikireader/lcd-flash
```

The fixture tool also makes a FAT32 card image containing `kernel.elf`, verifies
file readback, and records input/output hashes in `manifest.json`. It reuses
the existing pure FAT fixture helper in `emulator/tools/mem_dma_bench/run.py`;
it does not run that helper's build or benchmark commands. The emulator opens
the card read-only. The full boot uses the loader's 16 MiB SDRAM configuration
and reaches the same LCD terminal. No physical card or device was written.

### Run from the launcher

The `app` configuration boots the other way round: instead of a loader that
hands over the whole machine, the WikiReader's resident kernel, Grifo, loads
NuttX off the card as an application from its icon menu, the same way it loads
the ZIM reader and DOOM.

`CONFIG_WIKIREADER_GRIFO_APP` is what makes that work, along with
`CONFIG_RAM_START=0x10040000`, which keeps the image clear of the 256 KiB Grifo
occupies. Grifo's loader places allocated sections at their linked addresses
and jumps to `e_entry`, so nothing else about the image has to change: no
application ABI, no entry symbol name, no linker script of its own.

Stopping is `boards/.../src/wikireader_grifo.c`, and it is nine instructions.
`board_power_off()` and `board_reset()` disable interrupts, restore the trap
table `up_irqinitialize()` saved, and issue Grifo's `System_exit` -- `int 1`
followed by the halfword 16 -- with `EXIT_POWER_OFF` or `EXIT_REBOOT`. Cutting
the power rail means toggling the watchdog output pin, and resetting means
arming the watchdog; both are board specifics Grifo already implements.

Nothing but the trap table is handed back. Grifo's `System_exit` path runs to
`power_off()` or the reset watchdog with interrupts disabled -- it only ever
restores an interrupt state it saved, never enables unconditionally -- so the
interrupt controller can be left as this port had it.

Two details are still load-bearing. The trap table has to be restored or the
`int 1` reaches this port's own software-exception handler. And the exit code
cannot be passed through a local register variable: the compiler only
guarantees such a variable is in its register at the next `asm`, and is free to
use `%r6` for anything until then, so it is loaded inside the `asm`.

Nothing has to keep the watchdog alive either. Grifo arms a 20-second reset
before calling an application; `c33_lowsetup()` already stops it, early enough
that the port has never needed to know.

`wremu` models the watchdog, the reset it asserts and the power rail Grifo
drops, so both exits can be followed to the end: `poweroff` drops the rail and
the run stops, `reboot` resets the machine and the launcher comes back up. That
is still an emulator result, as the rest of this file is -- the rail and the
reset are modelled at the pin, not electrically.

```sh
tools/configure.sh -e -m -a apps wikireader:app KCONFIG_OLDDEFCONFIG=olddefconfig
gmake -j8
python3 boards/c33/s1c33e07/wikireader/tools/test_grifo_app.py
```

The test builds a card carrying `init.app`, this image stripped as `nuttx.app`
and a second entry so that the launcher draws its menu, taps the first icon,
runs `uname` and `free`, and leaves with `poweroff`; then repeats it with
`reboot`. It requires Grifo's printed exit code to be 1 and 2 respectively,
the power-off run to end with the rail dropped and the device to stay down,
and the reboot run to reset and come back up to the launcher. `--compile`
additionally compiles and runs a C file on the device.

The application is the stripped kernel: 2,928,780 bytes against 16,131,276,
most of the difference being DWARF that Grifo's loader would skip anyway. It
links `.text` at `0x10040000` and ends, with BSS and the idle stack, at
`0x103290e0`, so it occupies 2.9 MiB of the 16 MiB part. `free` under the
emulator's 32 MiB geometry reports about a 31.6 MB heap; `up_allocate_heap()`
caps it to the controller's configured size, which is what a 16 MiB board
depends on.

The card is NuttX's own once it is running: `s1c33e07_spi.c` plus stock
`mmcsd_spi`, `vfat` and the MBR reader mount the FAT32 boot partition at
`/sd`. The exFAT partition registers as a block device and cannot be
mounted, because NuttX's FAT driver does FAT12/16/32 only.

This port currently supports the GNU make flat build and C applications.
It has no CMake port, C++ runtime, separate interrupt
stack, nested external IRQs, stack coloration or hardware debug transport.
UART termios changes and flow control are not implemented. Timer 2, UART0 and UART1
are the peripheral IRQs supported by the architecture code. The busy-wait
delay calibration still needs a hardware measurement. Direct ELF execution
does not validate SDRAM bus setup, pin mux, electrical behavior or power use.

The PTY input queue is bounded at 1 KiB. Keys are dropped when an application
stops reading and fills it; the display worker stays responsive and Ctrl-C
still reaches the foreground task. There is no key repeat, scrollback viewer,
physical-button binding, terminal resizing or Unicode font support. NXTerm
provides the escape sequences needed for NSH editing; full-screen applications
and every VT100 sequence have not been validated.

## Next: hardware and storage

Done since this list was written: the handoff runs on a device (2026-09-12),
which corrected the tick frequency and the touch baud rate and found the P5
function-register write that killed SDRAM; the SPI controller, card power and
chip-select handling, MMC/SD and FAT mounting are in, so NSH reads and writes
the card itself; and the loader size problem is fixed at the source —
`samo-lib/mbr/Makefile` now fails the build when a boot program exceeds the
7,424 bytes the MBR copies.

What is left:

1. exFAT, so the archive partition can be mounted rather than merely
   registered. ChaN's FatFs is the plan.
2. Card detect and hot-swap. `SPI_STATUS` always says a card is present, so a
   missing one shows up as an identification timeout at boot.
3. Buttons, power management and suspend/wake behavior, then refine key
   sizes/layout and terminal navigation on the physical panel.
4. A power measurement. Nothing here has been near a meter.

The existing hardware manuals and working drivers under `~/wikireader`
remain the reference. This port does not call the Grifo kernel or reuse its
application ABI. Changes in that checkout are limited to emulator support;
other applications and firmware source files are preserved.
