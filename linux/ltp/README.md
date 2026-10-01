# LTP on C33 no-MMU Linux

These scripts run unchanged standalone Open POSIX tests from LTP revision
`2279d708c817db657611a30c427c7d06c4360049`. Every emulator trial uses
FLASH -> Grifo -> launcher -> Linux. The physical SD card is untouched.
The current trial uses wremu with 32 MiB SDRAM.

## Coverage and accounting

The build enumerates numbered `conformance/interfaces/*/*.c` cases,
excluding the speculative directories. Of 1,569 candidates, 1,236 build;
262 require unavailable `fork`, and 71 include unavailable `aio.h`.
Compile failures remain in the manifest with their commands and logs.
Each upstream case links its unchanged `lib/common.c` bootstrap using
C99, the suite's POSIX/XSI feature macros, `-O2`, pthreads, and librt.

The initial uniform thirty-second sweep and subsequent per-case observations:

| Status | Initial | Previous verified | After focused fixes |
|---|---:|---:|---:|
| Pass | 1,144 | 1,183 | 1,188 |
| Unsupported | 40 | 29 | 29 |
| Untested | 6 | 6 | 6 |
| Fail | 18 | 12 | 8 |
| Unresolved | 7 | 5 | 4 |
| Timeout | 9 | 1 | 1 |
| Other nonzero exit | 12 | 0 | 0 |

All 1,236 cases produced a result. The original binaries, root image, and
reports are retained under `linux/artifacts/ltp-all`,
`ltp-rootfs-before.img`, and `ltp-before`.

The previous full sweep rebuilt unchanged upstream cases with the corrected compiler
and used immutable `ltp-rootfs-final.img` and `ltp-kernel-before.app` fixtures.
Its raw counts are **1,179 pass and four execution errors**, with the other
final categories as shown above. Those four cases passed individually in
fresh boots, on the same default emulator and fixtures. The combined
per-case record is `ltp-verified/results.json`; it preserves each original
error and links its fresh-boot report. It is **not a clean batch sweep**.
The raw batch reports remain in `ltp-final-run`, and the four fresh boots
in `ltp-final-sd-affected-run`.

All ten missing results after the first batch's host deadline received fresh
boots and completed. In particular, unchanged `clock_gettime/4-1` performs
2.7 million `clock()` calls and passed with its 1,800-second guest budget.
The Makefile now allows a 2,400-second host budget per batch.

Of the 39 additional passes, eight come from appropriate deadlines, including
cases that already pass on the old image. The other 31 cover signal masks,
timer outputs, available clocks, and the large-frame compiler fix. No case
that initially passed failed its final individual observation. This is
functional coverage, not a speed measurement.

The matching unprivileged AArch64 Linux comparison returned 1,132 passes,
25 unsupported, 12 untested, 53 unresolved, 13 failures, and one timeout.
This is a reference, not a verdict: C33 runs as root and can exercise
real-time scheduling and credential changes that the native run cannot.
Its reports are under `ltp-native-all-run`.

## Bugs found and fixed

* **Signal actions:** libc's public `struct sigaction` contains a restorer
  pointer; C33's kernel layout does not. Passing it directly shifted both
  signal-mask words. The architecture wrapper now marshals the kernel
  structure explicitly, preserving the public libc layout. It copies old
  actions only on success, handles aliased input/output, and reports a null
  restorer because the kernel supplies signal return.
* **POSIX timer outputs:** the time64 kernel uses 64-bit nanosecond fields;
  libc uses 32-bit `long`. NPTL passed public `itimerspec` output buffers
  directly to `timer_gettime64` and `timer_settime64`, corrupting fields and
  adjacent memory. Kernel-layout temporaries now receive the results,
  followed by conversion on success. Optional and aliased outputs work.
* **Clock capabilities:** `_SC_MONOTONIC_CLOCK` tested only the legacy
  syscall number, and the process/thread CPU-clock queries failed to probe
  available clocks. Runtime capability checks now include time64 syscalls.
  Optional-feature header constants and test assertions are unchanged.
* **`vfork` failure:** saving the return address and `r0` left the argument
  base eight bytes short of the legacy sixteen-byte alignment before
  `__errno_location`. The error path now pads eight bytes and supplies its
  no-argument descriptor. The successful clone path is unchanged.
* **Large stack frames:** GCC's `add_sp_big` emits general-register
  arithmetic that changes condition flags. Its RTL omitted that clobber,
  allowing comparisons across the prologue. LTP's `sigaltstack/9-1` took
  the wrong branch before reaching `execl`. Declaring the flag clobber fixes
  the unchanged test, including against the old rootfs.

The local regressions are accounted separately from upstream passes.
They check both signal-mask words and failure-output preservation, timer
buffer canaries/aliasing/failure output, three clock capability queries,
large-frame branches, and short/long argument lists through genuine
`vfork` plus `execv`, `execl`, `execle`, and `execlp`. The `vfork` failure
regression includes the production assembly and intercepts only its errno
helper to measure entry alignment. It forces `EAGAIN` using `RLIMIT_NPROC`
after dropping root credentials. Its pre-fix helper entered at `SP % 16 == 4`;
the required and fixed value is twelve.
The separate SD regression checks file contents after leaving a prefetched
read stream idle and then reopening another file. All seven local cases
passed in `ltp-port-verified-run`, including the explicit signal-action
input/output alias check. The normal application checks also
passed: NPTL/TLS, pthreads, atomics, IPC, C++, launcher reboot, and suspend
with live-thread TLS (`ltp-app-final.log`).

## Follow-up fixes and remaining-case audit

Four more unchanged upstream cases pass after focused fixes:

* `shm_unlink/9-1`: translate sticky-directory `EPERM` into the shared-memory
  interface's required `EACCES`, preserving other errors. This matches the
  [glibc implementation](https://sourceware.org/pipermail/glibc-cvs/2021q2/073302.html).
  The earlier description of this as only an expectation difference was wrong.
* `mlockall/13-1` and `13-2`: no-MMU locking remains a no-op for resident
  memory, but its inline implementation now rejects zero, unknown flags,
  and `MCL_ONFAULT` alone. This changes a header implementation, so consumers
  must be rebuilt; replacing the runtime library alone is insufficient.
* `munmap/2-1`: `/dev/zero` wired both `read` and `read_iter`, which
  `kernel_read` rejects. No-MMU private mmap uses that operation to fill
  its copy. Removing the redundant legacy reader lets the existing iterator
  handle both ordinary and kernel reads. After fixing setup, the test exposed
  a second bug: unmapping an already unmapped range returned `EINVAL`.
  Valid absent ranges now succeed; zero length, address wraparound, and
  unaligned absent ranges still fail. Partial file unmaps remain restricted.

All **58 unchanged cases** in the affected locking, unmapping and POSIX shm
families completed, with no previously passing case regressing
(`ltp-remaining-run`). A further **27 mmap cases** retained their earlier
results (`ltp-mmap-remaining-run`); the expensive `mmap/10-1` and the
inapplicable `24-1` exhaustion loop were not repeated. All **eight local
regressions** pass (`ltp-port-remaining-run`). The added no-MMU case exercises
all six valid locking-flag combinations, invalid flags, ordinary/vector/empty
zero reads, independent private zero mappings, repeat unmap, and invalid
unmap ranges. The normal application, launcher reboot and live-thread TLS
suspend checks also pass (`ltp-app-remaining.log`). Every trial boots through
Grifo and uses the default emulator model.

The immutable follow-up fixtures are `ltp-rootfs-remaining.img` and
`ltp-kernel-memory.app`. The rootfs retains the prior image's contents except
for rebuilt libc and loader files. The private kernel source uses the checked-in
SD driver, with the earlier SD diagnostic experiments removed.
`ltp-remaining-verified/results.json` combines the 58 new observations with
the earlier per-case observations, preserving report hashes and the four
status changes. Its **1,187/1,236 passes (96.0%)** are not a new full sweep.

[remaining.json](remaining.json) records the raw status, source hash and
reason for all 48 remaining non-PASS interface cases after the additional
synchronization fix below. These categories explain the
results; they do not turn exclusions or failures into passes:

| Reason | Cases |
|---|---:|
| Optional features: sporadic scheduling, process-scope threads, memory locking | 23 |
| MMU page protection or fixed virtual-address mappings | 9 |
| Unimplemented no-MMU locking semantics | 6 |
| Permission-denial precondition absent; Linux permits the query | 3 |
| Undefined rwlock operations explicitly skipped by upstream on Linux | 2 |
| AIO placeholder with no implemented test body | 1 |
| Empty-file shared mapping setup unavailable on no-MMU | 1 |
| Virtual-address exhaustion loop inapplicable on no-MMU | 1 |
| Real-time rwlock priority-ordering bug | 1 |
| Semaphore count-limit precondition absent | 1 |

Literal 100% cannot be reached with this unchanged corpus on this hardware:
some cases deliberately return UNSUPPORTED or UNTESTED, and an MMU is needed
for protection assertions. Real work remains as well. The rwlock failure
requires priority-aware reader/writer admission and wakeup, including timed
and process-shared operations; waking readers indiscriminately is not a
complete fix. POSIX specifies
[priority order and writer precedence at equal priority](https://pubs.opengroup.org/onlinepubs/9699919799/functions/pthread_rwlock_unlock.html).
The no-MMU locking APIs still need coherent kernel range/limit/lock-state
handling before their broader contracts can be advertised. Those are
implementation limitations, not all architectural impossibilities.

## Extended coverage and resident-memory interfaces

`build.py --extended` adds the pinned suite's functional and behavior cases
and compiles its header-definition checks separately. It enumerates 20
runtime candidates and 261 header checks. Thirteen runtime cases build and
pass; seven require unavailable fork. Their source and the upstream bootstrap
remain unchanged. Header checks compile with `-c` and are **not runtime
passes**. Every failed compilation retains its command, source hash and log.

The header checks found an advertised-interface bug: `_POSIX_ADVISORY_INFO`
was 200809L, but no-MMU builds hid `posix_madvise` and its five constants.
These declarations and the real implementation are now available. The kernel
accepts normal/random/sequential/will-need advice after validating alignment,
overflow and coverage by the calling process's VMAs. Memory is already
resident, so accepted hints do not change contents. POSIX DONTNEED uses the
same validation without invoking Linux's destructive MADV_DONTNEED operation.
The wrapper returns error numbers directly and preserves errno. This follows
the [POSIX requirement that advice preserve memory-access semantics](https://pubs.opengroup.org/onlinepubs/009604399/functions/posix_madvise.html).
Header successes rise from **249/261 to 255/261**. The remaining six errors
are five unavailable AIO header checks and the missing `getdate` declaration;
they remain recorded as errors.

One further original interface failure, `mmap/14-1`, now passes. C33's
public `msync` was an inline no-op; it now calls the kernel through the
normal cancellation-point wrapper. The existing kernel msync implementation
also builds for NOMMU, validates flags/ranges and synchronizes shared files.
Because direct shared stores do not generate write faults, explicit
MS_SYNC/MS_ASYNC conservatively marks writable shared mapping timestamps.
Private and read-only mappings leave timestamps unchanged, and private
changes stay private. Length-rounding overflow is rejected before it can
turn into a zero-length operation. Other no-MMU libc targets retain their
existing msync wrapper behavior.

This supplies explicit synchronization; it does **not** detect individual
stores or make all no-MMU mapping semantics conform to MMU behavior.
An unchanged writable mapping can receive a conservative timestamp update
when explicitly synced. Applications rebuilt for the C33 msync wrapper
require the matching kernel implementation; older C33 kernels return ENOSYS.
Already compiled inline no-op callers need rebuilding to benefit.

The latest immutable fixtures are `ltp-rootfs-msync.img` and
`ltp-kernel-msync.app`. All **85 affected original cases** were rebuilt and
rerun: `mmap/14-1` changes FAIL to PASS, with no lost passes
(`ltp-mmap-msync-run`, `ltp-locking-msync-run`). Combined original-interface
observations are **1,188/1,236**, retained with report hashes in
`ltp-msync-verified/results.json`; this is not a new full sweep.
The thirteen new runtime observations (`ltp-extended-msync-run`) are counted
separately. `behavior/timers/1-1` performs no creations when `_SC_TIMER_MAX`
reports an indeterminate limit, so its raw PASS does not establish timer
capacity. `behavior/timers/2-1` explicitly accepts an indeterminate limit.

All **ten local regressions** pass (`ltp-port-msync-run`). The advice case
checks every hint, unchanged data and errno, invalid/overflowing ranges,
and a hole between two remaining anonymous mappings. The synchronization
case checks both timestamps, synchronous/asynchronous visibility, private
and read-only isolation, invalid flags/addresses/lengths, unmapped holes,
and pending deferred cancellation at msync. These same new regression
binaries fail against kernels missing the corresponding implementation
(`ltp-advice-old-kernel-run`, `ltp-msync-old-kernel-run`). Updated public
headers also compile as C++20. The normal application, launcher reboot and
live-thread TLS suspend tests pass (`ltp-app-msync.log`). All emulator runs
retain the default model and FLASH -> Grifo -> launcher boot path.

## Emulator DMA limitation found during the sweep

Running `timer_create/10-1` immediately before a new SD executable can leave
subsequent reads stalled. The first case passes; following launches return
127. An isolated diagnostic reproduces the same read failure inside one
process after a timer signal interrupts a tight loop, so it does not require
`vfork`, exec, or process teardown. Both CPU-time and monotonic timers trigger
it. The seven local regressions above do not include this tight-loop case.

The emulator's DMA contention model adds fifteen cycles to each pending
write for every CPU data access to SDRAM. Dense reads can accumulate a
large future delay that persists after the loop has ended. The existing
[emulator timing notes](../../emulator/README.md) already describe this
model's failure to represent bounded arbitration under dense traffic.
The card supplies the token, but the pending DMA writes stop making timely
progress. The unchanged two-case sequence passes with
`WREMU_MODEL=dma_cpu_penalty=0` (`ltp-sd-no-contention`); a private experiment
bounding the accumulated delay also passes (`ltp-sd-bounded-contention`).
Neither diagnostic is included in the default-model pass counts.

No speculative SD driver change or emulator timing change is adopted.
The proposed driver cleanup did not solve the failure. A contention fix
needs comparison with the physical device's arbitration and timing, rather
than silently changing a fitted parameter to make the suite green. The
failure can also disrupt guest filesystem writes, so all trials use disposable
virtual cards. The physical card remains untouched.

Reproduce the default-model failure using the final fixtures:

```sh
python3 linux/ltp/run.py --binaries linux/artifacts/ltp-final \
  --rootfs linux/artifacts/ltp-rootfs-final.img \
  --kernel linux/artifacts/ltp-kernel-before.app \
  --case timer_create/10-1 --case timer_gettime/1-1 \
  --output linux/artifacts/ltp-sd-repro-new
```

## Deadlines and limitations

Exit status is authoritative: 0 pass, 1 fail, 2 unresolved, 4 unsupported,
5 untested, 124 supervised timeout, and other values reported separately.
A complete trial requires all result markers and a clean reboot to Grifo.
Logs and fixture/binary hashes are retained. Each test has a private
working directory and process group; the supervisor kills remaining group
members after the test. It launches with genuine `vfork` then `exec`, rather
than running arbitrary test code in a shared-memory child.

`timeouts.json` increases budgets for the expensive unchanged string
cases, the 100,000-iteration mmap case, and two timer cases. All five string
cases and `mmap/10-1` pass on the old image with those larger budgets;
`timer_settime/2-1` deliberately waits 2+4+6+8+10 seconds. Those improvements
are deadline corrections, not libc fixes. `strncpy/2-1` also has an upstream
one-byte overflow in its own allocation; its raw pass is retained without
claiming memory-safety coverage.

Remaining no-MMU memory-locking and fixed/protected-mapping
expectations are not converted into passes. `mmap/24-1` expects repeated
shared mappings to exhaust virtual address space; no-MMU can reuse one
mapping, so a timeout does not demonstrate an allocation leak.
The real-time rwlock priority-ordering failure remains to be resolved;
the unprivileged native failure does not validate real-time ordering.
The `shm_unlink` permission error and valid repeat-unmap behavior are now
corrected as described in the follow-up section above.

The main LTP harness forks a worker even for tests without explicit fork.
Replacing that with vfork would change its semantics, and Buildroot's LTP
package requires an MMU. This trial therefore covers standalone POSIX
interfaces, not the complete LTP syscall, stress, or filesystem suites.
Physical-device and 16 MiB runs remain separate work.

## Reproduction

With the existing compiler, kernel, and root filesystem:

```sh
make -C linux ltp-test
make -C linux ltp-all-test
make -C linux ltp-port-test
```

To reuse already built binaries and choose an immutable rootfs fixture:

```sh
python3 linux/ltp/sweep.py --binaries linux/artifacts/ltp-all \
  --rootfs linux/artifacts/linux.img --output linux/artifacts/ltp-sweep \
  --batch-size 30 --jobs 3 --timeouts linux/ltp/timeouts.json
python3 linux/ltp/run.py --binaries linux/artifacts/ltp-all \
  --case sigaction/8-1 --output linux/artifacts/ltp-focused
```

`build.py --list FILE` builds a specified subset, and `--port-tests` builds
only the local regressions. `--extended` builds functional/behavior runtime
cases and records header-definition checks in a separate `compile_checks`
manifest field. These selectors are mutually exclusive. It refuses dirty or differently pinned LTP
checkouts rather than resetting them. `run.py --timeout N` sets a default
in guest seconds; `--timeouts FILE` overrides selected cases and
`--wall-timeout N` bounds the host trial. `sweep.py` retries missing cases
in fresh boots and retains each original log, retry, and result.
`native.py` runs separately compiled native cases under the same supervisor
in temporary directories; it should remain unprivileged.
`--kernel` and `--emulator` select immutable images for comparisons.
Reports hash the emulator before launch and record `WREMU_*` environment
overrides, including any diagnostic timing-model changes.

## Synchronization across unmapped holes

The follow-up kernel fixture `ltp-kernel-sync-gaps.app` uses a VMA iterator
instead of `find_vma`, whose no-MMU implementation only finds a containing
mapping. A leading or internal hole previously stopped msync before later
shared mappings. The iterator synchronizes those mappings while retaining
ENOMEM for the unmapped portions, and is reset whenever file I/O drops the
mapping lock. No-MMU MS_ASYNC also traverses holes for its timestamp updates;
the MMU asynchronous fast path is unchanged.

The expanded local synchronization regression maps the first page read-only
and the third page writable in a four-page contiguous ramfs file. The two
remaining pages have backing storage but no VMA, providing deterministic
leading, internal and trailing holes without depending on allocation luck.
Both synchronous and asynchronous calls must update the writable mapping's
timestamps and report ENOMEM. The read-only mapping cannot mask a skipped
update. The exact same binary fails on `ltp-kernel-msync.app`
(`ltp-sync-gaps-before-final`) and passes on the new kernel. All ten local
regressions pass (`ltp-port-sync-gaps-run`), the 27 unchanged mmap outcomes
are preserved (`ltp-mmap-sync-gaps-run`), and application, launcher and
live-thread suspend checks pass (`ltp-app-sync-gaps.log`). These runs reuse
`ltp-rootfs-msync.img` and the default emulator through Grifo. The combined
original-interface count remains **1,188/1,236**; this new regression is
separate coverage, not another upstream pass.

## Timerfd time64 output conversion

The timerfd wrappers had the same layout mismatch previously fixed in POSIX
timers: the kernel writes a 32-byte time64 itimerspec, while C33's public
itimerspec occupies 24 bytes. Both timerfd_gettime and timerfd_settime's
old-value output now use kernel-layout temporaries and copy the four fields
only on success. Aliased input/output remains supported, the optional
old-value pointer may be NULL, and the non-time64 path is unchanged.

The new `c33_timerfd_layout/1-1` regression checks surrounding guard words,
unarmed and active timer values, aliased input/output, seconds beyond
INT32_MAX, and unchanged output on invalid-input/bad-descriptor errors.
The same binary fails on `ltp-rootfs-msync.img` (`ltp-timerfd-before`) and
passes on `ltp-rootfs-timerfd.img` (`ltp-port-timerfd-run`). It also passes
with native glibc. All eleven local regressions and the normal application,
launcher and live-thread suspend checks pass on the corrected fixture
(`ltp-app-timerfd.log`), using `ltp-kernel-sync-gaps.app` through Grifo.
This adds Linux-specific local coverage; it does not change the original
Open POSIX pass count.

## Condition-variable clock selection on time64-only kernels

The generic NPTL timed-wait implementation already converts time64 clock
output, but its outer syscall guard required the legacy clock_gettime
number. C33 has only clock_gettime64, so the implementation fell back to
gettimeofday even for a CLOCK_MONOTONIC condition variable. Include the
time64 syscall in that guard to reach the existing selected-clock path.

The local `c33_condvar_clock/1-1` regression moves realtime forward one
hour in a disposable emulator boot, checks 100 ms condition deadlines for
both clocks, verifies mutex reacquisition, and restores realtime with the
elapsed monotonic duration. It fails on `ltp-rootfs-timerfd.img`
(`ltp-condvar-clock-before`): the monotonic wait expires in about **0.2 ms**,
while the realtime wait takes about **101 ms**. The same binary passes on
`ltp-rootfs-condvar-clock.img`: both waits take about **101 ms**. This test
must run only in the disposable emulator; it changes the system clock.

All **twelve local regressions** pass (`ltp-port-condvar-clock-run`), all
**48 unchanged upstream condition-variable cases** pass
(`ltp-condvar-upstream-run`), and application, launcher and live-thread
suspend checks pass (`ltp-app-condvar-clock.log`). The latest root filesystem
contains both new libc fixes and uses `ltp-kernel-sync-gaps.app` through
Grifo with the default emulator. The combined original-interface score
remains **1,188/1,236**: these fixes cover previously untested contracts,
not changes to upstream tests or another full sweep. Memory-locking state
and real-time rwlock admission/wakeup ordering remain unfinished.

## I/O timeout validation and writeback

Two further local regressions find errors in the emulated time64 I/O paths.
`recvmmsg` converted its input timeout to a temporary but discarded the
kernel's remaining-time result. The wrapper now retains that temporary and
copies its fields back after receiving messages, preserving NULL timeouts
and unchanged error outputs. `select` converted negative microseconds to an
unsigned value, normalized negative seconds into positive intervals, and
could overflow time_t during normalization. It also discarded the timeout
updated by its underlying pselect6 syscall. Validate negative fields first,
saturate oversized normalization, and copy the remaining timeout back.
The existing nonnegative GNU normalization extension remains supported.
This restores [Linux select timeout writeback](https://man7.org/linux/man-pages/man2/select.2.html);
POSIX permits either writeback behavior. The input-only public timeouts of
pselect and ppoll remain unchanged.

The new `c33_io_waits/1-1` checks readiness, seconds above INT32_MAX, malformed
fields, normalization overflow, expiry, remaining time after signals with
and without SA_RESTART, and atomic signal masks including an upper-word
realtime signal. `c33_unix_io/1-1` checks stream vectors, EOF, descriptor
passing with MSG_CMSG_CLOEXEC, datagram batch results and remaining time,
error-output preservation, socket send/receive timeout layouts and bounded
backpressure. Both binaries fail on `ltp-rootfs-condvar-clock.img`
(`ltp-io-epoll-before`) and pass with the timeout fixes on `ltp-rootfs-io.img`
(`ltp-timeouts-fixed`). Both also pass on native Linux. All trials use
`ltp-kernel-sync-gaps.app`, the default emulator and the Grifo boot path.

## Epoll fallback validation and cancellation

C33 exposes epoll_create1 and epoll_pwait, without the older epoll_create
and epoll_wait syscall numbers. The libc fallback for epoll_create ignored
nonpositive size arguments, creating a descriptor instead of returning
EINVAL. The epoll_wait fallback also bypassed the cancellation wrapper,
so deferred cancellation left the thread blocked indefinitely. Validate
legacy creation sizes and use one cancellable wait wrapper for both syscall
paths, supplying all six epoll_pwait arguments.

`c33_fd_events/1-1` reproduces both invalid-size cases and verifies eventfd
counter/semaphore behavior, saturation and short-buffer errors, epoll's
64-bit event payload, one-shot disabling/rearming, and timerfd periodic and
absolute readiness on both clocks. `c33_blocking_cancel/1-1` exercises
pending and blocking cancellation for read, poll, ppoll, select, pselect,
recv, recvmmsg, epoll_wait and epoll_pwait, checking exactly one cleanup and
intact TLS after each join. On the previous runtime the event test fails and
the cancellation test reaches its supervised timeout at epoll_wait
(`ltp-io-epoll-before`). Both pass on `ltp-rootfs-io-epoll.img`
(`ltp-port-io-epoll-run`) and on native Linux. The cancellation handshake
confirms entry into the worker; a short delay lets it reach its empty wait,
but is not a kernel-level proof that every trial was already sleeping.
