# musl for C33 Linux

musl 1.2.6 (`../revisions`) ported to the C33 under no-MMU Linux, as FDPIC
ELF with shared libraries: the same ABI as the uClibc-ng userspace
(`host-tools/toolchain-c33/gcc/ABI.md`), built by its own compiler,
`c33-linux-musl-`, beside `c33-linux-uclibc-`. The root image is still
uClibc-ng's.

```sh
make -C linux musl-toolchain   # binutils, musl headers, C compiler
make -C linux musl-libc        # musl, the final C/C++ compiler, the tests
make -C linux musl-test        # run them in wremu, through Grifo
```

`musl-test` runs the C library's regression programs from `../uclibc`
(threads, native TLS and `dlopen`, signals, IPC, atomics, C++ exceptions
through shared libraries, relocation, `$ORIGIN`, time64, malloc) built
against musl. They ride on the card's FAT partition with musl's `ld.so`
at `/mnt/sd/ldmusl.so`; programs on the root image use
`/lib/ld-musl-c33.so.1`. The one musl difference they allow for: thread
cancellation runs cleanup handlers but not C++ destructors.

## The port

`overlay/arch/c33` holds the architecture: the syscall table (generated
from the kernel's `unistd_32.h`, which has 64-bit time only, as
riscv32's), the asm-generic layouts, `int 0` system calls, atomics the
compiler inlines with interrupts masked, the thread pointer read from the
kernel's word in A0 RAM, and the start code. `overlay/src` and
`overlay/crt` hold the assembly: `setjmp`, `sigsetjmp`, `clone`,
`vfork`, the cancellation point, `dlsym`, `crti`/`crtn`, and
`__unmapself`, which runs on a stack of its own because a trap pushes its
frame on the user stack.

The start code has no PC-relative addressing: `c33_fdpic_fixup` takes the
link-time bounds of `.rofixup` and translates them through the load map,
as uClibc-ng's `crtreloc.o` does, and returns `%r15`. ld.so keeps the
kernel's two load maps below the stack vector, where musl's SuperH FDPIC
code expects them.

`patches/` changes musl's shared code, in number order:

- `arch/`: the target in `configure`.
- `fixes/`: an FDPIC library's program headers for `dl_iterate_phdr`, and
  `clock_nanosleep` on a kernel without `nanosleep`.
- `features/`: an architecture's default thread guard size, zero here.
- `port/`: what the C33 linker writes and libgcc expects: relative
  relocations with the link-time address in the addend, constructor
  tables of entry points, and load maps in `dl_phdr_info` and for a
  static program's TLS.

The allocator is `oldmalloc`: `mallocng` takes memory from `brk`, which
no-MMU Linux does not have, and returns it in pieces, which a no-MMU
mapping cannot be. Thread stacks have no guard area.
