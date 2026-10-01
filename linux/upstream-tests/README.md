# Upstream BusyBox and uClibc suites

These runners build and attempt the complete upstream test inventories for
the installed C33 Linux configuration. They use the existing compiler,
uClibc-ng 1.0.59 and BusyBox 1.38.0. Every target trial boots
**FLASH -> Grifo -> launcher -> Linux**, with 32 MiB SDRAM and a disposable
virtual SD card. These are emulator results; the physical device and the
16 MiB configuration need separate qualification.

## Sources and execution

`build.py` uses the preserved Linux build under
`/home/timdoug.guest/wr-linux/ltp-isolated`. Its separate test checkout is
`/home/timdoug.guest/wr-linux/upstream-suites/uclibc-ng-test`, pinned to
`132c6134d69146bcafbdda68ae3a9cbd9f8fb921`, the revision selected by our
Buildroot 2026.08 package. It verifies that tracked upstream test sources
stay unchanged. BusyBox tests come from the exact source tree which built
the installed BusyBox.

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

The main inventory contains 524 libc commands (510 built, 14 build errors),
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

## First sweep results

The full libc run returned 392 passes, 51 skips, 62 failures, 3 timeouts,
14 build errors and 2 reproducible crashes. The last two were `MISSING`
in its original report and are identified by the audit of fresh retries.
The BusyBox full run has 43 passing groups, 14 groups with both passes and
skips, 37 skipped groups, 14 failing groups, 5 timeouts and 1 reproducible
crash, plus the separate host-only source group. Neither is a clean sweep.

After the recorded fixture/deadline follow-ups:

| Inventory and counting unit | Pass | Fail | Skip/untested | Other |
|---|---:|---:|---:|---|
| uClibc selected commands (524) | 396 | 60 | 51 | 14 build errors, 2 crashes, 1 timeout |
| BusyBox applet result markers (778) | 643 | 12 | 123 | None |
| Individual Hush tests (404) | 375 | 21 | 3 | 4 timeouts, 1 crash |

BusyBox's applet rows cover all 95 target applet groups: 36 pass outright,
14 contain passes and skips, 37 are skipped, and 8 fail. The 123 skipped
markers comprise 95 `SKIPPED` and 28 `UNTESTED` upstream markers. A skipped
modern applet group can represent multiple unavailable tests; the marker
total is not the size of an all-features BusyBox inventory. All 404 Hush
tests have an outcome after 78 individually selected follow-ups and fresh
retries for cases following a crash. Host
source lint separately has 2 passes and 3 failures: its applet-sorting and
obsolete-function/header checks flag current upstream source, not executed
C33 code.

Libc gains are the two ethers tests with their documented fixture and the
two crypt tests with suitable deadlines. SHA-256 crypt passes with its
original upstream wrapper and a 600-second external limit; SHA-512 crypt
passes in upstream direct mode with a 1,200-second external limit. Xargs
passes on the original BusyBox image with a 1,200-second limit. These are
individual follow-ups, not a replacement full sweep.

`upstream-busybox-stack-long` also passes all three md5sum assertions on a
private image with only the BusyBox ELF stack reservation changed from
32 KiB to 128 KiB. This experiment is excluded from the baseline counts.
No production stack configuration change is included: extra reserved stack
uses real memory on no-MMU Linux, and other large-stack cases need review.

The tested baseline kernel SHA-256 is
`af396910fd14320c90b76fb9ec598f55250cbe0f695c83d94980795e11439a65`,
root filesystem
`fde79ba6f6e0aa851e7cb8589d0fec57a715fdfdd7d0982de8260a0484ec6bec`,
and emulator
`c7f9ff2d5117c20ca37196df82ab6f0bbea103e591084b0f9bb0f26c6023d40d`.
All full-run and shell-remainder batches use that same baseline.

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
seconds per boot. Both are adjustable. A timeout retains assertions already
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
  /Users/timdoug/wikireader/linux/upstream-tests/build.py
limactl shell wr-linux -- python3 \
  /Users/timdoug/wikireader/linux/upstream-tests/check-supervisor.py
limactl shell wr-linux -- python3 \
  /Users/timdoug/wikireader/linux/upstream-tests/check-source.py \
  --output /Users/timdoug/wikireader/linux/artifacts/upstream-source-new
python3 linux/upstream-tests/check-results.py

python3 linux/upstream-tests/run.py uclibc \
  --kernel linux/artifacts/serdev-dt-only/linux.app \
  --rootfs linux/artifacts/ltp-rootfs-poll.img \
  --output linux/artifacts/upstream-uclibc-new --batch-size 12 --jobs 2
python3 linux/upstream-tests/run.py busybox \
  --kernel linux/artifacts/serdev-dt-only/linux.app \
  --rootfs linux/artifacts/ltp-rootfs-poll.img \
  --output linux/artifacts/upstream-busybox-new --batch-size 6 --jobs 2
```

Output directories must be new. The generic runner defaults to the standard
`linux/artifacts/linux.app` and `linux.img`; these commands name the exact
tested private fixtures instead. Selected reruns accept repeated `--case`
arguments. Individual shell selections use, for example,
`--hush-case hush-misc/return1.tests`. `--direct` uses the upstream
test-skeleton's supported `-d` mode for explicitly selected libc cases; this
bypasses its internal fork/deadline wrapper and is identified separately
in reports. An external supervisor still bounds execution.

The initial full runs live in `upstream-uclibc-full` and
`upstream-busybox-full`. Their early runner version called interrupted
trials `MISSING`; the audit preserves those raw reports while identifying
the fresh-boot cancellation, setjmp and command-substitution crashes from
their logs. `upstream-busybox-hush-remainder` runs every shell case which
lacked an upstream completion marker in that sweep, independently. This
includes later cases in modules which crashed or reached their deadline.

To reproduce the final audit (after those runs finish):

```sh
python3 linux/upstream-tests/summarize.py \
  --busybox linux/artifacts/upstream-busybox-full \
  --uclibc linux/artifacts/upstream-uclibc-full \
  --hush linux/artifacts/upstream-busybox-hush-remainder \
  --libc-followup linux/artifacts/upstream-uclibc-ethers-ramfs \
  --libc-followup linux/artifacts/upstream-uclibc-crypt-deadline \
  --libc-followup linux/artifacts/upstream-uclibc-sha512-direct \
  --busybox-followup linux/artifacts/upstream-busybox-xargs-long \
  --output linux/artifacts/upstream-suites/summary.json
```

Run both suites even when the first returns nonzero. The builder uses
`make -k` and records unsuccessful compilation; the runner exits nonzero
for failures, build errors, crashes or timeouts while preserving all results.

## Findings to investigate

The first sweep identifies concrete leads rather than establishing that
every non-pass is a C33 implementation bug:

- Several exception-enabled pthread cancellation/cleanup tests fault in
  `ld-uClibc-1.0.59.so+0x41c8` while writing to a misaligned address inside the
  executable. `tst-cancelx2` also reproducibly sends the emulator into
  unmapped execution. FDPIC forced unwinding and loader lookup need review.
- `setjmp/bug269-setjmp`, which combines repeated longjmp with variable
  length stack allocations, reproducibly corrupts memory and stops the
  emulator. Other setjmp tests passing does not clear this case.
- `dlopen/tst-origin` fails to find its packaged library through
  `$ORIGIN/testlib`, despite the ELF carrying the intended RPATH.
- BusyBox's checksum stress test overflows the configured 32 KiB main
  stack. A private larger-stack image is an experiment, not a shipped fix.
  Hush's large command substitution also produces a kernel panic in a
  fresh boot; its root cause remains open.
- BusyBox sed has three failures in tests explicitly marked as known
  upstream bugs. `SKIP_KNOWN_BUGS` is not set. Cpio tests require disabled
  `bzcat`, tar requires `bunzip2`, and unzip's setup requires absent `zip`;
  these remain failures rather than silently becoming skips. Some Hush
  failures require disabled `FLOAT_DURATION` or Unicode support. The leak
  tests require Hush's disabled `HUSH_MEMLEAK` debugging builtin; failures
  or timeouts in those tests do not establish a measured memory leak.
- Cp/mv's 5 MiB sparse-file setup fails on no-MMU RAMFS with `EFBIG` before
  reaching the copy or rename. A disk-backed working-directory fixture is
  needed to evaluate those particular operations.
- Libc locale tests request unavailable locales; some generated commands
  are utilities requiring arguments the upstream inventory does not supply.
  Large thread-count tests exceed available contiguous memory. Networking,
  message-queue notifications, file preallocation and MMU guard-page
  assumptions also encounter configuration or no-MMU limits.
- Four math drivers report small ULP differences against generic zero-ULP
  tolerances. They require numerical review. Two long-double drivers cannot
  link `hypotl`. Nine TLS-assembly tests lack C33 definitions in upstream
  `tls-macros.h`; existing native TLS integration tests remain a separate
  source of coverage. The remaining three build errors are unavailable
  ifaddrs, resolver or iconv interfaces.

Detailed reports and fixture provenance are under
`linux/artifacts/upstream-*`. Pass counts after fixture or deadline
corrections must be distinguished from a clean full sweep.
