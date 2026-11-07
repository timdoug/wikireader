#!/bin/sh
# Build a Rust program for C33 Linux (std, musl, FDPIC) with
# rustc_codegen_gcc and the c33-linux-musl libgccjit.
#
#   ./build-linux.sh [crate-dir]      (default: stdhello)
#
# Needs the "c33" toolchain with the target installed (build-std.sh, which
# also puts the backend and libgccjit in it) and the toolchain's
# c33-linux-musl GCC, which links.  The program lands in the crate as
# NAME.bin.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="${HERE}/.."
CRATE=$(cd "${1:-${HERE}/stdhello}" && pwd)
NAME=$(basename "${CRATE}")
TCBIN="${ROOT}/host-tools/toolchain-c33/work/install/bin"

export RUSTUP_HOME="${HERE}/work/rustup"
export CARGO_HOME="${HERE}/work/cargo"
export PATH="${CARGO_HOME}/bin:${TCBIN}:${PATH}"

cd "${CRATE}"
cargo +c33 build --release
cp "target/c33-unknown-linux-musl/release/${NAME}" "${NAME}.bin"
c33-linux-musl-readelf -h -l "${NAME}.bin" | grep -E 'Type|Flags|LOAD|GNU_STACK'
echo "==> ${CRATE}/${NAME}.bin"
