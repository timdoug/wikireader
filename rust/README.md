# Rust on the WikiReader

Rust applications for Grifo, compiled by rustc with
[rustc_codegen_gcc](https://github.com/rust-lang/rustc_codegen_gcc) driving
the C33 GCC 16.2 backend through libgccjit.  `hello/` is a `no_std` Grifo
application: it prints on the serial port and the panel, draws a Sierpinski
triangle and returns to the launcher at the next tap.  `abitest/` checks
the calling convention against the C compiler: 107 calls in both
directions with structs of every shape around GCC's register/stack rule,
narrow and 64-bit scalars, floats and variadic arguments.  C sources in a
crate's `c/` are compiled with the C applications' flags and linked in.

## Pieces

- `gcc/patches/jit/`: libgccjit for the C33, applied after the port's own
  patches (`host-tools/toolchain-c33/gcc/patches`) in a tree of its own.
  0101 is GCC trunk's jit work since 16, 0102 the rust-lang GCC fork's
  changes that rustc_codegen_gcc needs, 0103 the build against macOS's
  libc++ and the Darwin export list, 0104 one EH personality however many
  declarations name it (the C33's gas has no .cfi directives, so a unit
  has a single personality).
- `patches/rust-0001`: `Arch::C33` in rustc_target, with the C calling
  convention of the C33 GCC port (see its `ABI.md`): records of 1, 2, 4 or
  8 bytes, aligned to their mode, in registers (one holding a lone double
  as a double); others returned through a hidden pointer and passed by
  value on the stack.  `rust-0003`: the built-in target
  `c33-unknown-linux-musl` (below).
- `patches/rustc_codegen_gcc-0001`: `-C llvm-args` reach the GCC driver as
  well as cc1, so `-mc33pe` reaches the assembler and the objects link with
  the C applications', and a target's own llvm-args are passed first.
  0002: a variadic call's anonymous arguments are passed as anonymous (on
  the C33 they are all on the stack).  0003: `target_has_reliable_f128_math`
  on glibc targets only, since musl has no `expf128` and the rest of the
  `*f128` functions GCC lowers `f128` math to.  0004: no
  `-Wstringop-overflow`: a dangling `Weak` is the address `usize::MAX`, and
  where `Weak::clone` is not inlined GCC warned of the atomic add to its
  count, at an address near zero, on a path that never runs.
- `c33-epson-none.json`: the target.  `build.sh` links as
  `samo-lib/grifo/Mk/application-pre.mk` does: `application.lds`, libgrifo,
  mini-libc and the `-mc33pe` libgcc.

A codegen backend is a rustc_private crate and has to be built against the
compiler that loads it, so rustc is built here: stage 2, as a nightly is,
with stage 1's rlibs as its rustc-dev, linked into rustup as `c33`.
Everything goes to `work/` (git-ignored, about 8 GB); nothing to `~/.rustup`
or `~/.cargo`.

## Building

    ./fetch.sh          # rustc_codegen_gcc, rustup + its nightly, rustc's source
    ./build-gccjit.sh   # work/install/lib/libgccjit.so (the toolchain's binutils first)
    JOBS=2 ./build-rustc.sh    # stage-2 rustc with Arch::C33, ~25 min at 4 jobs
    (cd work/rustc_codegen_gcc && RUSTUP_HOME=$PWD/../rustup CARGO_HOME=$PWD/../cargo \
        ../cargo/bin/cargo +c33 build --release)
    ./build.sh hello    # hello/hello.app
    ./build.sh abitest  # abitest/abitest.app
    ./test.py           # wremu: flash -> Grifo -> menu tap -> each app -> menu

Bootstrap has to run with the caller's environment: a different
`CARGO_HOME` changes every crate's fingerprint and rebuilds both stages.

## Linux: std

`stdhello/` is a program for C33 Linux with Rust's full standard library
(`c33-unknown-linux-musl`: musl, static FDPIC, no MMU): arguments,
the environment, files, threads over a channel and a clock.  It needs:

- `patches/libc-0001`: the C33 in the libc crate's musl module (riscv32's
  layout and syscall numbers, which C33 musl shares), fetched by
  `fetch.sh` at the version std's `Cargo.lock` names.
- `patches/rust-0002`: std's per-architecture lists, no stack-overflow
  guard page (no MMU), and libgcc's unwinder (`gcc_eh`), as the
  toolchain's C++ uses.
- The C33 Linux GCC on the Mac, built by the toolchain's own scripts
  against the VM's musl sysroot, and a libgccjit for it:

      D=../host-tools/toolchain-c33/work/install/c33-linux-musl; mkdir -p $D
      limactl shell wr-linux -- tar -C wr-linux/toolchain/install/c33-linux-musl \
          -cf - sysroot | tar -C $D -xf -
      (cd ../host-tools/toolchain-c33 && C33_TARGET=c33-linux-musl \
          C33_BINUTILS_CONFIGURE=--with-sysroot=$PWD/work/install/c33-linux-musl/sysroot \
          binutils/build.sh $PWD/work && C33_TARGET=c33-linux-musl gcc/rebuild.sh $PWD/work final)
      C33_TARGET=c33-linux-musl ./build-gccjit.sh
      ./build-std.sh      # the target's std, backend and libgccjit, in the toolchain
      ./build-linux.sh    # stdhello/stdhello.bin
      ./test-linux.py     # wremu: Grifo -> linux.app -> r

On the device the card carries `stdhello.bin`, `unwindtest.bin` (as
`unwind.bin`), `sdprobe.bin` and `drmhello.bin` at its top and
`card/bin/r` as `bin/r`: `r` at the Linux shell runs them all (drmhello
holds the panel for three seconds), checks them and leaves the output in
`rust.txt` on the card.

`c33-unknown-linux-musl` is built into rustc (`patches/rust-0003`), and
`build-std.sh` installs it in the toolchain: the standard
library, built by rustc_codegen_gcc, in the toolchain's
`lib/rustlib/c33-unknown-linux-musl/lib`, the backend in its
`codegen-backends` and the libgccjit for the target beside it.  So

    cargo +c33 build --target c33-unknown-linux-musl

needs neither `-Zbuild-std` nor `-Zcodegen-backend`: the target names
`gcc` as its codegen backend.  libgccjit compiles in-process with only the
options it is given, so the target carries the Linux driver's defaults
(`-mcore=c33pe -mfdpic -msep-data`) as llvm-args, which the backend passes
on.  It links `-lgcc` last: GCC calls libgcc helpers such as `__ucmpdi2`
that Rust's compiler_builtins does not have.  `unwindtest/` and
`i128probe/` use the toolchain's std; `stdhello/`, `sdprobe/` and
`drmhello/` build their own with `-Zbuild-std`, leaving out the backtrace
symbolizer and panic unwinding that the toolchain's std includes (the
symbolizer was 200 kB of stdhello's 450).

`unwindtest/` builds with `panic = "unwind"`, the target's default:
catch_unwind, destructors run on the way out, a panicking thread's join
and a second panic, through libgcc's DWARF unwinder as C++ exceptions are.
A program that sets `panic = "abort"` (stdhello) leaves the unwinder and
its tables out.

## Linux: graphics

`drmhello/` is the Rust graphics hello world on the panel: embedded-graphics
draws text and shapes into a DRM dumb buffer (the `drm` crate) in the
panel's own format, `D1` (a bit a pixel, 1 black, scanned where it is), and
runs on a VT of its own, as X and cage do, so the console steps aside:

    openvt -c 2 -s -w -- drmhello.bin [seconds]

It needs the C33 in `rustix` and both `linux-raw-sys` versions
(`crates.txt`, `patches/rustix-*` and `patches/linux-raw-sys-*`, patched in
by its `Cargo.toml`); riscv32's generated bindings serve, with rustc laying
their structs out for the C33.  `drm-fourcc` has no name for D1, so the
framebuffer is added through `drm-ffi`.

It draws in 92 ms.  Three things cost it 1.2 s first:
- `opt-level = "s"`: GCC's -Os does not inline even a four-instruction
  helper (`Point::add`), so every pixel was a chain of calls.  Use
  `opt-level = 2`; codegen units made no difference.
- embedded-graphics fills through `draw_iter` a pixel at a time unless the
  target overrides `fill_solid`; with whole bytes, `clear` is 3 ms, not 551.
- A stroked `Triangle` is outlined by intersecting its edges with every
  scanline, about 270 ms here; three `Line`s draw the same outline.

`test-linux.py` stops the emulator while drmhello holds the panel and checks
the picture in `screen.pgm`.  BusyBox's `openvt -s -w` exits 1 however its
program went, so scripts judge by the program's own output.

## core's and alloc's own tests

`libtests/` builds the standard library's test suites, library/coretests
and alloctests in the rust checkout, for C33 Linux, and `libtests/run.py`
runs them in wremu through Grifo, a boot a part:

    libtests/build.sh       # the parts split.py writes, built
    libtests/run.py         # about three hours, niced

What it takes on this machine:
- Each suite is more code than one segment can hold without an MMU (the
  kernel allocates at most 4 MB contiguous), so `split.py` builds each
  as three parts: tests/lib.rs with a third of its modules, plus any
  module the others use.
- The parts are over 2 MB of text, past a short call's reach: they build
  with `-mlong-calls` and link musl and libgcc dynamically
  (`-Ctarget-feature=-crt-static`), so the precompiled C is reached
  through the PLT.
- Each test runs on its own thread, and std's 2 MB default stack is real
  memory here: `RUST_MIN_STACK=262144`.
- Each part is a package of its own (`core-1` … `alloc-3`), so its test
  crate keeps the suite's name, which `any::dyn_type_name` checks.
- The sort tests take a short list of lengths (patches/rust-0002).

Results (2026-11-06): core 2,730 and alloc 1,497 tests, all passing once
a C33 GCC bug the suite found was fixed in the backend: a local aligned
beyond 4 bytes landed off its alignment while GCC folded the checks
(gcc.target/c33/overaligned-local-run.c).  With `_Float128` in the
compiler (2026-11-07) core's f128 tests build and run too: part 1 passes
1,579 tests, 30 of them f128; the other five parts were not rerun with it.
Built at opt-level "s" the suites compile without a warning.

## f128

Rust's `f128` is GCC's `_Float128`: IEEE quad precision in software,
libgcc's soft-fp routines (`__addtf3`, `__lttf2`, ...), on bare metal and
Linux alike.  A quad is passed by reference and returned in memory
(`host-tools/toolchain-c33/gcc/ABI.md`, "Quad precision").
compiler_builtins has the arithmetic too; the math functions (`exp`,
`ln`, `powf`, ...) are the C library's `*f128`, which musl does not have,
so `cfg(target_has_reliable_f128_math)` is off on C33 Linux (core's tests
of them are left out) and a program that calls them does not link.

## On the device

Convert the icon and copy both files to the card, and add a line to
`init.ini`:

    samo-lib/grifo/scripts/xpm2icon --icon=hello.ico hello/hello.xpm

    hello.ico : hello.app
