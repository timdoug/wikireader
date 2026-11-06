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
  libc++ and the Darwin export list.
- `patches/rust-0001`: `Arch::C33` in rustc_target, with the C calling
  convention of the C33 GCC port (see its `ABI.md`): records of 1, 2, 4 or
  8 bytes, aligned to their mode, in registers (one holding a lone double
  as a double); others returned through a hidden pointer and passed by
  value on the stack.
- `patches/rustc_codegen_gcc-0001`: `-C llvm-args` reach the GCC driver as
  well as cc1, so `-mc33pe` reaches the assembler and the objects link with
  the C applications'.  0002: a variadic call's anonymous arguments are
  passed as anonymous (on the C33 they are all on the stack).
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
    ./build-rustc.sh    # stage-2 rustc with Arch::C33, ~25 min at 4 jobs
    (cd work/rustc_codegen_gcc && RUSTUP_HOME=$PWD/../rustup CARGO_HOME=$PWD/../cargo \
        ../cargo/bin/cargo +c33 build --release)
    ./build.sh hello    # hello/hello.app
    ./build.sh abitest  # abitest/abitest.app
    ./test.py           # wremu: flash -> Grifo -> menu tap -> each app -> menu

Bootstrap has to run with the caller's environment: a different
`CARGO_HOME` changes every crate's fingerprint and rebuilds both stages.

## On the device

Convert the icon and copy both files to the card, and add a line to
`init.ini`:

    samo-lib/grifo/scripts/xpm2icon --icon=hello.ico hello/hello.xpm

    hello.ico : hello.app
