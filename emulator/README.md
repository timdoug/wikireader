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
| `-b ADDR` | breakpoint: registers plus recent PCs |
| `-W ADDR` | write watchpoint |
| `-V VAL` | watch stores of a byte value |
| `-D ADDR -L N -O FILE` | memory dump, optionally to a binary file |
| `-t N` | disassemble the first N instructions |
| `-m` | trace unclaimed MMIO registers |

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
| `src/timer.c` | 60 MHz tick timer |
| `src/periph.c` | ADC |
| `c33_forms.h` | **generated** decode tables |
| `c33_syscalls.h` | **generated** syscall names |
| `tools/` | table generators and ISA-fitting scripts |

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

Known divergences from the manual, none of which the firmware exercises on
the boot path: `slp` resumes immediately rather than halting until an
interrupt, `halt` stops the emulator outright, traps are not masked between
a `.d` branch and its delay slot, and `jpr`, `swap`, `swaph`, `adc`, `sbc`
and the coprocessor instructions are unimplemented (none appear in any of
the four firmware images).

The PSR's IL[3:0] field (bits 11-8) is not modelled: interrupts are gated
on IE alone, so there is no priority masking and IL is not updated on
acceptance. Nothing here raises more than one interrupt source at a time.

The peripherals are the least verified part. They were written from the
driver sources in samo-lib and from what the firmware demanded, not from
the S1C33E07 register descriptions, which run to several hundred pages.

## Caveats

This proves the firmware is self-consistent under this model of the ISA, not
that it would boot on hardware - the emulator was built by inferring
semantics from the same binaries it runs, so a shared misreading would not
show up. The independent checks are the decoder, validated instruction for
instruction against binutils over all four firmware images (65,605
instructions, exact match), and the known-answer arithmetic tests built with
the real cross compiler.

Timing is not wall-clock paced: one tick per instruction, so emulated time
runs at whatever speed the host manages.
