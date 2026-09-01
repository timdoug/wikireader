# C33 DejaGnu board

This directory connects GCC's standard DejaGnu testsuite to the installed C33
cross-compiler and `emulator/wremu`. It supersedes the retired handwritten
test harness.

GCC's own drivers select sources and optimization variants, interpret
`dg-*` directives, and produce the authoritative `gcc.sum` and `gcc.log`.
No upstream test is rewritten.

## Components

- `c33-sim.exp`: target flags, freestanding runtime linkage, emulator
  execution, runtime-prerequisite classification, and exit-status mapping.
- `site.exp`: board and cross-compiler location.
- `run-dejagnu.sh`: runtime build and convenient `runtest` invocation.
- `../runtime/`: crt entry, `setjmp`, test services, and linker script used
  only by DejaGnu execution tests.

## Prerequisites

```sh
brew install deja-gnu
```

Build and install the modern compiler, binutils, libgcc, mini-libc, and
`emulator/wremu` before running the board.

## Usage

From this directory:

```sh
./run-dejagnu.sh execute
./run-dejagnu.sh compile
./run-dejagnu.sh gcc.dg
./run-dejagnu.sh all
```

`all` passes no selector and discovers the complete GCC C testsuite. Narrow
runs are preferred while diagnosing a source:

```sh
./run-dejagnu.sh execute execute.exp=pr61725.c
./run-dejagnu.sh compile compile.exp=20030305-1.c
./run-dejagnu.sh gcc.dg dg.exp=20010516-1.c
./run-dejagnu.sh all dg-torture.exp=inline-mem-cmp-1.c
./run-dejagnu.sh all lto.exp=pr122515_0.c
```

Selectors use the entry-point basename, including multi-file LTO tests.

Results and generated objects go under ignored `work/` by default.
`GCC`, `GCOV`, `EMU`, `TCROOT`, `SRC`, `WORK`, `LIMIT`,
`HOST_TIMEOUT`, `LIBC`, and `RUNTIME_DIR` override their corresponding
defaults.

GCC's intentionally enormous tests follow the upstream default and are not
enabled automatically:

```sh
GCC_TEST_RUN_EXPENSIVE=1 ./run-dejagnu.sh all
```

Any nonempty value enables them, including `0`.

## Verdict contract

Execution runs the program in `wremu` with a deterministic instruction
limit. Reaching the exit breakpoint with `%r4 == 0` passes.

- `%r4 == 0xdead` is the test runtime's `abort()` and fails.
- faults, nonzero exits, and exhausted instruction budgets fail;
- output without a register dump is unresolved because execution did not
  complete; and
- a missing external target capability is unsupported only when every missing
  symbol belongs to an explicit known-runtime category.

An unknown undefined symbol is a failure. This prevents a missing or misspelled
compiler-generated libgcc helper from being hidden as a libc limitation.

Runtime objects are built once per option set. Builtins remain enabled.
Legacy C tests retain the standard `-w -fpermissive` compatibility flags.
The board uses GCC's standard `timeout_value`, so per-test
`dg-timeout-factor` scales both the host watchdog and emulator budget.

The macOS compatibility layer supports GCC drivers that open read-only Tcl
pipelines. Runtime-gap inspection is restricted to source-language inputs;
large LTO objects and archives are never opened as Tcl text.

## Current focused results

| Driver | Result |
| --- | --- |
| `gcc.c-torture/execute` | 24,260 passes, 251 legitimate unsupported, zero failures or unresolved |
| `gcc.dg/torture` | no demonstrated GCC/backend failure |
| IPA | 807 passes, 4 XFAIL, 13 unsupported, no unexpected |
| LTO | 1,651 passes, 34 unsupported, zero failures or unresolved |
| `gcc.dg/dg.exp` | 39,358 passes, 4 documented target-dependent mismatches, 534 XFAIL, 1,037 unsupported, no unresolved |

The final complete post-fix run is pending. Historical raw full-run counts are
not current status; preserve the next `gcc.sum` and `gcc.log` and triage
only its fresh unexpected results.

The remaining target-dependent mismatches, optional compiler features, and
external runtime boundary are documented in
[`../DEJAGNU-TODO.md`](../DEJAGNU-TODO.md).

GCC 16.2 has no standalone libgcc testsuite in this configuration. GCC
execution tests cover the installed `libgcc.a` through generated integer,
soft-float, conversion, complex-arithmetic, and forwarding helper calls.
