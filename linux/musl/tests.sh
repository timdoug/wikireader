#!/bin/sh
# Build the C library's regression programs (musl/tests) with the final
# compiler, once musl and the shared runtimes are installed (make -C linux
# libc runs musl/build.sh, toolchain.sh final, then this).  app-test.py runs
# those in linux/artifacts from the SD card.
set -eu

if [ "$(uname -s)" != Linux ]; then
	echo "musl/tests.sh must run inside the wr-linux VM" >&2
	exit 1
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
guest_root=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
tool_dir=${C33_TOOLCHAIN_WORK:-$guest_root/toolchain}
build_dir=${WR_MUSL_BUILD:-$guest_root/musl-build}/tests
target=c33-linux-musl
prefix=$tool_dir/install
cross=$prefix/bin/$target-
src=$root/linux/musl/tests
out=$root/linux/artifacts

rm -rf "$build_dir"
mkdir -p "$build_dir" "$out"

# The smoke test links statically, so that its undefined symbols show what
# the static library fails to supply; the other tests are dynamic, as every
# program on the device is.
"${cross}gcc" -Os -static -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -Wl,--gc-sections \
	"$src/smoke.c" -o "$build_dir/smoke-static"
undefined=$("${cross}nm" -u "$build_dir/smoke-static" | grep -v '^ *w ' || true)
if [ -n "$undefined" ]; then
	echo "static musl smoke test has undefined symbols:" >&2
	echo "$undefined" >&2
	exit 1
fi
"${cross}gcc" -Os "$src/smoke.c" -o "$build_dir/smoke"

# Pthread and native TLS regressions, run from the SD card by app-test.py.
"${cross}gcc" -O2 -Wall -Werror -pthread "$src/pthread-test.c" -o "$out/pthread-test"
"${cross}gcc" -Os -Wall -Werror -pthread "$src/signal-test.c" "$src/signal-entry.S" \
	-o "$out/signal-test"
"${cross}gcc" -O2 -Wall -Werror "$src/relocation-test.c" -o "$out/relocation-test"
"${cross}gcc" -Os -Wall -Werror -fPIC -shared "$src/origin-library.c" -o "$out/origin.so"
"${cross}gcc" -O2 -Wall -Werror "$src/origin-test.c" \
	-Wl,--disable-new-dtags,-rpath,'$ORIGIN' -o "$out/origin-rpath"
"${cross}gcc" -O2 -Wall -Werror "$src/origin-test.c" \
	-Wl,--enable-new-dtags,-rpath,'$ORIGIN' -o "$out/origin-runpath"
"${cross}gcc" -O2 -Wall -Werror -Wl,-z,stack-size=32768 \
	"$src/exec-stack-test.c" -o "$out/exec-stack-test"
"${cross}gcc" -O2 -Wall -Werror "$src/devmem-test.c" -o "$out/devmem-test"
"${cross}gcc" -O2 -Wall -Werror "$src/glob-test.c" -o "$out/glob-test"
"${cross}gcc" -O2 -Wall -Werror "$src/time64-test.c" -o "$out/time64-test"
"${cross}gcc" -O2 -Wall -Werror "$src/malloc-test.c" -o "$out/malloc-test"
"${cross}gcc" -O2 -Wall -Werror -pthread -static "$src/pthread-test.c" \
	-o "$out/pthread-test-static"
"${cross}gcc" -Os -Wall -Werror -pthread "$src/runtime-bench.c" -o "$out/runtime-bench"
"${cross}gcc" -Os -Wall -Werror -pthread "$src/tls-suspend-test.c" -o "$out/tls-suspend-test"
"${cross}gcc" -Os -Wall -Werror -fPIC -shared -funwind-tables \
	"$src/unwind-library.c" -o "$out/unwind-library.so"
python3 "$src/legacy-eh-header.py" "$out/unwind-library.so" "$out/unwind-legacy-library.so"
"${cross}gcc" -nostdlib -static "$src/tls-exec-test.S" -o "$out/tls-exec-test"
"${cross}gcc" -Os -Wall -Werror -fPIC -shared -Wl,-soname,tlslib.so \
	"$src/tls-library.c" -o "$out/tls-library.so"
"${cross}gcc" -Os -Wall -Werror -pthread "$src/nptl-test.c" "$out/tls-library.so" \
	-o "$out/nptl-test"
"${cross}gcc" -Os -Wall -Werror -fPIC -shared -Wl,-soname,tlslate.so \
	"$src/tls-late-library.c" -o "$out/tls-late-library.so"

# Independently executed processes share buffers and kernel IPC objects.
"${cross}gcc" -std=gnu11 -Os -Wall -Werror -pthread "$src/ipc-test.c" -o "$out/ipc-test"

# Atomics wider than the inline ones come from libatomic.
"${cross}gcc" -std=gnu11 -Os -Wall -Werror -pthread "$src/atomic-test.c" -latomic \
	-o "$out/atomic-test"
"${cross}g++" -Os -Wall -Werror -pthread "$src/cxx-test.cc" -latomic -o "$out/cxx-test"
python3 "$root/host-tools/toolchain-c33/gcc/tools/check-runtime.py" "$prefix" "$out/cxx-test"

"${cross}size" "$build_dir/smoke" "$build_dir/smoke-static"
