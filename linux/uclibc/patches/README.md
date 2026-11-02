# uClibc-ng patches

Applied by `../../fetch.sh` in number order across the directories. The
directories say what each is:

- `arch/`: the C33 configuration and ABI: the architecture's build
  settings, no-MMU NPTL, the TLS model, IPC and `stat` layouts, `ptrace`
  thread-area requests.
- `fixes/`: corrections to shared code that stand on their own: time64
  conversions and write-back, `ld.so`'s FDPIC hashing, start address and
  header checks, no-MMU NPTL, `epoll`, `select`, `poll` through `ppoll`,
  RUNPATH `$ORIGIN`, `memalign`, the locale build race and others.
- `features/`: C11 threads and `timespec`, and the wrappers for the
  kernel's optional no-MMU `msync` and memory locking, which are gated on
  the CPU today and should be gated on a capability.
- `perf/`: choices for this board: the pthread stack-cache budget, the
  loader's word `memset` and `-O2` build, and the heap's search finger.
