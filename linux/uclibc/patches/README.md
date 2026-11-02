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
- `features/`: C11 threads and `timespec`; the kernel's optional no-MMU
  `msync` and memory locking, behind the configuration options
  `UCLIBC_HAS_NOMMU_MSYNC` and `UCLIBC_HAS_NOMMU_MLOCK`; and the thread
  stack cache's size as an option, `PTHREADS_STACK_CACHE_SIZE`, which the
  C33 defconfig sets low.
- `perf/`: choices for this board: the loader's word `memset` and `-O2`
  build, and the heap's search finger.
