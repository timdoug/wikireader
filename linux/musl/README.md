# musl for C33 Linux

The system's C library is musl 1.2.6 (`../revisions`), ported to the C33
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
  `clock_nanosleep` on a kernel without `nanosleep`; `dl_iterate_phdr`
  running one callback at a time, as glibc does, which libgcc's unwinder
  cache needs when threads throw at once; `posix_madvise` checking its
  range for `POSIX_MADV_DONTNEED` too; and `mmap` keeping the kernel's
  `EPERM` for `MAP_LOCKED`.
- `features/`: an architecture's default thread stack and guard sizes,
  64 KB and none here.
- `port/`: what the C33 linker writes and libgcc expects: relative
  relocations with the link-time address in the addend, constructor
  tables of entry points, and load maps in `dl_phdr_info` and for a
  static program's TLS.
- `perf/`: ld.so's work at every exec: recently resolved symbols
  remembered, GNU hash buckets without a divide, function descriptors made
  on demand rather than for every function of every module (16 KB a
  process for libc alone), the counted relative relocations in a pass of
  their own, a two-segment module's addresses in one comparison, and lazy
  binding of `.plt` slots through the linker's stubs (`LD_BIND_NOW` binds
  at load).

The allocator is `oldmalloc`: `mallocng` takes memory from `brk`, which
no-MMU Linux does not have, and returns it in pieces, which a no-MMU
mapping cannot be. Thread stacks have no guard area, and a contended
spinlock yields, since its owner cannot run on this one core until it
does.

In Buildroot, BusyBox's vi searches for plain strings: its regex search
uses GNU's `re_search`. Packages key their no-MMU paths on `__uClinux__`,
which the compiler defines, not on the C library: musl declares a
`fork()`, which fails.
