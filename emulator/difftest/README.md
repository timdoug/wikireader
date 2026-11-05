# Differential C33 execution testing

This harness tests emulator and compiler behavior against native execution.
The same defined C program is compiled for C33 and for the host; both outputs
must match value for value.

Every target program boots through the real FLASH image, Grifo kernel and
`init.app`, which selects its generated application in `init.ini`. The old
bare-program startup and linker script have been removed. A successful run
requires the complete boot chain, matching nonempty output between application
markers, and application poweroff. Each run preserves its inputs, hashes,
compiler/linker identities, raw boot log and comparison result in `DT_WORK`.
The validated cleanup covers 20 seeds at `-O2` per compiler, two seeds at
`-O0` and `-Os` per compiler, and four final harness checks: 52 generated
program runs. GCC 3.3.2 uses default timing; GCC 16.2 uses the optional local
frontend. The shared application wrapper also passes all four cross-compiler
ABI combinations, the reduced compiler regression and Doom's 227,672 exact
arithmetic results. These are functional checks, not timing calibration.

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
Historical bare-program runs used 40 seeds at each level for each compiler;
those results do not qualify the Grifo boot path. Current path validation
is recorded separately in `build/wr128/difftest-grifo-review/`.

`DT_WORK`, `DT_OPTS` and `DT_NINSN` retain their existing meanings.
`DT_WREMU` selects a frozen emulator executable. `DT_LD` selects the production
linker: the old GCC 3.3.2 linker cannot parse the current application's overlay
sections. Target code and arithmetic helpers still come from `DT_TC`.
Existing C tests can use `--source`, `--extra-object` and `--extra-library`;
`--target-only` requires the caller to compare the resulting values, as the
ABI harness does. `--file-output` writes large result streams through the
real Grifo filesystem; Doom uses it to avoid spending its instruction budget
polling the configured serial transmitter. Output remains compared exactly.

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

The historical bare-program measurements below are retained for provenance.
`coverage.sh DIRECTORY ...` now reads only completed, hash-verified Grifo-run
logs; it does not rerun old ELFs. Its opcode counts include the boot chain and
application, so they cannot establish application-only coverage.

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
| `runtime/` | Grifo application entry, target I/O and native support |
| `compiler-bugs/` | reduced compiler divergences and adjudication |
