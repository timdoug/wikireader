# Upstream BusyBox suite

These runners build and attempt BusyBox's complete upstream test inventory
for the installed C33 Linux configuration: its applet tests and Hush's
shell tests, against the installed `/bin/busybox`. Every target trial boots
**FLASH -> Grifo -> launcher -> Linux**, with 32 MiB SDRAM and a disposable
virtual SD card. These are emulator results; the physical device and the
16 MiB configuration need separate qualification.

The C library's own upstream suite is not here: uClibc-ng's was, while the
system used it, and musl's (libc-test) has no runner yet. The C library's
regressions are `../musl/tests`, which `app-test.py` runs.

## Sources and execution

`build.py` uses the product build under `/home/timdoug.guest/wr-linux`:
its compiler and the BusyBox tree Buildroot built, from which the tests
come. BusyBox runs unchanged `testsuite/runtest -v APPLET` and
`shell/hush_test/run-all MODULE`. The unchanged upstream `scripts/echo.c`
is cross-compiled ahead of time, since the target has neither a native
compiler nor an echo with the upstream harness's required `-ne` support.
Source lint (`all_sourcecode.tests`, `check-source.py`) runs on the Linux
build host against the complete BusyBox source tree; it is counted
separately from C33 execution.

The inventory has 96 BusyBox applet/source groups and 19 Hush modules. Ash
is disabled in our BusyBox configuration; `busybox-disabled-suites.json`
records its 367 test files and hashes. None of these disabled tests is
counted as a pass.

Interrupted shell modules do not constitute complete coverage. The runner
can select each of Hush's 404 executable tests with expected-output files
through the same upstream `run-all`. Selection changes only executable
permissions on other staged `.tests` files in that module. Test bodies,
helpers and expected output stay unchanged. Failed Hush comparison files
are printed into the serial log.

```sh
python3 linux/upstream-tests/build.py            # in the VM
python3 linux/upstream-tests/run.py busybox --output DIR
python3 linux/upstream-tests/run.py busybox --hush-case hush-misc/func7.tests --output DIR
python3 linux/upstream-tests/summarize.py --busybox DIR --output summary.json
```

## Results

The last full sweep ran on the uClibc-ng system, before the move to musl;
it has not been repeated on musl:

| Inventory and counting unit | Pass | Fail | Skip/untested | Other |
|---|---:|---:|---:|---|
| BusyBox applet result markers (792) | 671 | 8 | 113 | None |
| Individual Hush tests (404) | 388 | 15 | 0 | 1 timeout |

| Tests | Cause |
|---|---|
| 3 BusyBox `bunzip2`/`bzip2`, 3 `cpio` | bzip2's default level needs a 3.6 MB contiguous buffer; the suite fragments memory first |
| 8 Hush leak tests + `many_ifs` timeout | Disabled `HUSH_MEMLEAK` builtin |
| 2 Hush | Disabled Unicode |
| 1 Hush `signal_read1` | `read` from the runner's `/dev/null` races its HUP; upstream expects a blocking terminal |
| 4 Hush, 3 sed | Upstream bugs: the Hush tests fail identically in a native no-MMU build; sed's are marked as known |

## Fixes these suites found

The kernel's no-MMU FDPIC loader sized a program's stack from
`PT_GNU_STACK` without counting the argument and environment strings,
pointer arrays, load maps and auxiliary vector it copies there; a large
argument image overwrote neighbouring memory, since no-MMU
`copy_to_user` cannot fault. `0031-fdpic-fit-initial-stack-image.patch`
treats `PT_GNU_STACK` as the whole stack, arguments included, and grows the
allocation only when the image exceeds a quarter of it. No-MMU Hush
re-executes itself for command substitutions with its variables as
arguments, so this was the BusyBox "stack overflow" and the Hush kernel
panic. `musl/tests/exec-stack-test.c` checks it.

Linux 7.2's `/dev/mem` mapping was rejected on no-MMU, and once accepted, a
mapping at another region's start reached `BUG()` in the region tree:
`0032-nommu-identity-pfn-mappings.patch` and
`0033-nommu-region-tree-aliased-starts.patch`. `musl/tests/devmem-test.c`
checks both.

FDPIC calls require the saved GOT operand in a preserved register: GCC's
DSE had discarded an explicit frame read inside the call pattern and then
deleted the store that initialised it, which corrupted `setjmp` callers.
`gcc.target/c33/fdpic-got-setjmp.c` checks it.
