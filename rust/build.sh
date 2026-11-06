#!/bin/sh
# Build a Rust Grifo application with rustc_codegen_gcc and the C33 libgccjit.
#
#   ./build.sh [crate-dir]      (default: hello)
#
# Needs the "c33" toolchain from build-rustc.sh, the backend built against
# it in work/rustc_codegen_gcc, and work/install/lib/libgccjit.so from
# build-gccjit.sh.  The application is linked as the C ones are
# (samo-lib/grifo/Mk/application-pre.mk): application.lds, libgrifo,
# mini-libc and the -mc33pe libgcc, and lands in the crate as NAME.app.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="${HERE}/.."
CRATE=$(cd "${1:-${HERE}/hello}" && pwd)
NAME=$(basename "${CRATE}")
TCBIN="${ROOT}/host-tools/toolchain-c33/work/install/bin"
CG="${HERE}/work/rustc_codegen_gcc"

export RUSTUP_HOME="${HERE}/work/rustup"
export CARGO_HOME="${HERE}/work/cargo"
export PATH="${CARGO_HOME}/bin:${TCBIN}:${PATH}"
export CG_GCCJIT_LIBRARY_PATH="${HERE}/work/install/lib/libgccjit.so"

LIBGCC=$(c33-epson-elf-gcc -mc33pe -print-libgcc-file-name)
GRIFO="${ROOT}/samo-lib/grifo"

# libgccjit runs the C33 cc1 with these after its own -fPIC (which the C33
# backend ignores; -fno-pic keeps the generic code static too).
RUSTFLAGS="-Zcodegen-backend=${CG}/target/release/librustc_codegen_gcc.dylib"
RUSTFLAGS="${RUSTFLAGS} -Cllvm-args=-mc33pe -Cllvm-args=-mno-long-calls"
RUSTFLAGS="${RUSTFLAGS} -Cllvm-args=-fno-pic"
for a in -static --strip-all -N --no-warn-rwx-segments \
	 -T "${GRIFO}/lds/application.lds" -Map "${CRATE}/${NAME}.map" \
	 "${GRIFO}/lib/libgrifo.a" "${ROOT}/samo-lib/mini-libc/lib/libc.a" \
	 "${LIBGCC}"; do
	RUSTFLAGS="${RUSTFLAGS} -Clink-arg=${a}"
done
export RUSTFLAGS

cd "${CRATE}"
cargo +c33 build --release
cp "target/c33-epson-none/release/${NAME}" "${NAME}.app"
c33-epson-elf-objdump -D "${NAME}.app" > "${NAME}.dump"
echo "==> ${CRATE}/${NAME}.app"
