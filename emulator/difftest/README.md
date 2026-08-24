# Differential testing against the real cross compiler

The emulator was built by inferring instruction semantics from the same
binaries it runs, so a misreading shared between the emulator and the
inference could not show up. This harness attacks that directly.

The same C source is compiled twice:

* by **c33-epson-elf-gcc 3.3.2** with binutils 2.10.1 - the real WikiReader
  toolchain - and run in the emulator;
* by the **host compiler**, and run natively.

Both print the same stream of 32-bit values. Any divergence means the
emulator executes at least one instruction differently from what the
compiler meant by it. The compiler is the independent authority: it has
never seen the emulator, and the emulator has never seen its output.

```
./run.sh 1 40                 # seeds 1..40
DT_OPTS=-O0 ./run.sh 1 20     # a different instruction mix
```

## Current state

100 programs across `-O0`, `-O1`, `-O2`, `-O3` and `-Os` all match, value
for value.

## What the generated programs cover

`gen.py` emits random expression trees over `u32`, plus targeted shapes
chosen for the parts of the ISA that were hardest to get right:

| shape | what it exercises |
| --- | --- |
| functions of 1..8 arguments | the argument ABI: `%r6`..`%r9`, then the stack |
| a recursive accumulator | nested frames, `pushn`/`popn` |
| a 200-element local array | `[%sp+imm]` past `imm6`, where the byte-vs-word scaling bug lived |
| `char`/`short` arrays, signed and unsigned | `ld.b`/`ld.ub`/`ld.h`/`ld.uh` extension |
| a 16-way `switch` | jump tables and indirect branches |
| `unsigned long long` arithmetic | multi-word carry sequences |
| division and modulo, signed and unsigned | libgcc's software divide |
| rotate idioms, 16x16 multiplies | `rl`, `mlt.h`, `mltu.h` |
| single-bit set/clear on struct members | read-modify-write on memory |

Immediates are drawn from a table that straddles every field width in the
ISA - 0x1fff/0x2000, 0x7fff/0x8000, 0xffff/0x10000 and so on - so the `ext`
prefix machinery is exercised at each boundary rather than at random.

Everything is written to avoid undefined behaviour, or the two compilers
could legitimately disagree and produce false alarms: arithmetic is done in
unsigned, shift counts are masked to 0..31, divisors are guarded, and
`INT_MIN / -1` is folded away. The one implementation-defined step is the
`u32` to `i32` conversion, which both compilers implement as two's
complement.

## Coverage, measured rather than assumed

`wremu -P` prints a histogram of the opcodes actually retired. This matters
because static disassembly cannot answer the question: `objdump` decodes
rodata and jump tables as instructions too, which is how a binary
containing no `div.w` at all appears to contain thirty of them.

| | distinct opcodes |
| --- | --- |
| c33 ISA table | 104 |
| implemented here | 68 |
| executed by a firmware boot and search | 53 |
| executed by the differential tests | 48 |
| **executed by at least one of the two** | **57** |

The two are complementary, which is the point of having both:

* only the difftest reaches `rl`, `mlt.h`, `mltu.h`, `halt`;
* only the firmware reaches `int`, `reti`, `psrset`, `psrclr`, `slp`,
  `bset`, `bclr`, `sla`, `nop` - the system instructions no C program can
  ask for.

Implemented but still never executed by either: `bnot` `brk` `btst` `jpr`
`jpr.d` `mltu.w` `pop` `pops` `push` `pushs` `rr`. `pushs`/`pops` is the one
worth flagging - its operand range was corrected against the manual, and
that correction still has no runtime evidence behind it.

The 36 unimplemented opcodes are the MAC, divide-step, saturate, scan and
coprocessor families. None appears in any firmware image and gcc 3.3.2 does
not emit them, so they are a real but inert gap: they would matter for other
C33 software, not for this device.

## Layout

| path | |
| --- | --- |
| `gen.py` | random program generator, seeded |
| `run.sh` | build both ways, run both, diff |
| `coverage.sh` | executed-opcode histogram over a work directory |
| `runtime/dt.h` | `emit()` - serial on the target, `printf` on the host |
| `runtime/start.s` | bare-metal entry: stack, `__dp`, zero `.bss`, `main`, `halt` |
| `runtime/target.lds` | link at 0x10000000 in SDRAM |

`__dp` is pinned to the start of the image rather than to `.data`: the
linker rejects any symbol below it with "Default Data area pointer value is
larger than symbol address value", and `.rodata` sits before `.data`.
