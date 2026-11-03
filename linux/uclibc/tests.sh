#!/bin/sh
# Build the C library's regression programs with the final compiler, once
# uClibc-ng and the shared runtimes are installed (make -C linux libc runs
# uclibc/build.sh, toolchain.sh final, then this).  app-test.py runs those in
# linux/artifacts from the SD card.
set -eu

if [ "$(uname -s)" != Linux ]; then
	echo "uclibc/tests.sh must run inside the wr-linux VM" >&2
	exit 1
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
guest_root=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
tool_dir=${C33_TOOLCHAIN_WORK:-$guest_root/toolchain}
build_dir=${WR_UCLIBC_BUILD:-$guest_root/uclibc-build}
target=c33-linux-uclibc
prefix=$tool_dir/install
cross=$prefix/bin/$target-

# The smoke test links statically, so that its undefined symbols show what
# the static library fails to supply; the other tests are dynamic, as every
# program on the device is.
"${cross}gcc" -Os -static -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -Wl,--gc-sections \
	"$root/linux/uclibc/smoke.c" -o "$build_dir/uclibc-smoke-static"
# Weak references may stay undefined: crtbegin.o's to the unwinder and the
# transactional-memory runtime resolve to zero in a C program.
undefined=$("${cross}nm" -u "$build_dir/uclibc-smoke-static" | grep -v '^ *w ' || true)
if [ -n "$undefined" ]; then
	echo "static uClibc smoke test has undefined symbols:" >&2
	echo "$undefined" >&2
	exit 1
fi
"${cross}gcc" -Os "$root/linux/uclibc/smoke.c" -o "$build_dir/uclibc-smoke"
mkdir -p "$root/linux/artifacts"

# Pthread and native TLS regressions, run from the SD card by app-test.py.
"${cross}gcc" -O2 -Wall -Werror -pthread "$root/linux/uclibc/pthread-test.c" \
	-o "$build_dir/pthread-test"
cp "$build_dir/pthread-test" "$root/linux/artifacts/pthread-test"
"${cross}gcc" -Os -Wall -Werror -pthread "$root/linux/uclibc/signal-test.c" \
	"$root/linux/uclibc/signal-entry.S" \
	-o "$root/linux/artifacts/signal-test"
"${cross}gcc" -O2 -Wall -Werror "$root/linux/uclibc/relocation-test.c" \
	-o "$root/linux/artifacts/relocation-test"
"${cross}gcc" -Os -Wall -Werror -fPIC -shared \
	"$root/linux/uclibc/origin-library.c" -o "$root/linux/artifacts/origin.so"
"${cross}gcc" -O2 -Wall -Werror "$root/linux/uclibc/origin-test.c" \
	-Wl,--disable-new-dtags,-rpath,'$ORIGIN' -o "$root/linux/artifacts/origin-rpath"
"${cross}gcc" -O2 -Wall -Werror "$root/linux/uclibc/origin-test.c" \
	-Wl,--enable-new-dtags,-rpath,'$ORIGIN' -o "$root/linux/artifacts/origin-runpath"
"${cross}gcc" -O2 -Wall -Werror -Wl,-z,stack-size=32768 \
	"$root/linux/uclibc/exec-stack-test.c" -o "$root/linux/artifacts/exec-stack-test"
"${cross}gcc" -O2 -Wall -Werror "$root/linux/uclibc/devmem-test.c" \
	-o "$root/linux/artifacts/devmem-test"
"${cross}gcc" -O2 -Wall -Werror "$root/linux/uclibc/glob-test.c" \
	-o "$root/linux/artifacts/glob-test"
"${cross}gcc" -O2 -Wall -Werror "$root/linux/uclibc/time64-test.c" \
	-o "$root/linux/artifacts/time64-test"
"${cross}gcc" -O2 -Wall -Werror "$root/linux/uclibc/malloc-test.c" \
	-o "$root/linux/artifacts/malloc-test"
# Strict C has its own spelling of struct stat; it must be libc's.
"${cross}gcc" -std=c99 -Wall -Werror -fsyntax-only "$root/linux/uclibc/stat-layout-test.c"
"${cross}gcc" -O2 -Wall -Werror -pthread -static \
	"$root/linux/uclibc/pthread-test.c" -o "$root/linux/artifacts/pthread-test-static"
"${cross}gcc" -Os -Wall -Werror -pthread "$root/linux/uclibc/runtime-bench.c" \
	-o "$root/linux/artifacts/runtime-bench"
"${cross}gcc" -Os -Wall -Werror -pthread "$root/linux/uclibc/tls-suspend-test.c" \
	-o "$root/linux/artifacts/tls-suspend-test"
"${cross}gcc" -Os -Wall -Werror -fPIC -shared -funwind-tables \
	"$root/linux/uclibc/unwind-library.c" -o "$root/linux/artifacts/unwind-library.so"
python3 "$root/linux/uclibc/legacy-eh-header.py" \
	"$root/linux/artifacts/unwind-library.so" "$root/linux/artifacts/unwind-legacy-library.so"
"${cross}gcc" -nostdlib -static "$root/linux/uclibc/tls-exec-test.S" \
	-o "$root/linux/artifacts/tls-exec-test"

"${cross}gcc" -Os -Wall -Werror -fPIC -shared \
	-Wl,-soname,tlslib.so "$root/linux/uclibc/tls-library.c" \
	-o "$build_dir/tlslib.so"
"${cross}gcc" -Os -Wall -Werror -pthread "$root/linux/uclibc/nptl-test.c" \
	"$build_dir/tlslib.so" -o "$build_dir/nptl-test"
cp "$build_dir/tlslib.so" "$root/linux/artifacts/tls-library.so"
cp "$build_dir/nptl-test" "$root/linux/artifacts/nptl-test"
"${cross}gcc" -Os -Wall -Werror -fPIC -shared \
	-Wl,-soname,tlslate.so "$root/linux/uclibc/tls-late-library.c" \
	-o "$root/linux/artifacts/tls-late-library.so"

# Independently executed processes share buffers and kernel IPC objects.
"${cross}gcc" -std=gnu11 -Os -Wall -Werror -pthread \
	"$root/linux/uclibc/ipc-test.c" -lrt -o "$build_dir/ipc-test"
cp "$build_dir/ipc-test" "$root/linux/artifacts/ipc-test"

# Atomics wider than the inline ones come from libatomic.
"${cross}gcc" -std=gnu11 -Os -Wall -Werror -pthread \
	"$root/linux/uclibc/atomic-test.c" -latomic -o "$root/linux/artifacts/atomic-test"
"${cross}g++" -Os -Wall -Werror -pthread \
	"$root/linux/uclibc/cxx-test.cc" -latomic -o "$root/linux/artifacts/cxx-test"
python3 "$root/host-tools/toolchain-c33/gcc/tools/check-runtime.py" \
	"$prefix" "$root/linux/artifacts/cxx-test"

"${cross}size" "$build_dir/uclibc-smoke" "$build_dir/uclibc-smoke-static"
