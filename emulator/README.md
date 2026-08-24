# wremu - WikiReader full-system emulator

Emulates the WikiReader's Epson C33 (S1C33) SoC well enough to boot the real
firmware from a real card image: `kernel.elf` (grifo) loads `init.app`, which
chains to `wiki.app`, which mounts a FAT32 SD card, renders to a 240x208
panel, and answers taps on its on-screen keyboard.

```
$ ./wremu -g -c images/wrcard.img images/grifo.elf
Grifo starting
init starting
starting wiki app
Display version.txt
VERSION: 20260823
```

Typing `LOVE` on the keyboard returns real article titles out of
`enquote/wiki.pfx` / `wiki.idx` / `wiki.fnd`.

## Building

```
make
```

Needs SDL2 (`brew install sdl2`) for the window. Everything else is plain C.
The generated decode tables are committed, so the c33 cross toolchain is
**not** required to build the emulator - only to regenerate those tables or
to rebuild the firmware itself.

## Running

```
./wremu -g -c images/wrcard.img images/grifo.elf
```

Click keys with the mouse; that is the touch panel. `Q` or `Esc` quits.

| flag | meaning |
| --- | --- |
| `-g`, `-S N` | SDL2 window, scale factor (default 3) |
| `-c FILE` | attach a FAT32 card image |
| `-n N` | stop after N instructions (unlimited with `-g`) |
| `-s` | trace grifo syscalls by name, with call sites and return values |
| `-K cycle,TEXT` | type TEXT on the on-screen keyboard |
| `-T x,y,cycle` | tap a pixel |
| `-G x,y0,y1,cycle` | drag vertically, for the scroll path |
| `-b ADDR` | breakpoint: registers plus recent PCs |
| `-W ADDR` | write watchpoint |
| `-V VAL` | watch stores of a byte value |
| `-D ADDR -L N -O FILE` | memory dump, optionally to a binary file |
| `-t N` | disassemble the first N instructions |
| `-m` | trace unclaimed MMIO registers |
| `-P` | histogram of opcodes actually executed |

`-s` is usually the fastest way in: it turns a hang into a named syscall,
a call site and a return value.

## Making a card image

```
hdiutil create -size 512m -fs "MS-DOS FAT32" -volname WIKIREADER \
    -layout NONE -format UDRW -srcfolder ~/wikireader-card -o wrcard
mv wrcard.dmg emulator/images/wrcard.img
```

The card directory is what `make install` produces: `kernel.elf`, `init.app`,
`wiki.app`, the `.bmf` fonts, `wiki.inf`, and a `<lang><suffix>/` data
directory such as `enquote/`.

## Layout

| path | |
| --- | --- |
| `src/c33.[ch]` | CPU: decode, execute, traps, interrupts |
| `src/mem.[ch]` | memory map and MMIO dispatch |
| `src/elf.c` | ELF32 loader |
| `src/uart.c` | EFSIF0 serial console |
| `src/sdcard.c` | SPI controller and an SD card in SPI mode |
| `src/lcd.c` | LCD controller, framebuffer capture |
| `src/display.c` | SDL2 window |
| `src/touch.c` | EFSIF1 touch panel, keyboard geometry |
| `src/timer.c` | 60 MHz tick timer (cascaded T16 ch0/ch5) |
| `src/itc.c` | interrupt controller registers and priorities |
| `src/cmu.c` | clock management unit, protect gate, derived MCLK |
| `src/periph.c` | ADC |
| `c33_forms.h` | **generated** decode tables |
| `c33_syscalls.h` | **generated** syscall names |
| `tools/` | table generators and ISA-fitting scripts |
| `difftest/` | differential tests against the real cross compiler |

## Regenerating the decode tables

Only needed if the ISA tables change. Requires the c33 toolchain
(`c33-epson-elf-objdump`), which is built by the top-level `make toolchain`
on a 32-bit Linux host.

```
# every 16-bit encoding, disassembled by binutils
python3 -c "import struct;open('/tmp/allinsn.bin','wb').write(
    b''.join(struct.pack('<H',i) for i in range(65536)))"
c33-epson-elf-objdump -D -b binary -m c33 /tmp/allinsn.bin \
    | grep -E '^ *[0-9a-f]+:' > allinsn_full.txt

make tables          # -> c33_forms.h
make test            # check the tables against real firmware
```

`tools/derive_fields.py` solves each operand field for
`(shift, width, signed, bias)` by grouping encodings that share a mnemonic
and operand shape. `tools/fit_ext.py` and `tools/fit_data_ext.py` fit how
`ext` prefixes compose, which is measured rather than assumed - see below.

## Notes on the ISA

The decoder is generated from binutils' own disassembler rather than from
the opcode table in `c33-opc.c`, because the two disagree. `c33-dis.c` has
`c33_opcodes` commented out and decodes with a hand-written switch; the
assembler table is missing forms the disassembler emits, such as the
`srl`/`sll`/`sra` family at `0x23xx` where the immediate is a single 5-bit
field rather than the two `IMM4` families the table lists.

Several semantics were measured against real firmware rather than assumed,
after the obvious reading turned out to be wrong. All of the following were
later confirmed against the S1C33E07 Technical Manual:

* `call` pushes the return address to the **stack**. r15 is the global data
  pointer (`__dp`), not a link register.
* `pushn %rN` saves **r0..rN**, with r0 ending at `[sp+0]`. grifo's syscall
  handler indexes that frame directly, so the order is observable.
* Two `ext` prefixes on a branch use only bits **12:3** of the first
  prefix, discarding its low three bits rather than shifting them in:
  `imm13(12:3) = sign32(31:22)`, `imm13 = sign32(21:9)`,
  `sign8 = sign32(8:1)`. Fitting this from firmware alone gave a shift of
  18 over 29 bits, which agrees on every real branch only because the
  assembler zeroes those three bits; the C33 PE Core manual gives the
  actual rule.
* `add`/`sub` zero-extend their 6-bit immediate; `cmp`/`and`/`ld.w`
  sign-extend. The assembler picks the opposite mnemonic instead of a
  negative immediate for the first pair.
* `add`/`sub %sp,imm10` counts **words**. `[%sp+imm]` scales the short field
  by the access size, but an ext-composed displacement is a plain **byte**
  offset.
* `slp` is not a halt: `CMU_initialise` uses it deliberately to switch clocks.

### Checked against the C33 PE Core manual

The PE Core manual has the encodings the S1C33E07 manual defers to, and
confirms the addressing rules derived here: `[%sp+imm6]` scales the
immediate by the access size ("for word data transfers... four times the
6-bit immediate"), an ext-prefixed `[%rb]` adds the composed immediate
directly as a byte displacement, and PC-relative branches add twice the
`sign8`. It also confirms the imm6/sign6 extension widths (19-bit with one
prefix, 32-bit with two) and that the MSB of `sign6` is data rather than
sign when a prefix is present.

It also documents the PSR layout, which had been guessed here: psrset's
imm5 "indicates a bit number, with values 0, 1, 2, 3, and 4 representing
bits 0 (N), 1 (Z), 2 (V), 3 (C), and 4 (IE)". The guess happened to be
right.

Every instruction entry's Function line was checked too -- pushn is
"repeated for rN = rs to r0", reti is "pc <- W[sp+4], psr <- W[sp],
sp <- sp+8", ld.b's ext forms use [sp+imm19]/[sp+imm32] as plain offsets,
shift counts are "0 to 31" -- and all matched.

It corrected two things. One is the two-prefix branch case above. The other
came from the per-instruction flag tables ("Flag IE C V Z N"), which were
extracted for all 57 documented mnemonics and checked against this
implementation. Everything matched -- add/sub/cmp update C V Z N while
their %sp,imm10 forms update nothing, shifts and rotates update only Z and
N, btst only Z, int clears IE, and multiplies, loads, stack operations and
branches touch no flags -- except that the logical operations are listed as
"- - 0 <-> <->": they force V to zero. That is now `set_nz_clrv`.

### Checked against the S1C33E07 Technical Manual

The manual confirms the ABI exactly as reverse-engineered here: `pushn %rs`
pushes "general-purpose registers %rs-%r0", %r0..%r3 are callee-saved, %r4
holds the return value, %r6..%r9 pass arguments, and %r15 is the default
data area pointer. It also confirms `ld.w %rd,sign6` is sign-extended,
`add`/`sub` take `imm6`/`imm10` (unsigned), `ext` carries `imm13`, branch
displacements are `sign8`, `int` takes `imm2`, software exception n is
vector 12+n relative to TTBR, and that more than two `ext` prefixes raises
an exception.

One caveat for anyone reading the manual: its prose says `cmp` takes its
immediate "zero-extended", but its own operand table lists `cmp %rd,sign6`,
and gcc emits a redundant `ext 0x0` before `xor %r6,0x30` when it wants +48
precisely because a bare 0x30 would sign-extend. The operand table is right.

`-A` turns on the address misaligned exception (vector 6): halfword and
word accesses must sit on their natural boundary. It is off by default
because it is a debugging aid rather than something the firmware needs,
but running the whole boot and a search with it enabled reports zero
misaligned accesses, which independently exercises the `[%sp+imm]` scaling
and `ext` composition rules -- getting either wrong produces unaligned word
accesses almost immediately.

Interrupts are deferred until an `ext` sequence completes, per 5.6.3:
"exception handling ... is not started for other exceptions until after the
target instruction to be extended is executed". This one bites in practice
rather than in theory. grifo's syscall return composes a 32-bit address
from two prefixes:

```
ext 0x200 ; ext 0x353 ; ld.w %r0,0x2c    ->  0x1000d4ec <saved_pc>
```

A touch interrupt landing between the two prefixes used to discard the
first, so the load came from `0xd4ec`, read as zero from unmapped memory,
and the indirect `ret` that follows jumped to address 0. It needed a real
keypress at exactly the wrong cycle, which is why a headless boot never
showed it. `make test-irq` now pins it deterministically.

Known divergences from the manual, none of which the firmware exercises on
the boot path: `slp` resumes immediately rather than halting until an
interrupt, `halt` stops the emulator outright, traps are not masked between
a `.d` branch and its delay slot, and `jpr`, `swap`, `swaph`, `adc`, `sbc`
and the coprocessor instructions are unimplemented (none appear in any of
the four firmware images).

### Interrupt priority

The PSR's IL[3:0] field (bits 11-8) is modelled per the PE Core manual: a
maskable request is accepted only when its priority is *strictly greater*
than IL, and IL is then raised to that priority until `reti` restores the
saved PSR. `make test-irq` checks all 64 (IL, priority) combinations plus
the IE gate, the saved-PSR contents and the pushed return address.

Priorities come from the interrupt controller's own registers rather than
being assumed. `src/itc.c` backs REG_BASE+0x200..0x2ff with a register file,
which the drivers need anyway because they read-modify-write it --
grifo's `CTP_initialise` does `REG_INT_PSI01_PAD |= SERIAL_CH1_INT_PRI_7`,
so a register that read back as zero would silently drop the neighbouring
field. Serial ch0's priority is bits 6:4 of 0x26a and ch1's is bits 2:0;
the touch panel therefore runs at priority 7, which the emulator now reads
out of the register the firmware wrote instead of hard-coding.

Sources whose priority register is not decoded report 7, so they are never
masked -- the previous behaviour. In a normal boot nothing is masked in
practice, because only one source is ever pending and IL is back to 0 by
the time the next packet arrives.

### LCD controller

The panel is composited through `lcd_pixel()`, which the PGM writer, the
ASCII dump and the SDL window all share.

The main window is read from `MADD` with a line stride of `MLADD` words.
The manual's own worked example is this exact device: "if the LCD width and
image width are 240 pixels in 1-bpp mode, MWLADR[9:0] = 240 x 1 / 32 = 7.5
[words]. In this case, MWLADR[9:0] must be set to 8. Furthermore, the image
must be prepared in 256 (8 x 32) pixels wide." That is where the padded
32-byte stride comes from, and it agrees with grifo's `LCD_BUFFER_WIDTH`.

The Picture-in-Picture Plus sub-window is now composited: when `PIPEN`
(SSP bit 31) is set, `SADD` replaces the main window inside the rectangle
given by `SSP`/`SEP`. This is how grifo's `LCD_Window` draws popups, and it
was previously not modelled at all -- the emulator rendered only `MADD`, so
a window would simply not have appeared. In 1-bpp mode the X registers count
32-pixel words while Y counts lines, and the sub-window has no line-offset
register of its own: its stride is its own width, `PIPXEND - PIPXST + 1`
words, which is exactly what grifo assumes.

`make test-lcd` drives the registers the way `LCD_Window()` does, writes
pixels the way `WindowPos()` does, and checks all 128x60 of them composite
where grifo put them, that the window does not leak outside its rectangle,
that clearing `PIPEN` restores the main window, and that `MLADD` drives the
stride.

The manual has a typo here worth knowing about: its example prints
"PIPYEND[9:0] = 60 + 120 lines -1 = 180 lines (= 0xB3)", but 0xB3 is 179,
which is what its own formula gives and what grifo computes.

The firmware writes no LCDC register at all on the boot path -- grifo takes
the framebuffer address from a linker symbol, and the bootloader has already
programmed the timing, mode and power registers before `kernel.elf` is
entered. That is why `MADD` is pre-loaded at attach and why `PS`/`DMD` are
not interpreted: modelling `PSAVE` from a register that reads as its reset
value of zero would blank a panel the hardware has running.

### Serial and SPI status registers

Both status registers were checked field by field against the manual.

EFSIF (0x300Bx2) has one trap in it: `TENDx` (D5) is called the
"transmit-completion flag", but 1 means transmission is *in progress* and 0
means it finished. `suspend.c` tests `if (0 != (STATUS & TENDx))` precisely
to catch a transmit still running, so reporting 0 is what says "idle". The
FIFO-occupancy field `RXDxNUM` (D[7:6]) is now reported too -- 0 encodes
"1 or 0" bytes, then 2, 3, 4 -- though nothing in the firmware reads it.
The error flags `FERx`/`PERx`/`OERx` stay clear, and the writes the drivers
label "clear errors" are accepted; neither of these links can frame,
parity or overrun.

SPI (0x301714) matches the manual on offsets and bit positions. `BSYF` (D6)
must read 0 or `sd_spi.c` spins forever: it brackets every byte with
`while ((SPI_STATUS & 0x40) != 0)`, and a transfer here completes inside
the store to TXD. `MFEF` cannot occur with a single bus master. `RDOF` (D3)
is now modelled -- set when TXD is written while a byte is still unread,
cleared by reading RXD -- and reported at exit. A full boot and search does
1831 commands with zero overflows, which independently confirms the driver
reads RXD after every exchange.

### A/D converter

This one had a real bug. The S1C33E07 has a "5-ch. 10-bit A/D converter"
and grifo agrees -- `analog.c` defines `ADC_FULL_SCALE` as 1024 -- but the
device returned 0x800 on every channel, which is twice full scale. Run
through grifo's own conversions that comes out as a battery of 9.2 V and a
temperature of **-154 C**, and wiki.app puts the temperature on screen
(`wiki/keyboard.c:617` reads `ANALOG_TEMPERATURE_CENTI_CELCIUS`).

Each channel now returns a count derived by inverting grifo's formulas, so
the numbers the firmware computes are physically sensible: 832 on ch0 gives
2799 mV of battery (`samo_a1.h` calls 3000 mV full and 2250 mV low), 502 on
ch1 gives 19.96 C, and 512 on ch2 gives a 23.5 V STN bias.

The channel status register is modelled rather than wired to "always done".
`ADFx` is raised by a conversion and, per the manual, "reset to 0 when the
converted data is read"; `OWEx` in the high half flags a sweep landing on
unread data. The sweep covers `CS[2:0]` to `CE[2:0]` from `TRIG_CHNL`, which
grifo programs as 0x1000 -- channels 0 to 2, exactly the three `ScanADC`
reads. That agreement is a cross-check in itself: a full boot does 13
conversions with zero overwrite errors, which would not hold if the sweep
range and the read set disagreed.

`make test-adc` runs grifo's conversions over the presented counts and
checks the results land in physically sensible ranges, plus the flag
behaviour and the sweep range.

### Clock management unit

CMU writes used to be swallowed. Two things made that wrong.

`CMU_enable1()` does `REG_CMU_GATEDCLK1 |= mask`, so a register reading back
as zero silently turns off every clock enabled earlier -- the same
read-modify-write hazard as the interrupt priority register. And writes are
gated: `CMU_PROTECT` takes 0x96 to unlock and 0x00 to lock, and every driver
brackets its accesses with that pair. `src/cmu.c` now backs the block with a
register file and honours the gate, so a missing unlock shows up as a
rejected write instead of taking effect anyway.

The clock tree is still not simulated -- there is one instruction stream --
but the configuration is decoded, which lets the emulator's timebase be
checked instead of assumed. grifo programs `OSCSEL_PLL` with the PLL at
48 MHz / 8 x 10, and `cmu_mclk_hz()` reads **60 MHz** back out of the
registers the firmware actually wrote. That is exactly the 60 MHz that
`Tick_TicksPerMicroSecond` and `TIMER_CountsPerMicroSecond` assume and that
`CYCLES_PER_TICK` in `src/timer.c` is scaled to, so the timebase is now
derived from the hardware configuration rather than taken on faith.

A full boot makes 17 CMU writes with **zero** blocked, which is the check
that the protect polarity is the right way round -- had it been inverted,
all 17 would have been rejected.

`make test-cmu` replays grifo's register values and checks the derived
frequency, the protect gate in both directions, and that a
read-modify-write preserves previously enabled clocks.

### Peripherals still taken on trust

The peripherals above have been checked against the S1C33E07 register
descriptions. What has not: the T16 timer block beyond the two count
registers `Tick_get` reads, the SD card's own command set (which is an SD
Association spec, not an Epson one), the port/pin configuration registers,
and the DMA and RTC blocks the firmware never touches. These were written
from the driver sources in samo-lib and from what the firmware demanded.

`make check` runs the decoder comparison against binutils plus the four
peripheral test programs.

## Caveats

Running the firmware proves it is self-consistent under this model of the
ISA, not that it would boot on hardware. Taken alone that would be close to
circular: the emulator was built by inferring semantics from the same
binaries it runs, so a misreading shared between the two would not show up.

Three checks are independent of that inference.

* The **decoder** is validated instruction for instruction against binutils'
  own disassembler over all four firmware images - 65,605 instructions,
  exact match.
* The **manuals**: every documented mnemonic's flag table and Function line
  was checked against this implementation, along with the register
  descriptions for the interrupt controller, LCDC, serial, SPI, ADC and CMU.
* The **cross compiler**, via `difftest/`: the same C source compiled by
  c33-epson-elf-gcc 3.3.2 and by the host compiler, run both ways, outputs
  diffed. 180 random programs across five optimisation levels match value
  for value. The compiler has never seen the emulator and the emulator has
  never seen its output, so agreement is evidence rather than consistency.
  The one divergence that survived reduction turned out to be a **gcc 3.3.2
  bug** - it discards a narrowing signed cast when reassociating a multiply.
  It is fixed in `host-tools/toolchain-patches/0008-*` and written up in
  `difftest/compiler-bugs/`; the firmware never triggered it.

Between the firmware and the differential tests, 57 of the 68 implemented
opcodes are known to execute (`wremu -P`). See `difftest/README.md` for what
the remaining 11, and the 36 unimplemented opcodes, actually are.

## Speed

About 80M instructions/sec, a little under 2x the real device. Getting there
was three changes, all found by profiling rather than by guessing:

* **No text in the execute path.** Operand shapes are interned to integers
  by the table generator. They used to be compared with `strcmp` against the
  shape string objdump prints -- a disassembler artifact reused as the
  semantic discriminator. Since forms are tested as an if/else chain, a
  common instruction like `ld.w` walked several string compares on every
  execution, and it profiled as the single hottest thing in the interpreter.
  The instruction word fully determines the semantics; nothing about that
  should involve English. The `ld.w` special-register forms were the last
  holdout -- they were recognised with `strncmp` prefix tests -- and are now
  classified into a `sreg` field when the tables are built.

### Two authorities for the decoder

The 65,536-entry table is not a claim that the ISA has 65,536 instructions.
It has 231 forms; the table is a flattened lookup from instruction word to
form index, which for a fixed-width 16-bit encoding is one load instead of a
mask-and-compare cascade. 64,448 words map to something and 1,088 are
invalid, which is itself a statement about how dense the encoding is.

The real problem with deriving it from binutils was different: binutils was
the *only* authority. A bug in its disassembler would be reproduced here
exactly, and "matches binutils on 65,605 instructions" could never detect
it. The PE Core manual documents each instruction's encoding as a bit
diagram plus a hex pattern with don't-care nibbles -- `add %rd, %rs` is
`0x22__`. `make test-manual` reads those out of the PDF and checks the table
against them: **90 documented forms, 90 agree, 0 disagree.**

That covers the opcode structure. It does not cover operand field extents,
which come from the field solver and are checked against binutils' operand
printing instead -- so the two authorities overlap rather than nest.

### What binutils is and is not used for

It is a **generation-time** authority, not a runtime dependency. The tables
were derived by disassembling all 65,536 encodings and solving each operand
field for `(shift, width, signed, bias)`, because the ISA documentation is
incomplete and binutils' own assembler table disagrees with its
disassembler. That derivation is also what validates the decoder, exactly,
on 65,605 instructions. The generated header is committed, so building the
emulator needs no toolchain.

What was wrong was letting the *shape of that derivation* reach the
executor. Two vestiges are now gone: the `strcmp` discrimination above, and
a `bias` field carried in every operand descriptor -- the solver looks for
one, and the answer is zero for all 330 fields in the ISA, so it was pure
residue of the discovery process. The generator asserts that invariant now
instead of paying for it at runtime.

`struct c33_form` went from 48 bytes to 24 as a result. That did **not**
make anything faster -- the table was 11K, comfortably cached either way --
but it is the right shape: an executor wants form index to semantics, not a
printer's decomposition into mnemonic plus operand syntax.
* **A region cache for fetch and load.** Every access went through an
  indirect call into `mem_read` and a walk of the memory map. Caching the
  region the PC or the data pointer currently sits in turns the common case
  into a bounds check and a load.
* **A byte-wide decode table.** `c33_form_of` was `uint16_t[65536]`, exactly
  128K, exactly the L1 data cache on the machines this runs on, so every
  decode thrashed it. There are only 231 forms, so a byte does.

One thing that turned out not to matter: `-O3` and `-mcpu=native` are within
noise of `-O2`.

Timing is not wall-clock paced, but it is cycle-based rather than
per-instruction: each instruction charges the MCLK cycles given by its CLK
line in the C33 PE Core manual, and the tick timer counts those. Real
firmware runs at about 1.36 cycles per instruction. Per-form variation
within a mnemonic is not modelled, and `ext` is charged one cycle where the
manual says "zero or one depending on the instruction queue status", so
elapsed time errs slightly long.

With a window the tick comes from wall-clock time instead of the
instruction count (`timer_use_wallclock`). Headless runs keep the
cycle-derived tick, which is deterministic and reproducible, but under a
human's hand it is wrong: this build runs at about 0.75x real time, so a
one-second drag looks like 0.75 s to the firmware. wikilib derives
`finger_move_speed` as pixels per tick, so the scroll momentum came out
inflated by the same factor -- and since the ratio moves with host load and
with what the firmware is doing, the fling felt inconsistent rather than
merely fast. Interactively the user's seconds are the ones the application
should be measuring.

Emulated seconds now pass fast enough to reach the application's idle
behaviour, which is worth knowing but is less dramatic than it sounds.
After two idle seconds `wikilib_run` calls `history_list_save` at the
NORMAL level, and after five at the POWER_OFF level -- that is a *save
level*, not a shutdown, meaning "save thoroughly, as you would before
losing power". It then blocks in `event_wait` until something happens.
The real `power_off()` is reached from exactly one place, a physical
BUTTON_POWER release, so an idle device sits there with the page still on
screen. Measured: screen content is unchanged from 200M through 1.6B
instructions, about 35 emulated seconds.
