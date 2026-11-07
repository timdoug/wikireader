#!/bin/sh
# Install the c33-unknown-linux-musl target in the "c33" toolchain: the
# standard library, built with rustc_codegen_gcc, in
# lib/rustlib/c33-unknown-linux-musl/lib, and beside the host's libraries the
# backend the target names (codegen-backends/librustc_codegen_gcc.dylib) and
# the libgccjit it loads for the target
# (codegen-backends/lib/c33-unknown-linux-musl/libgccjit.so).  Then
#
#     cargo +c33 build --target c33-unknown-linux-musl
#
# builds a program for C33 Linux with neither -Zbuild-std nor
# -Zcodegen-backend.
#
#   ./build-std.sh      (after build-rustc.sh, which replaces work/toolchain,
#                        the backend's build and
#                        C33_TARGET=c33-linux-musl ./build-gccjit.sh)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
TARGET=c33-unknown-linux-musl
TC="${HERE}/work/toolchain"
HOST=$("${TC}/bin/rustc" -vV | sed -n 's/^host: //p')
BACKENDS="${TC}/lib/rustlib/${HOST}/codegen-backends"

export RUSTUP_HOME="${HERE}/work/rustup"
export CARGO_HOME="${HERE}/work/cargo"
export PATH="${CARGO_HOME}/bin:${HERE}/../host-tools/toolchain-c33/work/install/bin:${PATH}"

mkdir -p "${BACKENDS}/lib/${TARGET}"
cp "${HERE}/work/rustc_codegen_gcc/target/release/librustc_codegen_gcc.dylib" "${BACKENDS}/"
cp "${HERE}/work/install-c33-linux-musl/lib/libgccjit.so" "${BACKENDS}/lib/${TARGET}/"

# A program that uses std, so that cargo builds all of it: its dependencies
# are the target's library.  The sysroot's crates are unstable unless marked
# (-Zforce-unstable-if-unmarked), so a program's own `libc` is never
# confused with std's.
CRATE="${HERE}/work/std-${TARGET}"
mkdir -p "${CRATE}/src"
cat > "${CRATE}/Cargo.toml" <<EOF
[package]
name = "std-${TARGET}"
version = "0.0.0"
edition = "2024"
EOF
echo 'fn main() { println!("std"); }' > "${CRATE}/src/main.rs"
cd "${CRATE}"
RUSTFLAGS="-Zforce-unstable-if-unmarked" cargo +c33 build --release --target "${TARGET}" \
	-Zbuild-std=std,panic_abort,panic_unwind,proc_macro,test

# Each rlib holds a stub; its metadata is the .rmeta beside it.
LIB="${TC}/lib/rustlib/${TARGET}/lib"
rm -rf "${LIB}"
mkdir -p "${LIB}"
find "target/${TARGET}/release" \( -name "*.rlib" -o -name "*.rmeta" \) -exec cp {} "${LIB}/" \;
ls "${LIB}"
