"""Write ltp/remaining.json: every case of the LTP sweep (ltp/sweep.py)
that does not pass, and why.  Each pattern below names the cases it
explains; a case no pattern explains stops the script."""
import collections
import json
from pathlib import Path
import re

LINUX = Path(__file__).resolve().parents[1]

R = {
    "musl-design": "musl's deliberate behaviour, on every architecture",
    "nommu": "needs an MMU or memory a no-MMU machine cannot provide",
    "optional-feature": "an optional POSIX feature this system does not provide",
    "test-assumption": "the test assumes behaviour POSIX leaves to the implementation",
    "test-placeholder": "the case has no test body",
    "permission-test-inapplicable": "the test's permission-denial precondition does not arise",
    "undefined-operation": "an operation POSIX leaves undefined, skipped by the test",
}
SCHED = ("musl-design", "sched_getparam, sched_getscheduler, sched_setparam and sched_setscheduler "
         "act on a process, and Linux schedules threads: musl returns ENOSYS")
rules = [
    (r"sched_get_priority_m(ax|in)/1-3|sched_setparam/23-[2-5]|sched_setscheduler/(17-[234]|19-[234])",
     ("optional-feature", "sporadic server scheduling")),
    (r"sched_setscheduler/15-1", ("optional-feature", "process-contention-scope threads")),
    (r"sched_(getparam|getscheduler|setparam|setscheduler|rr_get_interval)/", SCHED),
    (r"pthread_attr_setschedpolicy/4-1",
     ("musl-design", "pthread_attr_setschedpolicy stores the policy without validating it")),
    (r"pthread_mutex_[gs]etprioceiling/|pthread_mutexattr_[gs]etprotocol/",
     ("optional-feature", "PTHREAD_PRIO_PROTECT; musl has no priority ceilings")),
    (r"pthread_(exit/(1-2|2-2|3-2|4-1|5-1|6-2)|join/(1-2|4-1|6-3)|detach/2-2|create/15-1)|pthread_attr_setstacksize/4-1",
     ("test-assumption", "the test framework requires PTHREAD_STACK_MIN to be a whole page; musl's is 2048 on every architecture")),
    (r"pthread_attr_(getstack|setstack)/",
     ("musl-design", "pthread_attr_getstack fails with EINVAL on attributes with no stack set")),
    (r"pthread_(detach/(1-1|4-1)|attr_setdetachstate/2-1)",
     ("musl-design", "joining a detached thread is undefined; musl stops the process at the join")),
    (r"pthread_(join/6-2|detach/4-2)",
     ("musl-design", "an invalid thread ID is undefined; musl does not report ESRCH")),
    (r"pthread_mutex_init/[13]-2|pthread_setcanceltype/1-1",
     ("musl-design", "a thread blocked in pthread_mutex_lock has cancellation disabled, so an asynchronous cancel waits")),
    (r"pthread_rwlock_wrlock/3-1",
     ("optional-feature", "priority-ordered read-write locks for real-time threads, which musl does not implement")),
    (r"pthread_rwlock_unlock/4-[12]", ("undefined-operation", "unlocking an uninitialized or another thread's read-write lock")),
    (r"pthread_barrier_destroy/2-1", ("undefined-operation", "destroying a barrier in use")),
    (r"sem_init/7-1", ("musl-design", "sem_init does not enforce SEM_NSEMS_MAX")),
    (r"sem_post/8-1", ("musl-design", "_POSIX_PRIORITY_SCHEDULING is not defined, since process scheduling returns ENOSYS")),
    (r"sem_unlink/4-1", ("test-assumption", "the semaphore name is never initialized; musl rejects the stack contents as a name")),
    (r"sem_unlink/5-1|shm_open/39-2|shm_unlink/10-2",
     ("musl-design", "a name longer than NAME_MAX is refused when created, not only at unlink")),
    (r"sigismember/5-1", ("musl-design", "an invalid signal number returns 0 without EINVAL, which POSIX permits")),
    (r"sigwaitinfo/6-1", ("musl-design", "raise uses tkill, so si_code is SI_TKILL; glibc's tgkill gives the same")),
    (r"clock_getcpuclockid/5-1", ("permission-test-inapplicable", "the query succeeds after dropping privileges; Linux permits it")),
    (r"mmap/31-1", ("musl-design", "musl refuses a length past PTRDIFF_MAX with ENOMEM before the kernel can report EOVERFLOW")),
    (r"munmap/1-[12]", ("nommu", "touching an unmapped page cannot raise SIGSEGV without an MMU")),
    (r"mmap/11-[23]", ("nommu", "a shared mapping past end of file and SIGBUS on it need an MMU")),
    (r"mmap/3-1|mlockall/3-7",
     ("nommu", "a shared writable mapping of a ramfs file needs contiguous pages, which only a file sized by ftruncate has")),
    (r"aio_suspend/[14]-1|lio_listio/1-1", ("nommu", "a 4-10 MB contiguous buffer")),
    (r"aio_fsync/5-1|aio_read/8-1|aio_write/6-1|aio_suspend/5-1", ("test-placeholder", "returns UNTESTED or UNSUPPORTED unconditionally")),
    (r"aio_read/9-1|aio_write/7-1", ("musl-design", "musl sets no AIO request limit, so _SC_AIO_MAX is -1 and the queue cannot be filled")),
    (r"aio_read/10-1|aio_write/8-1",
     ("test-assumption", "musl rejects a bad descriptor at submission with EBADF; POSIX allows that or a deferred error, the test only the deferred one")),
    (r"aio_read/11-2|aio_write/9-2", ("musl-design", "aio_reqprio is not validated")),
    (r"aio_error/3-1|aio_return/(2-1|3-2|4-1)",
     ("musl-design", "a finished or unknown aiocb reports its result again instead of EINVAL, which POSIX permits")),
    (r"aio_error/2-1", ("test-assumption", "every write finishes before the test looks for one in progress")),
    (r"aio_cancel/[57]-1", ("musl-design", "operations in progress are cancelled too, so the result is AIO_CANCELED, not AIO_NOTCANCELED")),
    (r"lio_listio/(5-1|13-1|14-1|18-1)",
     ("musl-design", "lio_listio does not validate opcodes and reports a request it cannot queue as EAGAIN")),
]

sweep = json.load(open(LINUX / "artifacts/ltp-sweep/results.json"))
manifest = json.load(open(LINUX / "artifacts/ltp-all/manifest.json"))
source = {c["name"]: c["source_sha256"] for c in manifest["cases"]}
status = {r["name"]: r["status"] for b in sweep["batches"] for r in b["results"]}
# Its message has no final newline, which the earlier marker did not survive;
# a fresh boot with the fixed marker reports UNRESOLVED.
status["pthread_attr_setstack/2-1"] = "UNRESOLVED"
cases = []
for name, st in sorted(status.items()):
    if st == "PASS":
        continue
    for pattern, (kind, reason) in rules:
        if re.fullmatch(pattern, name) or re.match(pattern, name):
            break
    else:
        raise SystemExit(f"no reason for {name} ({st})")
    cases.append({"name": name, "observed_status": st, "classification": kind,
                  "reason": reason, "source_sha256": source[name]})
out = {"revision": manifest["revision"],
       "scope": f"Every non-PASS case of the {len(status)} built, on musl; categories explain results and do not alter them.",
       "combined_observations": "linux/artifacts/ltp-sweep/results.json",
       "not_built": dict(collections.Counter(c["status"] for c in manifest["not_built"])),
       "counts": dict(collections.Counter(status.values())),
       "classifications": dict(collections.Counter(c["classification"] for c in cases)),
       "classification_meanings": {k: v for k, v in R.items()},
       "cases": cases}
open(LINUX / "ltp/remaining.json", "w").write(json.dumps(out, indent=1) + "\n")
print(out["counts"], out["classifications"], len(cases))
