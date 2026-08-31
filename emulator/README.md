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

The window opens with the device **off**, as it would be sitting on a
shelf: press **P**, or click the power symbol, to turn it on.

Click keys with the mouse; that is the touch panel. Below it is the bezel,
laid out like the device: a WikiReader wordmark and three round buttons
reading **search**, **history**, **random** left to right. Click them, or
use keys **1**, **2**, **3** in that same order. To their right is a small
power symbol -- clickable, or **P** -- drawn smaller and set apart because
on the case it is on the edge rather than the bezel. `Q` or `Esc` quits.

| flag | meaning |
| --- | --- |
| `-g`, `-S N` | SDL2 window, scale factor (default 3) |
| `-c FILE` | attach a FAT32 card image |
| `-R` | open the card image read-only, so a run cannot change it |
| `-e FILE` | attach the serial FLASH and boot through it, as the hardware does |
| `-n N` | stop after N instructions (unlimited with `-g`) |
| `-s` | trace grifo syscalls by name, with call sites and return values |
| `-K cycle,TEXT` | type TEXT on the on-screen keyboard |
| `-T x,y,cycle` | tap a pixel |
| `-N code,cycle` | press a button: 0 random, 1 search, 2 history, 3 power |
| `-G x,y0,y1,cycle` | drag vertically, for the scroll path |
| `-b ADDR` | breakpoint: registers plus recent PCs |
| `-W ADDR` | write watchpoint |
| `-V VAL` | watch stores of a byte value |
| `-D ADDR -L N -O FILE` | memory dump, optionally to a binary file |
| `-t N` | disassemble the first N instructions |
| `-m` | trace unclaimed MMIO registers |
| `-P` | histogram of opcodes actually executed |
| `-H` | histogram of where time is spent, by address |
| `-X ADDR[,NAME]` | count entries to ADDR without stopping, and report the longest gaps between them |
| `-Y A,B` | profile only between the first hits of A and B |
| `-y M,N` | profile only between guest times M and N, in ms |
| `-F FILE` | write every non-empty profile bucket to FILE, for diffing two runs |
| `-Z ADDR` | time the whole input script from the first hit of ADDR |
| `WREMU_SUSPEND_DIV=N` | (env) divide the 120 s suspend timeout by N |

`-s` is usually the fastest way in: it turns a hang into a named syscall,
a call site and a return value.

### Measuring, without fooling yourself

Comparing two builds of the same firmware needs more care than it looks.

**`-n` is not a stopwatch.** It counts `cpu.cycles`, which the headless idle
path also advances when it fast-forwards to the next deadline. A run that
waits more looks like a run that computed more. The summary separates them:

```
--- work: 219633709 instructions executed, 180366291 idle, 7973.0 ms guest ---
```

**Absolute cycle numbers are not a fair script.** `-T x,y,cycle` fires at an
instruction count, so a build that boots in fewer instructions gets the tap
delivered at a different point in its own progress, and the two runs are no
longer the same interaction. `-Z ADDR` rebases the whole script -- `-K`,
`-T`, `-G`, `-N` -- onto the first time the guest reaches ADDR.

**Call counts are not work.** The idle loop here busy-spins until a 2 s
suspend timeout, so a *faster* build racks up *more* calls to `Event_get`
and everything the poll loop touches. What that costs the person holding the
device is the gap between polls, which `-X` reports:

```
--- probe Event_get   257853 hits  first ... last ... ---
      stalls:  201.4ms(8400k@4794ms)  130.3ms(5322k@2794ms)  21.6ms(959k@2061ms)
```

**Whole-run profiles say nothing.** The hot code over a whole run is always
the idle loop. `-Y A,B` and `-y M,N` restrict the profile to one phase, and
`-F` writes every bucket so two runs can be diffed function by function --
which is how a 25% regression in an article load was traced to a single
missing addressing mode in `memset`.

## Booting the way the hardware does

`./wremu -e flash.rom -c card.img` starts where the device starts, instead
of loading `grifo.elf` straight off the host filesystem.

The real chain has four stages before any application runs:

| stage | lives in | |
| --- | --- | --- |
| mask ROM | burned into the S1C33E07 | Epson's, not in this repo |
| `mbr` | serial FLASH offset 0x1 | `samo-lib/mbr/mbr.c`, linked at 0 |
| `menu` | serial FLASH 0x300 | boot menu, auto-boots on timeout |
| `file-loader` | serial FLASH 0x2300 | reads `kernel.elf` off the card |

Only the first cannot be run: it is silicon we do not have, so the emulator
reproduces its *effect* -- copy the first 512 bytes of the FLASH to RAM 0,
set the stack to the top of internal RAM, jump to 0. Those numbers are not
guesses: `SAMO_A1.mapfile-default` places `mbr` at FLASH offset 1,
`mbr.elf` is linked with `-Ttext=0`, and nothing in samo-lib sets up a
stack before mbr's first call, so the ROM must.

Everything after that is real firmware. Building the FLASH image needs

```
make AWK="python3 samo-lib/mbr/GenerateApplicationHeader.py" mbr
```

which produces `samo-lib/mbr/flash.rom`. The `AWK` override is because the
original header generator uses `gensub()`, a gawk extension;
`GenerateApplicationHeader.py` is a drop-in that needs only Python. The
image assembler `host-tools/flash07/image07` was Python 2 and has been
ported.

This required modelling three things the ELF path never touched: the I/O
ports, because the SPI chip selects live there (`port.c` -- SD is port 5
bit 0, FLASH is bit 2) and the drivers set them with read-modify-write; the
serial FLASH itself (`eeprom.c`, a PM25LV512); and the SDRAM controller
(`sdramc.c`), because the boot path spins on its initialise flag.

The whole chain works. `-n 300000000` reaches the rendered keyboard, in a
bit over two seconds:

```
load: kernel.elf
Grifo starting
init starting
starting wiki app
VERSION: 20260823
```

Before that the boot menu comes up over the serial console with live
readings from the emulated ADC, and auto-boots on timeout as the hardware
does:

```
BAT: 3079 mV      TMP: 19 DegC      LCD: 23462 mV
menu? -\|/...
```

### Where the stack goes, and why it matters

Getting this working came down to one number the manual does not give.

The boot flowchart (D.4.2) is explicit about everything else: read the
status register, issue READ with a 32-bit address, load 512 bytes to the
start of A0RAM, jump to 0. It says nothing about a stack -- and the PE Core
manual says SP "becomes indeterminate when it is initialized upon reset",
with software expected to set it. But `mbr` never does: `master_boot`
begins `pushn %r3`, using whatever it was given. So the mask ROM must leave
a working stack, and the emulator has to pick the same one.

The top of A0RAM is the obvious guess and it is wrong. `mbr` loads every
application with `FLASH_read(0x200, EEPROM_PAYLOAD_SIZE, ...)`, which writes
0x200 to 0x1F00 regardless of how big the application actually is, leaving
256 bytes below an 8K stack top. FatFs alone needs more than that:
`FATFS` embeds a 512-byte sector window, and `elf32_exec` puts one on the
stack. The symptom was a `dirbase` of 0x80409 -- an address, not a cluster
number -- and a read of sector 2,151,983,104.

The stack lives at the top of IVRAM instead. That is internal memory,
available before SDRAM is initialised, and does not collide with the
application A0RAM is full of.

### The offset-by-one

The FLASH map places `mbr` at EEPROM offset 1, not 0, which looks like a
mistake until the manual explains it: "The SPI-EEPROM boot sequence issues
a 32-bit address regardless of the EEPROM size." The PM25LV512 takes a
three-byte address, so it begins clocking out data during the fourth
address byte, and the boot ROM discards it. The 512 bytes it keeps start at
offset 1.

### Suspend, and the 27x it was worth

The chain first took about 8 billion instructions where the ELF path
reached the same screen in 600 million. It now takes **300 million**, and a
full hardware boot runs in a bit over two seconds.

The cost was not I/O: both paths did nearly identical card traffic, 587
commands against 538. It was not the boot stages either -- `-H` put 98% of
the time inside grifo, not in mbr, menu or file-loader. At 64-byte
resolution the map named the functions:

```
vuprintf 24%   Event_get 22%   ELF32_load 16%   Event_wait 14%   Suspend 15%
```

`Event_wait`, `Suspend` and `Event_get` are the idle loop, and they were
half the total. The device was spinning where it should have been asleep,
because of this, at the top of `Suspend`:

```c
// if in CTP receive sequence
if (0 == (REG_P6_P6D & 0x10)) {
	return;
}
```

Port 6 was initialised here to all zeros so the three buttons on bits 0..2
would read as not held. But `REG_MISC_PUP6` in `boards/samo_a1.h` enables
pull-ups on bits 3, 4 and 5, so those idle **high**. With bit 4 low,
`Suspend` concludes a touch packet is arriving and returns immediately,
every time, and the idle loop runs flat out instead of suspending.

Setting bit 4 high is the accurate thing and gives the 27x. It also makes
the machine deaf until the rest of suspend is modelled, which took five
more pieces. Each one hid the next:

* `halt` now parks the core until an interrupt instead of stopping the
  emulator, and wakes on a request **regardless of `IE`**, which is what
  the manual specifies -- "interrupt signals are able to cancel HALT and
  SLEEP modes even if the IE flag in PSR or the interrupt enable bits in
  the interrupt controller are set to disable interrupts". That matters
  because `Suspend` disables interrupts before halting.
* `SELDO` in the SDRAM refresh register is modelled, because the relocated
  suspend code enables self-refresh and spins until it reads back.
* 16-bit timer 2 is modelled as a wake source, because that is what the
  suspend path arms before halting.

* The **cause-of-interrupt flag registers** at 0x280..0x28f are
  write-1-to-clear, not storage: "The flag that has been set can be reset by
  writing". Treated as storage, the resume path's own attempt to clear the
  timer flags set them instead, so the firmware always concluded it had
  timed out and called `System_PowerOff()`.
* Interrupt delivery is **gated on the controller's enable bits**, and a
  cause that is disabled is refused at the point it is raised rather than
  when it is taken. The resume path disables and clears the timer 2
  interrupt before re-enabling interrupts, and delivering it anyway landed
  in grifo's "Panic: undefined interrupt". Refusing it late was not enough
  either: there is one pending slot, so a doomed request would displace a
  live one and take it down with it, which is how a stray timer wake-up ate
  the touch interrupts.
* The wake timer runs from **OSC3/32**, because the suspend code switches
  the clock down before arming it. That is the other factor in the
  firmware's own reload, `(MCLK / 32 / 4096) * seconds`. Modelling only the
  4096 made the timeout fire 32 times early, so the device decided it had
  been idle for two minutes and powered off.

The lesson from the port bug still stands, twice over: the reset state of an
I/O port is a property of the board, not a convenient default. The manual
lists these registers as "Ext." precisely because they read the external
pin, and the pull-ups are in the board header. The second lesson is that a
register can be more accurate in isolation and still be a regression, until
the rest of the model catches up -- this one was a regression for several
hours before it became a 27x speedup.

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
* `slp` follows CMU `WAKEUPWT`: clock-switch mode auto-wakes, while ordinary
  SLEEP mode waits for a wake source. `CMU_initialise` uses the former.

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

Cold reset also installs the architectural register values from sections
2.5, 2.7, and 2.8: TTBR is `0x00c00000`, IDIR identifies a PE core with
type byte `0x06`, and the read-only DBBR is `0x00060000`. The model/revision
byte in IDIR is left zero because the S1C33E07 manual does not specify it.
Special-register transfers enforce the same register definitions: unused
PSR bits read as zero, SP stays word aligned, TTBR stays 1K aligned, IDIR,
DBBR, and PC ignore writes, and reading PC produces the address immediately
after the `ld.w` as section 2.2 specifies. Encodings for special registers
which do not exist on PE execute as no-ops rather than exposing the decoder's
all-core register names.

One caveat for anyone reading the manual: its prose says `cmp` takes its
immediate "zero-extended", but its own operand table lists `cmp %rd,sign6`,
and gcc emits a redundant `ext 0x0` before `xor %r6,0x30` when it wants +48
precisely because a bare 0x30 would sign-extend. The operand table is right.

The address misaligned exception (vector 6) is architectural: halfword and
word accesses must sit on their natural boundary, so it is always enabled.
`-A` remains accepted for command-line compatibility. A full boot and search
report zero misaligned accesses, independently exercising `[%sp+imm]`
scaling and `ext` composition -- getting either wrong produces unaligned
word accesses almost immediately. A rejected access leaves its destination
and post-increment register untouched, as required by 6.3.5's rule that the
faulting instruction is retried after `reti`.

Synchronous processor exceptions have their own entry path rather than
being promoted to priority-15 hardware interrupts. They bypass IE, IL, and
the interrupt controller; push PC and PSR; clear IE without changing IL;
and fetch their handler from TTBR. `make test-exception` checks that frame,
the different saved PCs for alignment, undefined-instruction, and third-
`ext` exceptions, and IDIR's recorded instruction. It also checks the nine
instructions which Table I.5.3.5 says PE removed (`div0s` through `div3s`,
`mac`, `mirror`, `scan0`, and `scan1`): their old-core encodings correctly
take the undefined-instruction vector on PE.

The generated decoder contains the union of binutils' STD, ADV, and PE
tables. The executor separately rejects all 18 ADV-only operations (`div.w`,
the extended MAC/multiply family, `loop`/`repeat`/`retm`, and saturating
arithmetic) through that same undefined-instruction path. This distinction
matters because recognizing a word as an ADV mnemonic is not evidence that a
PE processor can execute it.

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

`make test-isa` covers the documented operations which none of the four
firmware images happen to execute: carry/borrow and overflow edge cases for
`adc` and `sbc`, both byte-order swaps, immediate and delayed `jpr`, delayed
`ret`, the valid and reserved `pushs`/`pops` forms, and the `brk`/`retd`
debug-exception path. The latter checks the fixed debug save area and that
ordinary interrupts remain pending throughout debug mode, as required by
section 6.5.

Known divergences from the manual, none of which the firmware exercises on
the boot path: illegal delay-slot instructions have no explicit unstable-
state model, and the coprocessor instructions are unimplemented because no
coprocessor is attached.

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

### Powering off

The power switch is not one of the three front buttons. It is P03 with its
own port interrupt, vector 19, rather than a member of the key comparator
the others share, so it is modelled separately. In the window it is the
small power symbol to the right of the three, deliberately unlike them
because the case puts it on an edge; it can also be pressed with **P** or
`-N 3,cycle`.

A tap is enough; there is no press-and-hold. `Button_PowerInterrupt` queues
a `BUTTON_DOWN` and a `BUTTON_UP` together from the single falling edge, so
the application sees a complete press and release however briefly the
switch is touched, and nothing anywhere measures duration. The pin is
active low and edge triggered (`REG_PINTPOL_SPP07` clears SPPT3,
`REG_PINTEL_SEPT07` sets SEPT3), so it idles high and pressing pulls it
down.

What happens next is the interesting part. `power_off()` in
`boards/samo_a1.h` does not stop the processor -- it drives P63 as an
output and toggles it forever, expecting circuitry outside the chip to
notice and cut the rails:

```c
for (;;) {
	REG_P6_P6D |= (1 << 3);              /* P63 high */
	for (i = 0; i < POWER_OFF_CYCLES; ++i) asm volatile ("nop");
	REG_P6_P6D &= ~(1 << 3);             /* P63 low  */
	for (i = 0; i < POWER_OFF_CYCLES; ++i) asm volatile ("nop");
}
```

Nothing in software ever returns from that. Before this was modelled the
emulator simply ran the loop, which is why an unattended run used to sit at
full CPU indefinitely: `SUSPEND_AUTO_POWER_OFF_SECONDS` is 120, so after
two idle minutes the firmware shuts down and the emulator spun on the
toggle from then on. `src/port.c` now recognises P63 being driven and
toggled, which is what the hardware does.

That path is reachable both ways: pressing the power switch, which makes
wiki.app save its history and call `power_off()`, and the idle timeout. A
run left alone reaches it at about 132 emulated seconds, and takes under
two seconds of real time to get there.

### Letting go outside the panel

Dragging to scroll and releasing outside the window used to work in one
direction only. Scrolling the text down means dragging upwards, so the
pointer leaves by the top edge; scrolling up means dragging downwards, and
it leaves by the bottom, over the bezel. The mouse is captured either way,
so the release does arrive -- but the handler dropped any mouse event below
the panel as "not a touch", which is right for a press and wrong for a
release. The finger was never lifted, so the scroll held the drag instead
of coasting.

A release now ends the touch wherever the pointer has got to, and
coordinates are pinned to the edge of the glass, since a panel cannot
report a position it does not have.

This is the second bug in the window rather than in anything the guest can
see, and scripted runs could not have caught either: `-T` and `-G` call
`touch_post()` directly, so nothing between an SDL event and that call was
ever exercised. `display_handle_event()` is now split out of the poll loop
and `make test-display` drives it with synthetic events -- no window, no
video device -- covering the release-off-panel case, the clamping, and the
counted power press.

### The watchdog

It was the busiest thing in the register map long after every other
peripheral had been modelled: 118,882 writes in one session, all landing in
an unclaimed hole. grifo arms it for twenty seconds with `RESEN` set and
then kicks it from the main loop, the suspend path and around every card
access. Unmodelled, a guest that wedges sits there forever; on the device
the chip resets.

Two things make it more than a register file. `REG_WD_WP` has to hold 0x96
before `COMP` or `EN` will take a value, which is how `Watchdog_SetTimeout`
brackets its writes. And the counter is gated: `WDT_CKE` is deliberately
absent from the set of clocks the suspend code enables, so it stops for the
whole two-minute suspend.

That gate is the part worth stating plainly, because the obvious model is
wrong. Counting as "now minus the last kick" reads correctly at every
instant the clock is on, and quietly banks the entire suspend -- the first
poll after the clock comes back sees two minutes of arrears against a
twenty-second timeout and resets a device that was behaving perfectly. The
counter accumulates only while the clock runs. `make test-wdt` covers it;
switching back to the subtraction fails three of its cases.

### The card keeps what is written to it

A card that forgets everything the moment the power goes is not a card, and
the guest has things worth keeping: `history_list_save()` writes `wiki.hst`
whenever the device is switched off and the history has changed. So the
image is opened for update and block writes go straight through to it.

grifo is built with `FATFS_MODE = read-write`, and `mmc_disk_write` uses
CMD24 for one block and CMD25 for a run of them, each block sent as a start
token, 512 bytes, two CRC bytes and then a data response from the card:
0x05 accepted, 0x0d write error. `-R` opens the image read-only and answers
0x0d, which is the honest reply -- the guest sees the failure rather than
losing the data quietly. The regression runs use it so a sweep cannot
change the reference image out from under the next one.

The trap in the write path is that block data is arbitrary. A byte in the
middle of an article can have bit 7 clear and bit 6 set, which is exactly
the shape of a command frame, so the data phase has to be recognised before
any command sniffing or a file writes itself into nonsense. `make test-sd`
writes blocks composed entirely of command-shaped bytes and reads them back
off the host file to keep that honest; putting the check back in the wrong
order fails eight of its cases.

### One read that goes nowhere

Every session makes two four-byte reads of 0x00de0d74, which is outside
every region. It is a firmware bug, not a hole in the memory map.

The instruction is `xld.w %r5,[%r4+0x4]` in the application, the tail of
`wiki_list[aActiveWikis[i].WikiInfoIdx].wiki_id`. Both base pointers are
sound and `i` is bounds-checked against `nWikiCount`, but the index stored
*inside* `aActiveWikis` is not checked at all, and at that moment it holds
-3816505: `aActiveWikis` comes from `memory_allocate()`, which does not
zero what it hands back. The computed address lands far below RAM. The
emulator returns 0, the comparison fails, and the application carries on,
which is why nothing looks wrong.

Unmapped accesses now name the instruction that made them. They report
`cur_pc` rather than `pc`, since the latter has already moved on and naming
the wrong instruction is worse than naming none.

### Nothing to show until the controller says so

Powering back on flashed the previous screen for a moment. Two reasons, both
of them the emulator being less careful than the board.

The framebuffer is ordinary RAM, and cutting the power loses it, so
`machine_power_on()` clears every RAM region before placing the boot image.
And the panel is not driven until the controller is told to drive it:
`LCD_initialise` (`samo-lib/drivers/src/lcd.c`) parks `REG_LCDC_PS` in
`PSAVE_POWER_SAVE`, programs the timing and the framebuffer address, and
only then selects `PSAVE_NORMAL`. Until that last write there is nothing on
the glass, so the window now draws the same blank grey it uses for an off
device. An off machine and an uninitialised controller look alike because
neither is refreshing the panel.

### Off is a state, not an exit

Powering a device off does not make it stop existing, so with a window open
the emulator does not exit either. It marks the machine off, blanks the
panel to the flat grey of an LCD with nothing driving it, and keeps
pumping events. Pressing the power switch again calls `machine_power_on()`,
which resets every device, re-places the boot image -- the mask ROM copy
out of the serial FLASH under `-e`, otherwise the ELF -- and restarts the
core at the entry point. So a window comes up **off**, exactly like a
device on a shelf, and the first thing to do is press **P**.

Headless runs have nobody to press the switch, so they come up powered and
still end at power-off. Every scripted test depends on that.

Two things about that switch are easy to get wrong, and both were. The
off-state check has to be the **first** thing in the run loop: the periodic
event pump further down hands whatever the window collected to the port, so
with the check below it a press was delivered to a machine that was not
running and discarded, and the button appeared dead. And a press cannot be
sampled, only counted -- a quick click's down and up can arrive in the same
poll, which leaves `button_pressed` false. `display.c` counts press edges in
`power_presses` and the loop consumes the count, so no press is lost. The
count is resynchronised when the firmware powers the device off, or the
press that caused the shutdown would turn it straight back on.

Getting this right needed one more fix. `c33_reset` used to clear the whole
CPU structure and then restore, by hand, the few fields that are host-side
wiring rather than machine state. That list was wrong three times, and the
third time cost a real bug: the first power-on wiped `irq_enabled`, the hook
that asks the interrupt controller whether a cause is still enabled, so the
timer 2 wake-up that ends a suspend was delivered instead of withdrawn. It
landed in grifo's default vector and printed `Panic: undefined interrupt`.
The struct now has a `reset_barrier__` marker: reset zeroes the machine
state above it and never touches the wiring below, so a field added later
cannot be silently lost.

The suspend timeout is two minutes, which is a long time to wait when the
thing being debugged is at the far end of it. `WREMU_SUSPEND_DIV=12` divides
the span the firmware programs into timer 2, turning it into ten seconds,
without changing a byte of the guest.

### Why the panel reports on change, not continuously

Worth knowing before "improving" the touch model.

An earlier version streamed a packet on every poll while the mouse was
held, reasoning that a finger sitting still must keep reporting so the
scroll momentum decays to zero. That is wrong, and it silently breaks
tapping links inside an article.

`wikilib` arms a link with `set_article_link_number()`, which resets its
activation timer on **every** touch event, and `check_invert_link()` will
not promote the link to activated until `LINK_ACTIVATION_TIME_THRESHOLD`
(0.1 s) has passed without one. A stream re-arms that timer forever, so no
link ever activates and the release does nothing. Search and the keyboard
keep working, which makes it look like a link-specific bug rather than a
touch one.

The hardware cannot stream either, which is the clinching argument: six
bytes at `CTP_BPS` (9600), eight data bits with start and stop, is 6.25 ms
per packet, so even back-to-back packets on a real device would break the
same 0.1 s threshold. The panel must go quiet when nothing moves.

Momentum still works, because the speed is computed on release from the
last few recorded positions and the time since them -- pausing before
letting go produces a small number without needing any packets to say so.

That 6.25 ms is also enforced as a floor on how fast the emulator will emit
packets. Events are deferred rather than dropped.

### The front buttons

The three buttons are P60..P62, and they do not simply appear in a port
register: the firmware arms a key-input comparator and takes an interrupt.
`REG_KINTCOMP_SMPK0` selects which bits participate, `SCPK0` holds the
state last seen, and KINT0 (vector 20) is raised whenever the two stop
matching. grifo's handler re-arms by writing the current state back, so a
held button does not retrigger.

`src/port.c` models that, and the interrupt controller knows KINT0's enable
(EK0), flag (FK0) and priority bits.

In the window they are round buttons on a bezel below the panel, in the
order they appear on the case: search, history, random. That is worth
stating because it is **not** grifo's numbering, which is 0 random, 1
search, 2 history (`button.c`); the display maps position to code. `-N`
takes grifo's code, not the screen position. Codes are grifo's own numbering from
`button.c` -- "0=random, 1=search, 2=history". The power button is separate,
on a P03 port interrupt, and is not modelled.

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

Between the firmware and the differential tests, 57 of the 74 implemented
PE opcodes are known to execute (`wremu -P`). Focused core tests cover more;
see `difftest/README.md` for the distinction between valid PE operations,
non-PE words recognized by the all-core decoder, and the coprocessor gap.

### Drawing

Three things keep an idle window nearly free, and the first two were not
enough on their own.

**Repaints are paced by the wall clock, not the guest.** The repaint used
to be driven by guest cycles -- once every 200k -- which at emulation speed
is several hundred presents a second. Nothing on a 240x208 panel needs more
than sixty.

**The bezel is a texture.** Drawn live it is several hundred draw calls a
frame, because every lit pixel of the 3x5 font is its own rectangle and
each button outline is a stack of lines. It changes only when a button goes
down, so it is rendered once and blitted after that.

**The window is high-DPI and scaled nearest-neighbour.** Without
`SDL_WINDOW_ALLOW_HIGHDPI` the backing store is at window size and the
compositor upscales it to a Retina panel, which looks soft -- most
obviously on a still screen, where there is time to notice. With it, SDL
scales, and at an integer factor with `SDL_HINT_RENDER_SCALE_QUALITY` at
nearest it stays sharp. A one-bit panel wants hard pixel edges;
interpolating between them is only blur.

**Presents are synchronised to the display.** Without vsync the emulator
hands over a new frame whenever it has one and the display scans out part
of the old buffer and part of the new -- a thin seam along an edge. It is
an odd bug to chase because it only shows while frames are actually being
presented, so it appears when the guest draws and vanishes when the screen
settles, and it **cannot be screenshotted**: a screenshot copies the
composited surface, which is intact, while tearing happens afterwards
during scanout. Waiting for the refresh is the right pacing for a window
anyway, and since unchanged frames are skipped before that point, an idle
screen never waits.

**Unchanged frames are not presented at all.** This is the one that
mattered. Nothing draws to an idle panel, so those sixty frames a second
were sixty identical uploads. `lcd_fingerprint()` hashes the framebuffer
bytes -- 6656 of them, plus the sub-window when PIP is on -- and the
repaint is skipped when nothing has moved.

Measured on an idle window: **0.6%** of a core for the emulator and no
measurable addition to the compositor, against 20% and 40-odd before. A
run reports its own figures -- update calls against presents -- and a
settled screen presents a handful of times in thousands of calls.

Note that "idle" means the device is idle. Using the interface wakes it,
and a woken machine emulates at full speed, as it should.

### Idling

The core spends most of its life in HALT waiting for an interrupt, and for
a while the emulator ground through that a cycle at a time, pinning a host
core to do nothing.

It now skips it, differently in each mode. Headless, the guest clock jumps
straight to whatever is due next -- the suspend wake timer, or a scripted
tap or keypress -- since nothing can happen before then anyway. A five
billion instruction run finishes in about 1.5 seconds, because nearly all
of it was idle. `-n` counts guest cycles, so it still bounds the run the
same way; the report says how many were skipped rather than spun.

With a window the tick comes from the wall clock, so instead of jumping it
hands the time back to the operating system in 10 ms slices, repainting and
pumping events at about 100 Hz. That is well inside what a click needs, and
the guest clock is advanced by the elapsed amount so limits and scripted
input keep their meaning. An idle window costs a few percent of a core
rather than all of one.

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
