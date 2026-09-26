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

uclibc_make -j"$jobs"
uclibc_make install
test -f "$sysroot/usr/lib/libc.a"

"${cross}gcc" -Os -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -Wl,--gc-sections \
	"$root/linux/uclibc/smoke.c" -o "$build_dir/uclibc-smoke" \
	-Wl,-elf2flt=--shared-text
undefined=$("${cross}nm" -u "$build_dir/uclibc-smoke.gdb")
if [ -n "$undefined" ]; then
	echo "static uClibc smoke test has undefined symbols:" >&2
	echo "$undefined" >&2
	exit 1
fi
mkdir -p "$root/linux/artifacts"
cp "$build_dir/uclibc-smoke" "$root/linux/artifacts/uclibc-smoke"

# LinuxThreads regression, run from the SD card by app-test.py.
"${cross}gcc" -O2 -Wall -Werror -pthread "$root/linux/uclibc/pthread-test.c" \
	-o "$build_dir/pthread-test" -Wl,-elf2flt=--shared-text
cp "$build_dir/pthread-test" "$root/linux/artifacts/pthread-test"

libc_bytes=$(wc -c <"$sysroot/usr/lib/libc.a")
printf '%s\n' "C33 libc.a: $libc_bytes bytes"
"${cross}size" "$build_dir/uclibc-smoke.gdb"
printf '%s\n' "C33 uClibc-ng installed in $sysroot"
