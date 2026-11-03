# Upstream BusyBox and libc-test suites

These runners build and attempt two complete upstream test inventories
for the installed C33 Linux configuration: BusyBox's applet tests and
Hush's shell tests, against the installed `/bin/busybox`, and musl's
libc-test, against the installed C library. Every target trial boots
**FLASH -> Grifo -> launcher -> Linux**, with 32 MiB SDRAM and a disposable
virtual SD card. These are emulator results; the physical device and the
16 MiB configuration need separate qualification.

The C library's own regressions are `../musl/tests`, which `app-test.py`
runs.

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

libc-test is pinned in `build.py` and built by its own Makefile with its
own flags, every program dynamic and static (math dynamic only), with a
256 KB stack. Its runner forks, so each program runs under `super.bin`
from the top of its tree, where the dlopen tests find their libraries; a
test passes when it exits 0. A trial stages only the selected programs and
the test libraries.

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
python3 linux/upstream-tests/run.py libc --batch-size 16 --output DIR
```

## Results

BusyBox's last full sweep ran on the uClibc-ng system, before the move to
musl; it has not been repeated on musl:

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

libc-test (revision 63e36e93, musl 7813e911) has 486 programs: 445 pass,
2 do not build, 31 fail and 8 are killed. None crashes or times out:

| Tests | Cause |
|---|---|
| `api/main` (build) | POSIX names musl lacks: `_PC_TIMESTAMP_RESOLUTION`, `_SC_XOPEN_UUCP` |
| `musl/pleval` (build) | Calls libc's hidden `__pleval`; only its static build links, and passes |
| 6 `ipc_*`, 2 `wordexp`, 2 `daemon-failure`, 2 `fflush-exit`, 2 `pthread_exit-dtor`, 2 `raise-race` | Need `fork`, which fails with `ENOSYS` without an MMU |
| 4 `malloc-oom`, 2 `pthread_create-oom`, 2 `setenv-oom`, 2 `malloc-brk-fail` | Fill the address space with `PROT_NONE` mappings first: without an MMU each one is RAM, and the OOM killer ends the test |
| 2 `sscanf_long` | An 8 MB contiguous allocation |
| 2 `socket` | UDP over IPv4; the kernel has no IPv4 |
| 4 `regex-backref-oob`, `regexec-backref` | Tests newer than any musl fix |
| 2 `strptime` | musl has no `%s` or `%z` |
| `acoshl`, `asinhl`, `erfcl`, `lgammal`, `tgammal` | `long double` is `double`, and the results are the double functions'; the double drivers accept these errors (their `X` lines), the `long double` drivers do not |

## Fixes these suites found

libc-test found two:

- musl's `fork` on an architecture without `SYS_fork` is
  `clone(SIGCHLD, 0)`, which a no-MMU kernel accepts: the child ran on its
  parent's stack and data, and the tests crashed, hung or took the kernel
  down. `../musl/patches/fixes/0017` makes it fail with `ENOSYS`; nothing in
  the root image calls it.
- An address in a dynamic executable's TLS image was a `.rofixup` entry,
  applied by the start code after ld.so had copied the image for the
  first thread, which kept the link-time value (`tls_local_exec`). The
  linker now gives it an `R_C33_RELATIVE`;
  `ld-c33/fdpic-tls-image-*.d` checks both kinds of executable. No program
  in the root image has a TLS segment.

BusyBox's found these:

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
