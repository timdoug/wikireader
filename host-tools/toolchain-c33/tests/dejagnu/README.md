# C33 DejaGnu board

This directory connects GCC's standard DejaGnu testsuite to the installed C33
cross-compiler and `emulator/wremu`. DejaGnu is the sole test harness: GCC's
own drivers select tests and optimization options, interpret `dg-*`
directives, and write the result summaries.

The C33-specific pieces are limited to normal target integration:

- `c33-sim.exp` supplies target flags, links the freestanding test runtime,
  invokes the emulator, and translates its exit register into a DejaGnu
  verdict.
- `site.exp` locates the board and cross-compiler.
- `run-dejagnu.sh` builds the target runtime and provides convenient paths to
  `runtest`.

## Usage

Install DejaGnu once on macOS:

```sh
brew install deja-gnu
```

Then run from this directory:

```sh
./run-dejagnu.sh all
./run-dejagnu.sh execute
./run-dejagnu.sh compile
./run-dejagnu.sh gcc.dg dg.exp=20010516-1.c
```

`all` passes no test selector to `runtest`, so DejaGnu discovers and runs the
complete GCC C testsuite for the C33 board. The narrower modes are useful for
reproducing and iterating on one family.

A focused torture test uses the entry-point basename, not its full path:

```sh
./run-dejagnu.sh execute execute.exp=pr61725.c
./run-dejagnu.sh compile compile.exp=20030305-1.c
```

The same basename rule applies to multi-file LTO tests:

```sh
./run-dejagnu.sh all lto.exp=pr122515_0.c
```

`GCC`, `EMU`, `TCROOT`, `SRC`, `WORK`, `LIMIT`, `HOST_TIMEOUT`, `LIBC`, and
`RUNTIME_DIR` can override the defaults. Generated runtime objects and results
go under `work/`, which is ignored.

The authoritative output is DejaGnu's standard `gcc.sum` and `gcc.log` in the
selected `WORK` directory. The wrapper does not replace or reinterpret those
results. GCC's intentionally enormous cases follow the upstream default and
are skipped. Set the nonempty environment variable `GCC_TEST_RUN_EXPENSIVE=1`
to opt into them; even the string `0` is nonempty and therefore enables them.

The macOS compatibility layer covers DejaGnu command execution and GCC's
direct read-only Tcl pipelines without modifying upstream tests. Runtime-gap
classification reads only source-language inputs; link-time `.o` and `.a`
files must never be opened as Tcl text. This matters for upstream LTO
`pr122515`, which deliberately creates a 2.88 GB archive.

The complete post-fix `gcc.dg/lto/lto.exp` run records 1,651 expected passes,
34 unsupported tests, and no failures or unresolved cases.

GCC 16.2 has no standalone `libgcc/testsuite`; its `libgcc` `check` target is
empty. The execution suites cover the installed `libgcc.a` through generated
integer, soft-float, conversion, and complex-arithmetic helper calls.

## Full-run baseline

The first unfiltered run completed on 2026-08-28 under
`work/full-20260828-2130/`:

```text
# of expected passes        145212
# of unexpected failures      1836
# of unexpected successes        4
# of expected failures          921
# of unresolved testcases      1198
# of unsupported tests         6107
```

Those raw counts are not 1,836 independent C33 code-generation bugs. The
largest families are sanitizer tests without a target sanitizer runtime,
analyzer tests whose hosted-library assumptions do not match mini-libc, gcov
tests without the matching host-side tool, and repeated assertions from one
source/option matrix. Preserve `gcc.sum` and `gcc.log` and group failures by
source before drawing conclusions.

The run found and fixed three board integration omissions: target headers were
not globally visible, the matching binutils directory was not on `PATH` for
GCC's object-format probes, and long-double `fmaxl`/`ilogbl` libm gaps were not
classified. The last correction was verified by a focused rerun after the
baseline, so the baseline retains nine now-obsolete `cdivchkld.c` failures.

Missing capabilities remain implementation work even when DejaGnu reports
them unsupported. The concrete backlog and acceptance tests are in
[`../DEJAGNU-TODO.md`](../DEJAGNU-TODO.md).

## Target-runtime boundary

The board reports tests requiring deliberately absent hosted facilities as
unsupported. These include filesystem operations, libm, and floating-point
`printf`. Unknown linker symbols remain failures so a bad compiler-generated
libcall cannot be hidden as a runtime limitation.

For execution tests, `c33-sim_load` runs:

```text
wremu -n 3200000000 -b 0x10000002 program
```

Reaching `pc=10000002` with `r4=00000000` passes. `r4=0000dead` is the
runtime's `abort()` and fails; faults, nonzero exits, and instruction-limit
stops also fail. Output with no register dump is unresolved because the
emulator did not complete.
