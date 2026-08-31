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

`DT_TC` selects which toolchain generates the target code. It defaults to
the original, which is the stronger oracle -- it has never seen the
emulator. Point it at the gcc 16 install to check the emulator against the
instruction mix the *new* compiler emits:

```
DT_TC=/path/to/gccinstall/bin ./run.sh 1 40
```

## Current state

All programs match value for value across `-O0`, `-O1`, `-O2`, `-O3` and
`-Os`, with **both** toolchains -- 200 programs each.

### What the gcc 16 run added

The emulator was differentially tested only against code gcc 3.3.2 emits,
so anything the new compiler generates that the old one never did was
running unchecked. Measured over the same 40 programs at `-O2`:

| | gcc 3.3.2 | gcc 16.2 |
|---|---:|---:|
| instructions emitted | 49,715 | 27,258 |
| delay-slot forms (`.d`) | 7,081 | 3,546 |
| **post-increment (`[%rb]+`)** | **0** | **370** |
| `ext` prefixes | 7,987 | 3,773 |

Two of the three things worth knowing here contradict the guess that
prompted the run:

* **Delay slots were already covered.** 3.3.2 fills them at a similar rate
  (14.2% of instructions against 16.2's 13.0%), so they were never the gap.
* **Post-increment addressing was not covered at all.** 3.3.2 emits none --
  `HAVE_POST_INCREMENT` was never defined in that backend -- so `ld.w
  [%rb]+,%rd` had never been checked against an independent authority. It is
  also precisely the addressing mode the article-load speedup rests on, so
  it was the worst thing to have untested. 370 instances now match.
* One opcode is newly reached: `rr`.

The instruction counts are a side observation, not a benchmark: same
programs, same `-O2`, 45% fewer instructions from the newer compiler.

Getting there took finding and fixing a **bug in gcc 3.3.2**. Seed 49
diverged because the compiler discards a narrowing signed cast when it
reassociates a multiply: `(u32)(signed char)(s * K) * C` was emitted as
`s * (K * C)`, with no sign-extension instruction anywhere. The emulator
executed exactly what it was given.

It is a one-identifier fix in `fold-const.c` -
`host-tools/toolchain-patches/0008-*` - reduced and root-caused in
[`compiler-bugs/`](compiler-bugs/README.md). The firmware never triggered it:
268 translation units compiled with an instrumented compiler reached zero
sites, and `grifo.elf` rebuilt with the fixed compiler is byte-identical.

Finding a real compiler bug is the harness working as intended: the two
sides genuinely disagree, and the disagreement had to be adjudicated against
the C standard rather than against either implementation.

Three earlier divergences were the *generator's* fault, not either
compiler's - `(unsigned short)a * (unsigned short)b` promotes both operands
to `int`, so products above `INT_MAX` are signed overflow. The host
optimiser exploited that undefined behaviour and the target did not. The
templates now force such multiplies into unsigned arithmetic. That is the
recurring hazard in this kind of harness: a false alarm from UB looks
exactly like a real bug until you reduce it.

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
| all-core binutils ISA table | 104 |
| valid on C33 PE | 77 |
| implemented here | 74 |
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

The all-core table also names 27 operations which are not C33 PE
instructions: nine STD instructions explicitly removed from PE and 18
ADV-only operations. They now take PE's undefined-instruction exception;
they are not emulator feature gaps. The only valid PE operations not
implemented are the three coprocessor opcodes (`ld.c` in two directions,
`do.c`, and `ld.cf`), because the S1C33E07 has no attached coprocessor model.

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
