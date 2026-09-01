# Differential C33 execution testing

This harness tests emulator and compiler behavior against native execution.
The same defined C program is compiled for C33 and for the host; both outputs
must match value for value.

The target compiler and emulator are independent implementations. Agreement
therefore checks more than running firmware inferred from the same binaries
used to develop the emulator.

## Usage

```sh
./run.sh 1 40
DT_OPTS=-O0 ./run.sh 1 20
```

`DT_TC` selects the target toolchain. It defaults to the original GCC 3.3.2
toolchain, which is the stronger emulator oracle because it predates this
emulator:

```sh
DT_TC=/path/to/gcc16/install/bin ./run.sh 1 40
```

Run the same seeds under `-O0`, `-O1`, `-O2`, `-O3`, and `-Os`.
Current qualification uses 40 seeds at each level: 200 target executions per
compiler. GCC 3.3.2 and GCC 16.2 both match the native reference in every
case.

The original compiler is patched for one reduced GCC 3.3.2 optimizer defect:
it discarded a narrowing signed cast while reassociating a multiply. The
reproducer and language-level analysis are in
[`compiler-bugs/`](compiler-bugs/README.md). Instrumented firmware builds
reach no instance of that transformation and remain byte-identical.

## Generated coverage

`gen.py` emits unsigned expression trees plus targeted shapes:

| Shape | Coverage |
| --- | --- |
| functions with 1-8 arguments | register and stack argument ABI |
| recursive accumulator | nested frames and `pushn` / `popn` |
| 200-element local array | large scaled `%sp` displacements |
| signed/unsigned byte and halfword arrays | load extension |
| 16-way switch | jump tables and indirect branches |
| `unsigned long long` arithmetic | multiword carry and shifts |
| signed/unsigned division and modulo | generic libgcc division |
| rotate and 16x16 multiply idioms | `rl`, `rr`, `mlt.h`, `mltu.h` |
| bitfields | memory `bset`, `bclr`, and bit tests |

Immediate values straddle every C33 field-extension boundary. Generated
programs avoid undefined behavior: arithmetic is unsigned, shift counts are
masked, divisors are guarded, and `INT_MIN / -1` is excluded.

GCC 16.2 adds independent coverage that GCC 3.3.2 does not produce, notably
post-increment memory operations and `rr`. Delay slots are exercised by both
compilers.

## Measured ISA coverage

`wremu -P` reports retired opcodes:

| Set | Distinct opcodes |
| --- | ---: |
| all-core binutils ISA table | 104 |
| valid on C33 PE | 77 |
| implemented by wremu | 74 |
| executed by firmware boot/search | 53 |
| executed by differential programs | 48 |
| executed by either | 57 |

Firmware supplies the privileged/system forms a C test cannot request:
`int`, `reti`, `psrset`, `psrclr`, `slp`, `bset`, `bclr`, `sla`,
and `nop`. Differential programs uniquely reach `rl`, `mlt.h`, `mltu.h`,
and `halt`.

Implemented forms not retired by either set are `bnot`, `brk`, `btst`,
`jpr`, `jpr.d`, `mltu.w`, `pop`, `pops`, `push`, and `pushs`.
Focused emulator core tests cover many of these directly; targeted independent
runtime evidence for stack-special and indirect-jump forms remains useful.

The 27 all-core operations not valid on PE are nine Standard instructions
removed from PE and 18 Advanced-only instructions. They correctly take PE's
undefined-instruction exception. The only valid PE operations intentionally
not implemented are the coprocessor forms, because the S1C33E07 has no
attached coprocessor.

## Layout

| Path | Purpose |
| --- | --- |
| `gen.py` | deterministic defined-program generator |
| `run.sh` | compile target/native, execute, and compare |
| `coverage.sh` | aggregate retired-opcode histograms |
| `runtime/` | target startup, I/O, linker script, and native support |
| `compiler-bugs/` | reduced compiler divergences and adjudication |
