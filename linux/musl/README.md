# musl for C33 Linux

The system's C library is musl, at a commit after 1.2.6 for its fixes
since that release (`../revisions`), ported to the C33
under no-MMU Linux, as FDPIC ELF with shared libraries
(`host-tools/toolchain-c33/gcc/ABI.md`), and built by `c33-linux-musl-`.
`make -C linux libc` builds it, the final C/C++ compiler against it, and
its regression programs (`tests/`): threads, native TLS and `dlopen`,
signals, IPC, atomics, C++ exceptions through shared libraries,
relocation, `$ORIGIN`, time64, malloc. `app-test.py` runs them from the
card through Grifo. One musl difference they allow for: thread
cancellation runs cleanup handlers but not C++ destructors.

musl replaced uClibc-ng, the system's C library before it. On the same
kernel and card, both from Buildroot 2026.08.1 with the same packages,
`../compare-images.py`, which boots images through Grifo and reads the
guest's own clock, measured in wremu:

| | uClibc-ng | musl |
|---|---|---|
| Boot to the console prompt | 4.16 s | 4.19 s |
| `/bin/true` from the shell | 64 ms | 58 ms |
| `startx` to an xterm's first command | 24.1 s | 22.0 s |
| Available at the prompt | 22.79 MB | 23.12 MB |
| Available with X up | 12.78 MB | 13.36 MB |

musl's libc is its ld.so too: one library to open and map at every exec,
and the kernel's share of an exec is smaller for it. The musl system runs
on the device (2026-11-03).

## The port

`overlay/arch/c33` holds the architecture: the syscall table (generated
from the kernel's `unistd_32.h`, which has 64-bit time only, as
riscv32's), the asm-generic layouts, `int 0` system calls, atomics the
compiler inlines with interrupts masked, the thread pointer read from the
kernel's word in A0 RAM, the kernel's ptrace requests, and the start
code. `overlay/src`, `overlay/crt` and `overlay/ldso` hold the assembly:
`setjmp`, `sigsetjmp`, `clone`, `vfork`, the cancellation point, `dlsym`,
`crti`/`crtn`, the lazy binding resolver, and `__unmapself`, which runs
on a stack of its own because a trap pushes its frame on the user stack.

The start code has no PC-relative addressing: `c33_fdpic_fixup` takes the
link-time bounds of `.rofixup` and translates them through the load map,
with a loop of its own for a program of two segments, and returns `%r15`.
ld.so keeps the kernel's two load maps below the stack vector, where
musl's SuperH FDPIC code expects them.

`patches/` changes musl's shared code, in number order:

- `arch/`: the target in `configure`.
- `fixes/`: an FDPIC library's program headers for `dl_iterate_phdr`;
  `dl_iterate_phdr` running one callback at a time, as glibc does, which
  libgcc's unwinder cache needs when threads throw at once; `posix_madvise` checking its
  range for `POSIX_MADV_DONTNEED` too; `mmap` keeping the kernel's
  `EPERM` for `MAP_LOCKED`; `fork` failing with `ENOSYS`, since the
  generic `clone(SIGCHLD, 0)` would share the parent's memory and stack;
  `shm_unlink` reporting a sticky directory's `EPERM` as POSIX's
  `EACCES`, as glibc does; and `aio_cancel` of sequenced writes (sockets,
  pipes, `O_APPEND`), which deadlocked on the queue lock in two ways.
- `features/`: an architecture's default thread stack and guard sizes,
  64 KB and none here.
- `port/`: what the C33 linker writes and libgcc expects: relative
  relocations with the link-time address in the addend, constructor
  tables of entry points, and load maps in `dl_phdr_info` and for a
  static program's TLS.
- `perf/`: ld.so's work at every exec, numbered after everything else so
  that each applies to a tree that builds without it: recently resolved symbols
  remembered, function descriptors made on demand rather than for every
  function of every module (16 KB a process for libc alone), the counted
  relative relocations in a pass of their own, and lazy binding of `.plt`
  slots through the linker's stubs (`LD_BIND_NOW` binds at load); and,
  after them, mallocng keeping the only group of a size class when its last
  slot is freed, rather than building a group again at the next
  allocation (`malloc(41)` and `free` 7,900 -> 3,100 cycles).

Each was timed alone and left out of the set, as spawn/exec/wait of
`/bin/true` (BusyBox) and of `xeyes` without a display (twelve libraries),
in wremu (`runtime-bench.py --argument "--single --spawn /usr/bin/xeyes"`):

| | `/bin/true` | `xeyes` |
| --- | --- | --- |
| none of them | 106 ms | 1230 ms |
| all four | 54 ms | 650 ms |
| all but the symbol cache | +31% | +37% |
| all but descriptors on demand | +30% | +8% |
| all but the relative pass | +18% | +11% |
| all but lazy binding | +18% | +20% |
| all four, `LD_BIND_NOW=1` | 65 ms | 802 ms |

Two more were measured and dropped: GNU hash buckets by a reciprocal
instead of a divide, and a two-segment module's addresses in one
comparison.  Neither showed a gain, alone or in the set; the set without
them is as fast.

The allocator is musl's default, `mallocng`. Without an MMU it has no
`brk` and maps its memory, and it gives back what a process frees, which
`oldmalloc` never does here: freeing everything after a 4 MB peak of small
objects leaves nothing held, against 5.4 MB. It costs a little, measured
against `oldmalloc` in wremu: an exec about 1.5 ms slower, 100 KB less
available at the prompt and 250 KB less with X up, churn of small objects
9% and of 1-16 KB ones 55% slower, and `realloc` growing a block past
128 KB four times slower, since `mremap` cannot grow a mapping without an
MMU; many small blocks are allocated twice as fast. `bench/malloc-bench.c`
measures these: build it with `c33-linux-musl-gcc -O2`, put it on the card
with `ask-app.py --file MBENCH.BIN=...`, and run it from `/tmp`.

Thread stacks have no guard area, and a contended
spinlock yields, since its owner cannot run on this one core until it
does.

In Buildroot, BusyBox's vi searches for plain strings: its regex search
uses GNU's `re_search`. Packages key their no-MMU paths on `__uClinux__`,
which the compiler defines, not on the C library: musl declares a
`fork()`, which fails with `ENOSYS`.
