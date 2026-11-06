#!/bin/sh
# Build rustc with the C33 architecture (patches/rust-*) and install it as the
# rustup toolchain "c33" in work/rustup.
#
#   ./build-rustc.sh            (after fetch.sh)
#
# A codegen backend is a rustc_private crate, so it has to be compiled
# against the rlibs of the very compiler that loads it.  As for a nightly,
# that compiler is stage 2: built by stage 1, whose sysroot receives the
# rlibs (the rustc-dev component).  work/toolchain is stage 2's sysroot plus
# those rlibs; the LLVM backend comes from CI and serves the host.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
WORK="${HERE}/work"
RUST="${WORK}/rust"
HOST=$(rustc -vV 2>/dev/null | sed -n 's/^host: //p')
HOST=${HOST:-aarch64-apple-darwin}

# Bootstrap runs with the caller's environment untouched: a different
# CARGO_HOME changes every crate's fingerprint and rebuilds the compiler.
cd "${RUST}"
nice -n 19 ./x build --stage 2 library
export RUSTUP_HOME="${WORK}/rustup" CARGO_HOME="${WORK}/cargo"

STAGE1="${RUST}/build/host/stage1"
STAGE2="${RUST}/build/host/stage2"
TC="${WORK}/toolchain"
rm -rf "${TC}"
cp -R "${STAGE2}" "${TC}"
# The rustc-dev crates, the proc macros among them as dylibs; std's are the
# same files in both (stage 2's library is stage 1's, uplifted).
for f in "${STAGE1}/lib/rustlib/${HOST}/lib/"*; do
	[ -e "${TC}/lib/rustlib/${HOST}/lib/${f##*/}" ] ||
		cp "${f}" "${TC}/lib/rustlib/${HOST}/lib/"
done
# rust-src, for -Zbuild-std.
mkdir -p "${TC}/lib/rustlib/src"
rm -rf "${TC}/lib/rustlib/src/rust"
ln -s "${RUST}" "${TC}/lib/rustlib/src/rust"
# cargo, which bootstrap does not build here, from the pinned nightly.
NIGHTLY=$(sed -n 's/^channel = "\(.*\)"/\1/p' "${WORK}/rustc_codegen_gcc/rust-toolchain.toml")
cp "${RUSTUP_HOME}/toolchains/${NIGHTLY}-${HOST}/bin/cargo" "${TC}/bin/"

"${CARGO_HOME}/bin/rustup" toolchain link c33 "${TC}"
"${CARGO_HOME}/bin/rustc" +c33 -vV
