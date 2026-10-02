# Upstream BusyBox and uClibc suites

These runners build and attempt the complete upstream test inventories for
the installed C33 Linux configuration. They use the existing compiler,
uClibc-ng 1.0.59 and BusyBox 1.38.0. Every target trial boots
**FLASH -> Grifo -> launcher -> Linux**, with 32 MiB SDRAM and a disposable
virtual SD card. These are emulator results; the physical device and the
16 MiB configuration need separate qualification.

## Sources and execution

`build.py` uses the product build under `/home/timdoug.guest/wr-linux`:
its compiler, its uClibc configuration and the BusyBox tree Buildroot built.
Its separate test checkout is
`/home/timdoug.guest/wr-linux/upstream-suites/uclibc-ng-test`, pinned to
`132c6134d69146bcafbdda68ae3a9cbd9f8fb921`, the revision selected by our
Buildroot 2026.08 package. `arch/` adds the C33 in upstream's
per-architecture form. `tls-macros-c33.h` and its include line in
`tls-macros.h` give the TLS tests their four access models. Every C33 TLS
relocation is a data word, so the macros emit those words and C reads them.
`libm-test-ulps-c33` lists the math drivers' tolerances. It comes from
upstream's procedure: each driver's `-u` output on the board, merged by
`gen-libm-test.pl -n`. Except for the long-double drivers' `ceil` and
`floor` cases, ARM's file lists the same cases. Those come from inputs a
53-bit long double rounds, not from the functions. `build.py` verifies
that the include line is the only change to tracked upstream test sources.
BusyBox tests come from the exact source tree which built the installed
BusyBox.

The libc build supplies the actual enabled libc configuration symbols to
upstream Make and attempts every selected family, including math. It uses
`-O2 -std=gnu99 -fpermissive` for the older tests' C dialect and reserves a
256 KiB ELF stack per test. Compile failures stay in the inventory. Upstream
Make generates the commands, wrappers, arguments and expected exit statuses.
Each command runs through the unchanged `uclibcng-testrunner.sh`, including
its output comparisons and exit-23 skip handling.

BusyBox runs unchanged `testsuite/runtest -v APPLET` and
`shell/hush_test/run-all MODULE`. The installed `/bin/busybox` supplies the
applets and Hush. The unchanged upstream `scripts/echo.c` is cross-compiled
ahead of time, since the target has neither a native compiler nor an echo
with the upstream harness's required `-ne` support. Source lint
(`all_sourcecode.tests`) runs on the Linux build host against the complete
BusyBox source tree; it is counted separately from C33 execution.

The main inventory contains 524 libc commands (521 built, 3 build errors),
96 BusyBox applet/source groups and 19 Hush modules. Upstream Make disables
39 additional libc tests. Ash is disabled in our BusyBox configuration;
`busybox-disabled-suites.json` records its 367 test files and hashes. None
of these disabled tests is counted as a pass.

Interrupted shell modules do not constitute complete coverage. The runner
can select each of Hush's 404 executable tests with expected-output files
through the same upstream `run-all`. Selection changes only executable
permissions on other staged `.tests` files in that module. Test bodies,
helpers and expected output stay unchanged. Failed Hush comparison files
are printed into the serial log.

## Results

A full sweep of the product build (all 524 libc commands and every BusyBox
group, then each Hush test individually wherever its module timed out, and
the fixtures' long-deadline cases) gives:

| Inventory and counting unit | Pass | Fail | Skip/untested | Other |
|---|---:|---:|---:|---|
| uClibc selected commands (524) | 433 | 37 | 51 | 3 build errors |
| BusyBox applet result markers (792) | 671 | 8 | 113 | None |
| Individual Hush tests (404) | 388 | 15 | 0 | 1 timeout |

That is 92% of runnable libc commands, 98.8% of BusyBox applet assertions
and 96% of Hush tests. Applet groups: 41 pass outright, 16 contain passes
and skips, 35 are skipped and 3 fail. The skipped markers comprise 89
`SKIPPED` and 24 `UNTESTED` upstream markers; a skipped group can stand for
several unavailable tests. Host source lint separately has 2 passes and 3
failures, flagging current upstream source rather than executed C33 code.

None of the remaining failures is a known C33 defect:

| Commands or tests | Cause |
|---|---|
| 15 libc `locale/*` | No locale data in this configuration; two are utilities run without arguments |
| 15 libc NPTL | Several 1 MiB thread stacks in contiguous memory, or memory exhaustion |
| 3 BusyBox `bunzip2`/`bzip2`, 3 `cpio` | bzip2's default level needs a 3.6 MB contiguous buffer; the suite fragments memory first |
| 2 libc: `tst-cleanup2/x2` | Expect a null-pointer store to fault |
| 1 libc: `mmap2` | Maps the page that ends at 4 GiB, which a no-MMU VMA cannot represent |
| 2 libc: `tst-cancel7/x7` | Hush runs `sh -c`'s command as a child, which outlives cancellation |
| 2 libc `inet` | No IPv4 configured |
| 8 Hush leak tests + `many_ifs` timeout | Disabled `HUSH_MEMLEAK` builtin |
| 2 Hush | Disabled Unicode |
| 1 Hush `signal_read1` | `read` from the runner's `/dev/null` races its HUP; upstream expects a blocking terminal |
| 4 Hush, 3 sed | Upstream bugs: the Hush tests fail identically in a native no-MMU build; sed's are marked as known |

`tst-clock2` is among the memory failures here; it also expects blocked
threads' CPU clocks to advance. `tst-cancel2` and `tst-cancelx2` put a
100,000-byte array on a 64 KiB thread stack, so the overflow lands wherever
layout puts it and either can pass or crash. The 64 KiB stack is a
deliberate RAM choice. `tst-cancel14`, `tst-cancel15` and `ex3` failed once
in an earlier batch and pass on fresh boots and replays; a race has not
been ruled out.

The sweep used kernel
`3a25725d8bd7efca12c9ef4d364d68e99ded821db4143fc38304ce4ce29a5b67`, root
filesystem `205c2236b1b27e3fe28ef41131308c36dfdc4b3b60643d902ee78db199797073`
(the product `linux.img` grown to 32 MB) and emulator
`c7f9ff2d5117c20ca37196df82ab6f0bbea103e591084b0f9bb0f26c6023d40d`.
Reports: `upstream-uclibc-sweep3`, `upstream-uclibc-sweep3-slow`,
`upstream-busybox-sweep3`, `upstream-busybox-sweep3-long` and
`upstream-busybox-sweep3-hush`, audited in
`upstream-suites/summary-product.json`. The first sweep, on the unfixed
baseline, remains in `upstream-uclibc-full` and `upstream-busybox-full`.

## Validated loader fix

The function-descriptor relocation now uses the loader's existing unaligned
store helper. `.eh_frame` personality pointers can lie at addresses which
are not word-aligned; the previous direct word store faulted during startup.
On the same baseline compiler, kernel and libc, changing only the loader
makes 12 of 14 selected previously failing cancellation/cleanup commands
pass. The other two, `tst-cancelx7` and `tst-cleanupx2`, and also
`tst-cancelx10` and `tst-cancelx2`, fail for reasons in the results table.

`linux/uclibc/relocation-test.c` exercises descriptor pointers at all four
byte alignments. It fails with the old loader and passes with the fix.
The full `app-test.py` integration and suspend/TLS checks also pass with the
fixed loader. Reports: `upstream-loader-fixed-cancellation`,
`upstream-loader-fixed-probe`, `upstream-native-fixed`, and
`upstream-suites/loader-fixed-app.log`.

## Validated compiler fix

FDPIC calls now require the saved GOT operand in a preserved register. If
it spills, LRA emits a normal load before the call; the restore stays inside
the call template so implicit GOT accesses cannot be scheduled before it.
Previously the call could read its saved GOT directly from the frame, but
GCC DSE discarded that explicit read when recording the call's implicit
memory effects, then deleted the initialization store. This explains the
`bug269-setjmp` corruption, rather than a failure of libc's setjmp save area.
The unused frame-only constraint and predicate are removed.

The unchanged upstream `bug269-setjmp` and all four other selected setjmp
commands pass after rebuilding with the fixed compiler, on the baseline
kernel/libc with the loader fix. A standalone GCC regression derived from
the same test also passes through Grifo. Rebuilding the whole inventory
still produces 510 executables and the same 14 build errors. Reports: `upstream-fixed-tests`,
`upstream-compiler-fixed-setjmp-v2`, and `upstream-native-fixed`.

The builder accepts `--compiler` and `--work` for isolated comparison builds.
It cleans before compiling and no longer forces non-file setup targets
with `make -B`, which could fail on an existing `testlib` directory.

## Validated library search fix

Enable the upstream loader's RPATH/RUNPATH feature explicitly: disabling
`ld.so.cache` also disabled its default, so the executable's `$ORIGIN/testlib`
RPATH was never searched. The unchanged `dlopen/tst-origin` and four adjacent
dlopen tests pass with this configuration. In addition, RUNPATH's search
must supply the module filename to expand `$ORIGIN`, just as RPATH's does;
`0028-ldso-expand-runpath-origin.patch` fixes that separate omission.

The new native tests run the same executable with DT_RPATH and DT_RUNPATH
from a different directory, with LD_LIBRARY_PATH unset. RUNPATH fails with
feature enablement alone; both pass with the patch. Both are part of the
Grifo application checks. The final stripped loader is 36,580 bytes versus
36,108 in the baseline; libc remains byte-identical. Reports:
`upstream-runpath-fixed-probe`, `upstream-origin-different-cwd`,
`upstream-origin-fixed`, and `upstream-suites/fixed-runtime-provenance.json`.

## Validated exec stack fix

The no-MMU FDPIC loader copies argument and environment strings, their
pointer arrays, the load maps and the auxiliary vector into the stack
allocation. It sized that allocation from `PT_GNU_STACK` without counting
them. A large argument image consumed the program's stack, and past the
bottom it overwrote neighbouring memory, because no-MMU `copy_to_user`
cannot fault. `0031-fdpic-fit-initial-stack-image.patch` treats
`PT_GNU_STACK` as the whole stack, arguments included, as `RLIMIT_STACK` is
with an MMU. It grows the allocation only when the image exceeds a quarter
of it, leaving the program three quarters, and checks the untrusted size
for overflow. Ordinary execs cost nothing: the toolchain's default 32 KiB
stack is still one 32 KiB block. No-MMU mmap takes the next power-of-two
contiguous block before trimming the tail. Always adding the image would
have doubled that block for every power-of-two stack, and the extra
fragmentation made 1 MiB thread-stack tests such as `tst-tls2` fail.

No-MMU Hush re-executes itself for command substitutions and passes its
shell variables as arguments, so this was the cause of the BusyBox
"stack overflow" and the Hush kernel panic. Without any BusyBox stack change,
`md5sum` (3 of 3) and `sha256sum` (2 of 2) pass. So do the previously
crashing `tick_huge` and the `heredoc_huge` Hush tests. `heredoc_huge` also
needed the runner to ignore the console-blanking line printed between a test
name and its result. `linux/uclibc/exec-stack-test.c` execs a program with a
32 KiB stack. With ordinary arguments, its stack mapping must be exactly
32 KiB. It then passes a 120,000-byte argument, 8,000 arguments, or a
120,000-byte environment variable. Each child checks its arguments and uses
20 KB of its stack. The test is part of the Grifo application checks, which
pass. The always-add version fails the 32 KiB mapping check with 36 KiB.
Reports: `upstream-exec-stack-quarter`, `upstream-exec-stack-reserve`,
`upstream-quarter-checksums`, `upstream-quarter-hush`,
`upstream-quarter-fresh-boot`, and `upstream-suites/stack-quarter-app.log`.

## Validated /dev/mem mapping fix

Linux 7.2's `/dev/mem` describes its mapping as a PFN remap action, which
the no-MMU mm layer warned about and rejected. Every `/dev/mem` mmap
failed with `EINVAL` and three kernel WARNs. No-MMU places such a mapping
at the physical address itself. `0032-nommu-identity-pfn-mappings.patch`
checks that identity, as no-MMU `remap_pfn_range()` does, and applies the
same VMA flags. With the remap working, a mapping at another region's start
address reached `BUG()` in the region tree and panicked the kernel.
`0033-nommu-region-tree-aliased-starts.patch` orders such regions by
address; `do_mmap()` already allows direct device mappings to overlap.

`linux/uclibc/devmem-test.c` maps one of its own pages through `/dev/mem`,
writes through the alias, reads it back with `pread`, and unmaps both
mappings. It fails on the previous kernel and passes with both patches.
It is part of the Grifo application checks, which pass, and the other six
mmap tests still pass. Upstream `mmap2` maps the page at 0xfffff000. On
no-MMU that mapping would end at 2^32 and wrap the VMA end to zero, so it
now fails with `ENOMEM`, as any range beyond `TASK_SIZE` does. The native
test checks that error. Reports: `upstream-devmem-fixed`,
`upstream-devmem-baseline`, and `upstream-suites/devmem-fixed-app.log`.

## Validated no-MMU NPTL fixes

NPTL's no-MMU `pthread_atfork()` returned `EPERM`, although POSIX lets it
fail only for lack of memory. There is no `fork()` to run handlers, so
`0029-nptl-nommu-atfork-registration.patch` accepts and ignores them.
`mq_notify()` treated the failure as fatal. It closed its netlink helper and
reported every `SIGEV_THREAD` request as `ENOSYS`.

NPTL also carved an unprotected guard area out of each thread's stack. Each
thread lost a page, and a guard as large as the stack failed
`pthread_create()` with `EINVAL`. `0030-nptl-nommu-no-guard-memory.patch`
allocates no guard memory without an MMU. `pthread_getattr_np()` still
reports the requested size. `mq_notify()`'s helper had silently dropped
notifications whose attributes carried such a guard.

`tst-attr3` and `tst-mqueue6` pass. The NPTL check in the Grifo application
tests registers an atfork handler, creates a thread whose guard equals its
stack size, and receives a `SIGEV_THREAD` notification with those
attributes. It fails with the previous libc and passes with both patches,
as do the full application checks.

With all four fixes, and with `rcS` disabling watermark boosting, 197 of 220
previously executed NPTL, pthread and TLS commands pass. That includes
`tst-oncex3`, `tst-oncex4` and 12 cancellation tests repaired by the loader
fix. The 23 others:

| Commands | Cause |
|---|---|
| 9, e.g. `tst-barrier4`, `tst-tls2` | Several 1 MiB thread stacks in contiguous memory |
| `tst-basic7` | Exhausts memory |
| `tst-cancel2`, `tst-cancelx2` | A 100,000-byte array on a 64 KiB thread stack |
| `tst-cancel7`, `tst-cancelx7` | Hush runs `sh -c`'s command as a child, which outlives cancellation |
| `tst-cleanup2`, `tst-cleanupx2` | Expect a null-pointer store to fault |
| `tst-clock2` | Expects blocked threads' CPU clocks to advance |
| `tst-cond10`, `tst-cond20`, `tst-cond21` | Exceed the upstream internal timeout; the first two pass in direct mode |
| `tst-cancel14`, `tst-cancel15`, `ex3` | Intermittent: SIGSEGV, SIGSEGV, timeout |

The 64 KiB thread stack is a deliberate RAM choice. Without a guard, the
`tst-cancel2` overflow lands where layout puts it, so either test can pass
or crash. The intermittent three pass on fresh boots and when the same
batch is replayed. A race in asynchronous cancellation has not been ruled
out. Reports: `upstream-atfork-fixed`, `upstream-final-thread-regression`,
`upstream-final-cancel-fresh`, `upstream-final-batch12-repeat`,
`upstream-final-cond-direct`, and `upstream-suites/final-app.log`.

## Validated glob and hypotl fixes

uClibc's `glob()` opened a directory without metacharacters as a path,
backslash escapes and all. Hush escapes `-` in quoted text as `\-`, so
`for f in "$dir"/*` matched nothing whenever `$dir` contained a dash.
BusyBox's `runtest` uses that form, and found none of `cp`'s test cases
under `/upstream-work`. `0032-glob-unescape-literal-directory.patch`
removes the escapes first, as glibc does. `linux/uclibc/glob-test.c` fails
with the previous libc and passes with the fix. It is part of the Grifo
application checks.

`mathcalls.h` declares `hypotl` with `libm_hidden_proto`, but `w_hypotl.c`
never defined the public alias, so the long-double math drivers could not
link. `0031-libm-export-hypotl.patch` adds it. Reports: `upstream-glob-old`,
`upstream-glob-new` and `upstream-suites/product-app.log`.

## Isolation and reporting

`supervise.c` uses genuine `vfork` followed by `execv`. It never replaces
`fork` inside upstream tests. A deadline kills the test process group;
subreaper cleanup also kills and reaps descendants which establish their
own groups. The supervisor propagates exit status and signal termination.
Native checks cover those paths, including an escaped-group descendant.

Test files are staged on RAMFS to allow no-MMU execution and the tests'
shared file mappings. The ethers tests require a manually provided
`/etc/ethers`; the runner supplies their documented `teeth` entry on RAMFS.
An early full sweep lacked this fixture, so its two failures are retained
alongside separate successful fixture-corrected runs.

Default limits are 120 guest seconds per command/module and 1,200 host
seconds per boot. Both are adjustable. `case-fixtures.json` gives six slow
libc commands and three BusyBox groups 1,200 seconds. The libc ones also
get upstream's `TIMEOUTFACTOR`, so the test skeleton's own deadline yields
to the external one. The two fallocate commands and BusyBox's `cp` and `mv`
groups run in `/upstream-work` on the ext4 root. No-MMU RAMFS supports
neither fallocate nor files over 4 MiB, the largest contiguous block. Test
cards carry a copy of `linux.img` grown to 32 MB with `resize2fs`, since
`cp` writes a 5 MiB file; the product image stays 16 MB. A timeout retains assertions already
completed and remains a timeout. A crash triggers fresh boots for subsequent
unattempted commands. Output failure markers override a successful harness
exit, since upstream Hush does not propagate every failure through its exit
status. A zero exit without test assertions is untested, never a pass.

Reports retain raw UART logs, per-case output, invocation, exit status, and
SHA-256 hashes of the compiler, sources, binaries, archive, supervisor,
kernel, root filesystem, boot fixtures and emulator. The installed libc and
loader were compared with stripped copies of the compiler's runtime and
matched; BusyBox matched its preserved Buildroot target binary.

`summarize.py` audits inventories against raw logs and records follow-ups
separately. It recognizes entered tests which crash the emulator or panic
the kernel; it does not relabel unentered tests after a crash. It also
reports individual Hush coverage so a timed-out module cannot hide later
tests. Raw reports are never overwritten by the audit.

## Reproduction

Build on the Linux VM, then run on macOS from the repository root:

```sh
limactl shell wr-linux -- python3 \
  /Users/timdoug/wikireader/linux/upstream-tests/build.py \
  --output /Users/timdoug/wikireader/linux/artifacts/upstream-product-tests
limactl shell wr-linux -- python3 \
  /Users/timdoug/wikireader/linux/upstream-tests/check-supervisor.py
limactl shell wr-linux -- python3 \
  /Users/timdoug/wikireader/linux/upstream-tests/check-source.py \
  --output /Users/timdoug/wikireader/linux/artifacts/upstream-source-new
python3 linux/upstream-tests/check-results.py

limactl shell wr-linux -- bash -c 'cp linux/artifacts/linux.img /tmp/r.img &&
  truncate -s 32M /tmp/r.img && /sbin/e2fsck -fy /tmp/r.img;
  /sbin/resize2fs /tmp/r.img && cp /tmp/r.img linux/artifacts/upstream-root-32m.img'
python3 linux/upstream-tests/run.py uclibc \
  --binaries linux/artifacts/upstream-product-tests \
  --rootfs linux/artifacts/upstream-root-32m.img \
  --output linux/artifacts/upstream-uclibc-new --batch-size 12 --jobs 3
python3 linux/upstream-tests/run.py busybox \
  --binaries linux/artifacts/upstream-product-tests \
  --rootfs linux/artifacts/upstream-root-32m.img \
  --output linux/artifacts/upstream-busybox-new --batch-size 6 --jobs 3
```

Output directories must be new. The runner boots the standard
`linux/artifacts/linux.app`. `build.py --output` names the inventory
directory (`upstream-product-tests` here). Selected reruns accept repeated
`--case` arguments. Individual shell selections use, for example,
`--hush-case hush-misc/return1.tests`. `--direct` uses the upstream
test-skeleton's supported `-d` mode for explicitly selected libc cases; this
bypasses its internal fork/deadline wrapper and is identified separately
in reports. An external supervisor still bounds execution.

Hush tests without a result in a timed-out module are then selected
individually with `--hush-case`. `summarize.py` audits reports built from
one inventory, with those Hush runs and any reruns as follow-ups:

```sh
python3 linux/upstream-tests/summarize.py \
  --binaries linux/artifacts/upstream-product-tests \
  --uclibc linux/artifacts/upstream-uclibc-sweep3 \
  --busybox linux/artifacts/upstream-busybox-sweep3 \
  --hush linux/artifacts/upstream-busybox-sweep3-hush \
  --libc-followup linux/artifacts/upstream-uclibc-sweep3-slow \
  --busybox-followup linux/artifacts/upstream-busybox-sweep3-long \
  --output linux/artifacts/upstream-suites/summary-product.json
```

Run both suites even when the first returns nonzero. The builder uses
`make -k` and records unsuccessful compilation; the runner exits nonzero
for failures, build errors, crashes or timeouts while preserving all results.
