#!/bin/sh
# Build musl with c33-linux-musl-gcc and install it, with the kernel's UAPI
# headers, into that compiler's sysroot.
#
#   build.sh [headers]
#
# "headers" installs only the headers, using the bare-metal compiler: the
# step linux/toolchain.sh needs before it can build c33-linux-musl-gcc.
set -eu

if [ "$(uname -s)" != Linux ]; then
	echo "musl/build.sh must run inside the wr-linux VM" >&2
	exit 1
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
guest_root=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
linux_source=${WR_LINUX_SOURCE:-$guest_root/linux-src}
musl_source=${WR_MUSL_SOURCE:-$guest_root/musl}
tool_dir=${C33_TOOLCHAIN_WORK:-$guest_root/toolchain}
build_dir=${WR_MUSL_BUILD:-$guest_root/musl-build}
headers_build=${WR_LINUX_HEADERS_BUILD:-$guest_root/linux-headers-build-musl}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
target=c33-linux-musl
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

if [ ! -d "$musl_source/.git" ]; then
	echo "musl source not found at $musl_source" >&2
	echo "Run make -C linux fetch first." >&2
	exit 1
fi

"$root/linux/sync-trees.sh" linux musl
rm -rf "$sysroot" "$headers_build" "$build_dir"
mkdir -p "$sysroot/usr" "$headers_build" "$build_dir"
make -C "$linux_source" O="$headers_build" ARCH=c33 CROSS_COMPILE="$cross" \
	INSTALL_HDR_PATH="$sysroot/usr" headers_install

# musl's own allocator, mallocng, takes memory from brk, which no-MMU Linux
# does not have, and gives it back in pieces, which a no-MMU mapping cannot
# be; oldmalloc grows by mmap and keeps what it has.
cd "$build_dir"
"$musl_source/configure" --target=$target --prefix=/usr --syslibdir=/lib \
	--with-malloc=oldmalloc CC="${cross}gcc" AR="${cross}ar" \
	RANLIB="${cross}ranlib"

if [ "$mode" = headers ]; then
	make install-headers DESTDIR="$sysroot"
	printf '%s\n' "C33 kernel and musl headers installed in $sysroot"
	exit 0
fi

make -j"$jobs"
make install DESTDIR="$sysroot"
test -f "$sysroot/usr/lib/libc.a"
# ld.so is a link to /usr/lib/libc.so, which the sysroot is not.
test -L "$sysroot/lib/ld-musl-c33.so.1"

if "${cross}readelf" -d "$sysroot/usr/lib/libc.so" | grep -q 'NEEDED'; then
	echo "libc.so unexpectedly needs another library" >&2
	exit 1
fi

libc_bytes=$(wc -c < "$sysroot/usr/lib/libc.so")
printf '%s\n' "C33 libc.so: $libc_bytes bytes"
printf '%s\n' "C33 musl installed in $sysroot"
