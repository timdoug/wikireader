#!/bin/sh
# Build uClibc-ng with c33-linux-uclibc-gcc and install it, with the kernel's
# UAPI headers, into that compiler's sysroot.
#
#   build.sh [headers]
#
# "headers" installs only the headers, using the bare-metal compiler: the
# step linux/toolchain.sh needs before it can build c33-linux-uclibc-gcc.
set -eu

if [ "$(uname -s)" != Linux ]; then
	echo "uclibc/build.sh must run inside the wr-linux VM" >&2
	exit 1
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
guest_root=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
linux_source=${WR_LINUX_SOURCE:-$guest_root/linux-src}
uclibc_source=${WR_UCLIBC_SOURCE:-$guest_root/uclibc-ng}
tool_dir=${C33_TOOLCHAIN_WORK:-$guest_root/toolchain}
build_dir=${WR_UCLIBC_BUILD:-$guest_root/uclibc-build}
headers_build=${WR_LINUX_HEADERS_BUILD:-$guest_root/linux-headers-build}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
target=c33-linux-uclibc
sysroot=$tool_dir/install/$target/sysroot
mode=${1:-all}

if [ "$mode" = headers ]; then
	cross=$tool_dir/install/bin/c33-epson-elf-
else
	cross=$tool_dir/install/bin/$target-
fi
if [ ! -x "${cross}gcc" ]; then
	echo "C33 compiler not found at ${cross}gcc" >&2
	echo "Run make -C linux toolchain first." >&2
	exit 1
fi

if [ ! -d "$uclibc_source/.git" ]; then
	echo "uClibc-ng source not found at $uclibc_source" >&2
	echo "Run make -C linux fetch first." >&2
	exit 1
fi

cp -R "$root/linux/overlay/." "$linux_source/"
rm -rf "$sysroot" "$headers_build" "$build_dir"
mkdir -p "$sysroot/usr" "$headers_build" "$build_dir"
make -C "$linux_source" O="$headers_build" ARCH=c33 CROSS_COMPILE="$cross" \
	INSTALL_HDR_PATH="$sysroot/usr" headers_install

# uClibc installs straight into the sysroot, as Buildroot lays it out.
uclibc_make() {
	make -C "$uclibc_source" O="$build_dir" ARCH=c33 \
		CROSS_COMPILE="$cross" PREFIX="$sysroot" DEVEL_PREFIX=/usr/ \
		RUNTIME_PREFIX=/ "$@"
}

cp -R "$root/linux/uclibc/overlay/." "$uclibc_source/"
uclibc_make defconfig

# The generated configuration is deliberately adjusted here rather than
# committing build-machine paths into the architecture defconfig.
sed -i "s|^KERNEL_HEADERS=.*|KERNEL_HEADERS=\"$sysroot/usr/include\"|" \
	"$build_dir/.config"
uclibc_make olddefconfig

if [ "$mode" = headers ]; then
	uclibc_make install_headers
	printf '%s\n' "C33 kernel and uClibc-ng headers installed in $sysroot"
	exit 0
fi

# libpthread_nonshared.a's rule writes its object list into lib/ with no
# order-only dependency on the directory, which a parallel build can reach
# first.
mkdir -p "$build_dir/lib"
uclibc_make -j"$jobs"
uclibc_make install
test -f "$sysroot/usr/lib/libc.a"
test -f "$sysroot/lib/ld-uClibc.so.1"

# Build the optional unwinder after libc. PT_GNU_EH_FRAME discovery lets
# NPTL load it on the first cancellation; ordinary C programs need no copy.
C33_TARGET=$target "$root/host-tools/toolchain-c33/gcc/rebuild.sh" \
	"$tool_dir" libgcc-shared
# Buildroot copies runtime libraries from the external compiler's sysroot.
cp "$tool_dir/install/$target/lib/libgcc_s.so.1" "$sysroot/lib/"
if "${cross}readelf" -d "$sysroot/lib/libuClibc-"*.so | grep -q '\[libgcc_s.so.1\]'; then
	echo "libc unexpectedly depends on libgcc_s" >&2
	exit 1
fi

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
	-o "$root/linux/artifacts/signal-test"
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

libc_bytes=$(cat "$sysroot"/lib/libuClibc-*.so | wc -c)
printf '%s\n' "C33 libc.so: $libc_bytes bytes"
"${cross}size" "$build_dir/uclibc-smoke" "$build_dir/uclibc-smoke-static"
printf '%s\n' "C33 uClibc-ng installed in $sysroot"
