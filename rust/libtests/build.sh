#!/bin/sh
# Build core's and alloc's test suites for C33 Linux, in the parts split.py
# writes; run.py runs them.  The parts are dynamically linked and use long
# calls: each is over 2 MB of text, past a short call's reach, and the
# precompiled musl and libgcc are reached through the PLT.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
RUST="${HERE}/.."
export RUSTUP_HOME="${RUST}/work/rustup"
export CARGO_HOME="${RUST}/work/cargo"
export PATH="${CARGO_HOME}/bin:${RUST}/../host-tools/toolchain-c33/work/install/bin:${PATH}"
export RUSTFLAGS="-Cllvm-args=-mlong-calls -Ctarget-feature=-crt-static"

cd "${HERE}"
python3 split.py > /dev/null
cargo +c33 test --release --no-run
