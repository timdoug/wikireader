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

| Status | Initial | Previous verified | After focused fixes | After rwlock fixes | After memory locking | After mapping ownership |
|---|---:|---:|---:|---:|---:|---:|
| Pass | 1,144 | 1,183 | 1,188 | 1,189 | 1,196 | 1,197 |
| Unsupported | 40 | 29 | 29 | 29 | 28 | 28 |
| Untested | 6 | 6 | 6 | 6 | 6 | 6 |
| Fail | 18 | 12 | 8 | 7 | 4 | 4 |
| Unresolved | 7 | 5 | 4 | 4 | 1 | 1 |
| Timeout | 9 | 1 | 1 | 1 | 1 | 0 |
| Other nonzero exit | 12 | 0 | 0 | 0 | 0 | 0 |

All 1,236 cases produced a result. The original binaries, root image, and
reports are retained under `linux/artifacts/ltp-all`,
`ltp-rootfs-before.img`, and `ltp-before`.

The previous full sweep rebuilt unchanged upstream cases with the corrected compiler
and used immutable `ltp-rootfs-final.img` and `ltp-kernel-before.app` fixtures.
Its raw counts are **1,179 pass and four execution errors**, with its other
categories preserved in the raw report. Those four cases passed individually in
fresh boots, on the same default emulator and fixtures. The combined
per-case record is `ltp-verified/results.json`; it preserves each original
error and links its fresh-boot report. It is **not a clean batch sweep**.
The raw batch reports remain in `ltp-final-run`, and the four fresh boots
in `ltp-final-sd-affected-run`.

All ten missing results after the first batch's host deadline received fresh
boots and completed. In particular, unchanged `clock_gettime/4-1` performs
2.7 million `clock()` calls and passed with its 1,800-second guest budget.
The Makefile now allows a 2,400-second host budget per batch.

Of the 39 additional passes in that earlier follow-up, eight come from appropriate deadlines, including
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
`24-1` exhaustion loop were not repeated in that batch. The earlier
classification of the latter as inapplicable was wrong; see the mapping
ownership fix below. All **eight local
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
reason for the current 38 remaining non-PASS interface cases, following
the synchronization, memory-locking and mapping-ownership fixes below. These categories explain the
results; they do not turn exclusions or failures into passes:

| Reason | Cases |
|---|---:|
| Optional features: sporadic scheduling and process-scope threads | 22 |
| MMU page protection or fixed virtual-address mappings | 8 |
| Permission-denial precondition absent; Linux permits the query | 3 |
| Undefined rwlock operations explicitly skipped by upstream on Linux | 2 |
| AIO placeholder with no implemented test body | 1 |
| Empty-file shared mapping setup unavailable on no-MMU | 1 |
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

Fixed/protected-mapping expectations are not converted into passes.
The earlier classification of `mmap/24-1` as a virtual-address-only
limitation was wrong: it also accepts exhaustion of the configured mapping
count. The ownership and limit fixes below make that unchanged case pass.

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
remains unfinished; the later rwlock correction is described below.

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

## Descriptor-test audit and raw futex coverage

[syscall-candidates.json](syscall-candidates.json) records the source hashes
for 37 pinned LTP units in poll, ppoll, pselect, epoll_wait, eventfd and
timerfd. Every unit uses the modern C API whose test worker is launched with
fork, including units without an explicit fork in their body. This is an
audit, not 37 build attempts or runtime passes. The upstream sources and
assertions remain untouched; running them unchanged requires further runner
support. No fork call is silently replaced with vfork.

The additional local `c33_futex_waits/1-1` directly exercises the kernel's
time64 futex ABI: wide-timeout value mismatch, invalid nanoseconds, relative
expiry, absolute monotonic/realtime expiry, expired deadlines, invalid
bitsets and selective wakeups. It passes both before and after the libc
changes. The two-waiter wake test uses an entry handshake and scheduling
delay, with two-second deadlines to bound a failed wakeup.

All **17 local regressions** pass on `ltp-rootfs-io-epoll.img` with
`ltp-kernel-sync-gaps.app` (`ltp-port-io-epoll-run`). The exact same five new
binaries on the preceding runtime produce **one PASS, three FAIL and one
TIMEOUT** (`ltp-io-epoll-before`). All five new programs also pass on native
Linux (`ltp-io-native.log`). Application, launcher reboot and live-thread TLS
suspend checks pass (`ltp-app-io-epoll.log`). Fixture, source and report hashes
are collected in `ltp-io-followup.json`. No kernel, toolchain ABI or emulator
timing changes were needed, and the physical card remains untouched.
The original upstream interface observations remain **1,188/1,236**; this
batch adds separate Linux-specific coverage rather than another full sweep.

## Realtime rwlock handoff and timed-wait cleanup

The unchanged `pthread_rwlock_unlock/3-1` now passes. The old NPTL unlock
always selected a writer, letting a lower-priority writer overtake a
higher-priority reader. The C33 implementation selects across one wait list,
using current assigned priorities and favoring writers at equal priority,
as required by [POSIX rwlock unlock](https://pubs.opengroup.org/onlinepubs/9699919799/functions/pthread_rwlock_unlock.html).
It reserves ownership before waking the selected thread. Admission follows
the same rule for blocking, try and timed locks, including ordinary-priority
readers arriving behind realtime writers.

Reader ownership records preserve recursive read locking while writers are
waiting. Timed waits use absolute realtime time64 futex deadlines, retry
signals, remove expired waiters, and honor a grant committed during a
timeout race. Removing the last queued writer also admits eligible readers
while another reader still holds the lock. The new regression exposes this
additional bug in the old implementation: those readers could remain asleep
until a later unlock.

This is specific to C33 no-MMU Linux: stack wait nodes and TLS/heap owner
records have the same physical addresses in independent processes. The
32-byte public object, static initializers and existing exported symbols
remain intact, but two internal words now hold list pointers. Old statically
linked rwlock implementations must be rebuilt before sharing a lock with
the new implementation. Existing dynamically linked callers use the new
functions without changing their declarations or object size.

Four simultaneous distinct read locks use embedded owner records, adding
64 bytes of libc TLS per thread; further locks allocate records and can
return EAGAIN on exhaustion. Reader operations add ownership bookkeeping,
and contended handoff queries each waiter's current priority once. This is
a correctness change, with no claimed runtime speedup. In the measured
fixture the stripped shared libc decreases from 501,980 to 501,720 bytes.

The new local coverage comprises:

* `c33_rwlock_priority/1-1`: FIFO/RR, ordinary/timed ordering, both increases
  and decreases of blocked-thread priorities, equal-priority writer ties,
  reader cohorts, recursive reads, new-reader admission and try-lock exclusion.
* `c33_rwlock_waits/1-1`: invalid, expired and post-2038 deadlines, signal
  retry, errno preservation, timeout removal, owner-record reuse/overflow,
  512 contended operations, and independently exec'd processes sharing a
  lock and semaphores through System V shared memory.
* `c33_rwlock_static/1-1`: the same waits/ownership/process checks linked
  statically, exercising static NPTL and TLS initialization as well.

The two dynamic binaries fail/time out on the previous libc and pass on the
new libc. Worker-entry handshakes plus scheduling delays allow them to reach
blocking calls; they are not kernel-level proofs of a blocked state. All
trials retain the default emulator model and full Grifo boot path.

The release fixture is `ltp-rootfs-rwlock-release.img` with the unchanged
`ltp-kernel-sync-gaps.app`. All **349 unchanged pthread cases** complete:
**347 PASS and two UNSUPPORTED**. The latter are the upstream skips for
undefined unlock operations; there are no remaining pthread failures.
All 41 rwlock-family cases are included in that sweep. The existing and new
dynamic local regressions pass **19/19**, and the static case passes **1/1**,
for **20/20 local regressions** across the two reports. Application, TLS,
atomics, IPC, C++, launcher reboot and live-thread suspend checks pass.

Reports are `ltp-pthread-rwlock-release-run`, `ltp-port-rwlock-release-run`,
`ltp-rwlock-static-run`, `ltp-rwlock-release-before`, and
`ltp-app-rwlock-release.log`. `ltp-rwlock-followup.json` records their hashes
and source/fixture identity. The combined original-interface observations
advance from **1,188 to 1,189 of 1,236 (96.2%)**, retaining all prior passes.
`ltp-rwlock-verified/results.json` merges the fresh pthread sweep into the
previous per-case record; it is **not a new 1,236-case sweep**.
[remaining.json](remaining.json) now audits the remaining **47 non-PASS**
cases. Coherent no-MMU memory-lock accounting remains the main unfinished
implementation work in that original interface set.


## No-MMU memory-lock validation and accounting

Seven more unchanged upstream bodies pass:

* `mlock/12-1` and `mlockall/15-1`: reject locking with EPERM when an
  unprivileged process has a zero `RLIMIT_MEMLOCK`.
* `mlock/8-1` and `munlock/10-1`: reject unmapped ranges with ENOMEM.
* `mlockall/3-6`: `MS_INVALIDATE` rejects this process's locked pages with
  EBUSY, including shared mappings.
* `mmap/18-1`: `MCL_FUTURE` enforces the process's limit, returning EAGAIN
  before attempting an oversized backing-file mapping.
* `munlockall/5-1`: the now-advertised `_POSIX_MEMLOCK` capability selects
  the unchanged test body, which successfully unlocks all mappings.

C33's RAM is already resident. The kernel now implements the API's validation,
limits and per-process state using a lazy bitmap indexed by RAM PFN, protected
by `mmap_lock`. Overlapping locks do not double-count; partial unlocks and
unmaps clear exactly the affected pages. Different processes have independent
lock state, while threads share it. `MCL_FUTURE` applies to new mappings;
`munlockall` and exec clear its policy and accounting. `MLOCK_ONFAULT` and
`MCL_ONFAULT` have the same residency effect as ordinary locking on this
no-MMU target. Non-RAM device mappings are already resident and do not consume
RAM locking quota. `VmLck` exposes the charged pages in `/proc/PID/status`.

The bitmap costs **1 KiB per process that locks RAM** on the current 32 MiB
machine, plus a four-byte pointer per `mm_struct`; `mlockall(MCL_CURRENT)`
builds a temporary bitmap before committing state. No VMA splits, page
reference changes or new syscall numbers are needed. Locking does not add
MMU protection or swapping. The generic backend is selected only for C33;
other no-MMU libc targets retain their previous behavior.

During lifecycle work, no-MMU `mremap` was also found to change `vm_end`
without resizing its VMA lookup-tree entry or `total_vm`. The resize now updates
both, along with the file interval tree and lock accounting. Tree allocation
precedes changes, so a failed locked growth preserves the old mapping and
its accounting. Late mmap failures release a reused region's attempted
reference instead of destroying another process's live mapping.

The new `c33_memory_lock/1-1` covers page rounding, overlap, partial unlock
and unmap, unmapped holes, address overflow, exact `VmLck` accounting,
partial `MS_INVALIDATE`, anonymous and private-file `mremap`, denied growth,
`MAP_LOCKED`, raw `mlock2`, all legal locking flags, future-policy replacement,
limits after dropping privileges, privileged bypass, thread sharing and exec
reset. An independently exec'd child performs repeated limit-denied mappings
of a live shared region; the parent's mapping and lock must survive. The
ELF-FDPIC loader reserves no brk growth, so the brk check verifies rejection
without changing the break or accounting; it does not claim successful heap
growth coverage for other loaders.

All **85 rebuilt unchanged memory-family cases** complete: **75 PASS,
four FAIL, three UNSUPPORTED, two UNTESTED and one UNRESOLVED**. Every prior
pass remains a pass. All **21 local regressions pass**, including the existing
static rwlock case. Application, NPTL/TLS, atomics, IPC, C++, launcher reboot
and live-thread suspend checks pass. All runs retain the full Grifo boot path
and default emulator model; the physical card remains untouched.

The release fixtures are `ltp-rootfs-mlock.img` and `ltp-kernel-mlock.app`.
The rootfs derives from the rwlock release with only libc and loader replaced
and byte-checked after installation. Reports are `ltp-memory-mlock-release-run`,
`ltp-port-mlock-release-run` and `ltp-app-mlock-release.log`.
The exact seven newly passing upstream binaries, using the new libc on the
previous kernel, yield **six UNRESOLVED and one FAIL**, with no passes
(`ltp-memory-mlock-before`). Those statuses differ from the old inline-no-op
binaries because the new wrappers encounter missing kernel syscalls.

Consumers of the previous inline no-op locking calls must be rebuilt.
Replacing their runtime library alone cannot change those calls. The new libc
wrappers require the new kernel; no compiler calling convention changes are
involved. The stripped shared libc grows from **501,720 to 502,056 bytes**.

`ltp-mlock-followup.json` records source, fixture and report hashes.
`ltp-mlock-verified/results.json` merges the fresh 85-case report with earlier
observations for the other 1,151 cases. The combined total advances from
**1,189 to 1,196 of 1,236 (96.8%)**; this is **not a new full sweep**.
[remaining.json](remaining.json) now contains **40 non-PASS** cases: four
failures, one unresolved, one timeout, six untested and 28 unsupported.
The empty-file shared mapping setup in `mlockall/3-7` still fails before its
locking assertion; it has not been counted as fixed.


## Exact-alias ownership and no-MMU mapping limits

Revisiting `mmap/24-1` found two real no-MMU mapping bugs. A repeat shared
mapping returns the same physical address, but the Maple Tree stores only
one VMA per range. Registering the new VMA replaced the old pointer without
releasing or retaining its ownership record. The old record's file, region
and callback references leaked. Unmapping the newer result also removed the
lookup entry for the still-live older mapping; both `mlock` and `msync` then
returned ENOMEM for it.

Each tree entry now retains a chain of exact aliases. Every alias keeps its
own VMA, file reference, region reference and mapping callbacks. Unmap removes
one record and restores the preceding one; the final unmap clears the range's
locks. Exit releases the entire chain, yielding between records, so System V
attachment counts and callback cleanup stay balanced. Append and pop are
constant time, and exit/proc metadata accounting are linear in retained
records. The change adds one four-byte pointer per no-MMU VMA; it introduces
no new userspace structure or compiler calling convention.

Nonidentical overlapping ranges now return ENOMEM before registration,
preserving the existing mapping and its locks. The previous behavior could
clip a tree entry while leaving its VMA bounds unchanged. Supporting those
partial overlaps would require a different representation; they are not
silently treated as exact aliases. `/proc/PID/maps` continues to show physical
address ranges once, while memory accounting includes each retained VMA
record without charging its shared physical range once per alias. Whole and
partial unmap also decrease `total_vm`.

The existing `max_map_count` variable was checked for VMA splits but not new
no-MMU mappings, and its sysctl was registered only by the MMU backend. The
no-MMU backend now exposes `/proc/sys/vm/max_map_count` and enforces the limit
before allocating a new mapping record, including exact aliases. This limits
actual ownership records rather than adding a special test-only ceiling.

The unchanged `mmap/24-1` now passes. Its own setup caps the sysctl at 65,530;
the final fixture completes **65,520 additional mappings** (67,092,480 bytes
of logical mapping requests) before ENOMEM, with ten existing mappings in
that process. The backing buffer remains one resident shared allocation.
Both kernels time out with the original thirty-second guest deadline. With
**the same 180-second deadline**, the new kernel passes and the old kernel
still times out. `timeouts.json` records that budget; the pass requires the
kernel fixes as well as enough time to build and release the records.

The new separate `c33_nommu_mappings/1-1` covers duplicate mmap ownership,
locks through an intermediate unmap, partial-overlap rejection, exact aliases
with different requested permissions, anonymous and alias limits, slot reuse,
128 alias removals, and metadata accounting that returns to its baseline.
It checks System V `shm_nattch` across duplicate attachments and an independently
exec'd child that leaves 32 attachments for process exit. The exact final
binary fails on the previous kernel, where the first intermediate unmap
makes the older mapping disappear.

All **86 unchanged memory-family cases** complete: **76 PASS, four FAIL,
three UNSUPPORTED, two UNTESTED and one UNRESOLVED**, with no prior pass lost.
All **22 local regressions** pass, including the static rwlock case. Application,
NPTL/TLS, atomics, IPC, C++, launcher reboot and live-thread suspend checks
pass. Every trial retains FLASH -> Grifo -> launcher -> Linux and the default
emulator model; the physical SD card is untouched.

The new kernel fixture is `ltp-kernel-mappings.app`; libc and rootfs remain
`ltp-rootfs-mlock.img`. Reports are `ltp-memory-mappings-final-run`,
`ltp-port-mappings-final-run`, `ltp-mappings-final-before`,
`ltp-mapping-limit-before-long`, and `ltp-app-mappings-final.log`.
The earlier thirty-second reports remain in `ltp-memory-mappings-release-run`
and `ltp-mapping-limit-before`. `ltp-mappings-followup.json` records source,
fixture and report hashes. `ltp-mappings-verified/results.json` refreshes the
86 observations and retains prior results for the other 1,150 cases.

The combined original-interface record advances from **1,196 to 1,197 of
1,236 (96.8%)**, with **no remaining timeouts**. This is not a new full sweep.
[remaining.json](remaining.json) now audits **39 non-PASS** cases: four failures,
one unresolved, six untested and 28 unsupported. The four failures still
require MMU protection or fixed virtual-address placement, and `mlockall/3-7`
still fails its beyond-EOF shared-mapping setup.

## Fixed-range validation

`mmap/24-2` checks ENOMEM for an impossible fixed address range, not successful
MAP_FIXED placement. The earlier audit wrongly described its errno as an
architectural impossibility. NOMMU now validates rounded length and fixed
address alignment, then compares the address against `TASK_SIZE - rounded_len`
before returning EINVAL for otherwise valid unsupported fixed placement.
The subtraction avoids address addition overflow. Ordinary address hints
remain ignored, and rejected requests never change an existing mapping.
The documented [mmap errors](https://man7.org/linux/man-pages/man2/mmap.2.html)
include ENOMEM for an address outside the process address space.

The unchanged `mmap/24-2` and the new `c33_mmap_errors/1-1` boundary regression
both fail with the previous kernel and pass with `ltp-kernel-fixed-range.app`,
using identical binaries and `ltp-rootfs-mlock.img`. The local regression checks
rounding and address overflow, zero length, alignment, missing mapping type,
valid-range rejection, ignored wrapping hints, and preservation of data and
ownership through rejected calls. All 22 existing local regressions pass.

The fresh 86-case memory batch has **77 PASS, three FAIL, three UNSUPPORTED,
two UNTESTED and one UNRESOLVED**, with no lost passes. Its report is
`ltp-memory-fixed-range-run/results.json`; focused before/after observations
are in `ltp-fixed-range-before` and `ltp-fixed-range-after`. Source, fixture and
report hashes are recorded in `ltp-fixed-range-followup.json`.
Combined original-interface observations advance to **1,198/1,236 (96.9%)**,
with **38 remaining non-PASS** and no timeouts. The combined report is
`ltp-fixed-range-verified/results.json`, not a new full sweep.
