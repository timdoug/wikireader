#!/bin/sh
# Build a Rust program for C33 Linux (std, musl, FDPIC) with
# rustc_codegen_gcc and the c33-linux-musl libgccjit.
#
#   ./build-linux.sh [crate-dir]      (default: stdhello)
#
# Needs the "c33" toolchain (build-rustc.sh), the backend built against it,
# C33_TARGET=c33-linux-musl ./build-gccjit.sh, and the toolchain's
# c33-linux-musl GCC, which links.  The program lands in the crate as
# NAME.bin.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="${HERE}/.."
CRATE=$(cd "${1:-${HERE}/stdhello}" && pwd)
NAME=$(basename "${CRATE}")
TCBIN="${ROOT}/host-tools/toolchain-c33/work/install/bin"
CG="${HERE}/work/rustc_codegen_gcc"

export RUSTUP_HOME="${HERE}/work/rustup"
export CARGO_HOME="${HERE}/work/cargo"
export PATH="${CARGO_HOME}/bin:${TCBIN}:${PATH}"
export CG_GCCJIT_LIBRARY_PATH="${HERE}/work/install-c33-linux-musl/lib/libgccjit.so"

# The Linux driver's own defaults (DRIVER_SELF_SPECS in c33/linux.h):
# libgccjit compiles in-process with only the options it is given, and runs
# the driver only to assemble.
RUSTFLAGS="-Zcodegen-backend=${CG}/target/release/librustc_codegen_gcc.dylib"
for o in -mcore=c33pe -mfdpic -msep-data; do
	RUSTFLAGS="${RUSTFLAGS} -Cllvm-args=${o}"
done
export RUSTFLAGS

cd "${CRATE}"
cargo +c33 build --release
cp "target/c33-unknown-linux-musl/release/${NAME}" "${NAME}.bin"
c33-linux-musl-readelf -h -l "${NAME}.bin" | grep -E 'Type|Flags|LOAD|GNU_STACK'
echo "==> ${CRATE}/${NAME}.bin"
