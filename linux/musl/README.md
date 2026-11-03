# musl for C33 Linux

musl 1.2.6 (`../revisions`) ported to the C33 under no-MMU Linux, as FDPIC
ELF with shared libraries: the same ABI as the uClibc-ng userspace
(`host-tools/toolchain-c33/gcc/ABI.md`), built by its own compiler,
`c33-linux-musl-`, beside `c33-linux-uclibc-`. The default root image is
still uClibc-ng's; `WR_LIBC=musl` builds the same system on musl.

```sh
make -C linux musl-toolchain   # binutils, musl headers, C compiler
make -C linux musl-libc        # musl, the final C/C++ compiler, the tests
make -C linux musl-test        # run them in wremu, through Grifo
WR_LIBC=musl ./linux/buildroot/build.sh   # in the VM: artifacts/linux-musl.img
```

`musl-test` runs the C library's regression programs from `../uclibc`
(threads, native TLS and `dlopen`, signals, IPC, atomics, C++ exceptions
through shared libraries, relocation, `$ORIGIN`, time64, malloc) built
against musl. They ride on the card's FAT partition with musl's `ld.so`
at `/mnt/sd/ldmusl.so`; programs on the root image use
`/lib/ld-musl-c33.so.1`. The one musl difference they allow for: thread
cancellation runs cleanup handlers but not C++ destructors.

`../compare-images.py` boots root images on one kernel, through Grifo,
and reads the guest's own clock. In wremu, uClibc-ng's image against
musl's, both from Buildroot 2026.08.1 with the same packages:

| | uClibc-ng | musl |
|---|---|---|
| Boot to the console prompt | 4.15 s | 4.17 s |
| `/bin/true` from the shell | 64 ms | 58 ms |
| `startx` to an xterm's first command | 24.1 s | 22.2 s |
| Available at the prompt | 22.79 MB | 23.12 MB |
| Available with X up | 12.78 MB | 13.37 MB |

musl's libc is its ld.so too: one library to open and map at every exec,
and the kernel's share of an exec is smaller for it.

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
as uClibc-ng's `crtreloc.o` does, with a loop of its own for a program of
two segments, and returns `%r15`. ld.so keeps the kernel's two load maps
below the stack vector, where musl's SuperH FDPIC code expects them.

`patches/` changes musl's shared code, in number order:

- `arch/`: the target in `configure`.
- `fixes/`: an FDPIC library's program headers for `dl_iterate_phdr`;
  `clock_nanosleep` on a kernel without `nanosleep`; and
  `dl_iterate_phdr` running one callback at a time, as glibc does, which
  libgcc's unwinder cache needs when threads throw at once.
- `features/`: an architecture's default thread guard size, zero here.
- `port/`: what the C33 linker writes and libgcc expects: relative
  relocations with the link-time address in the addend, constructor
  tables of entry points, and load maps in `dl_phdr_info` and for a
  static program's TLS.
- `perf/`: ld.so's work at every exec: recently resolved symbols
  remembered, GNU hash buckets without a divide, function descriptors made
  on demand rather than for every function of every module (16 KB a
  process for libc alone), the counted relative relocations in a pass of
  their own, a two-segment module's addresses in one comparison, and lazy
  binding of `.plt` slots through the linker's stubs, as uClibc-ng does
  (`LD_BIND_NOW` binds at load).

The allocator is `oldmalloc`: `mallocng` takes memory from `brk`, which
no-MMU Linux does not have, and returns it in pieces, which a no-MMU
mapping cannot be. Thread stacks have no guard area.

For Buildroot, `../buildroot/build.sh` builds the musl image from the same
defconfig, with the toolchain lines changed and one BusyBox option off
(vi's regex search, which uses GNU's `re_search`). Packages keyed their
no-MMU paths on uClibc-ng; they key on `__uClinux__`, which the compiler
defines for both, since musl declares a `fork()` that fails.
