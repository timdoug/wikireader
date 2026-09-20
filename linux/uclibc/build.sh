#!/bin/sh
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
headers_dir=${WR_LINUX_HEADERS:-$guest_root/linux-headers}
install_dir=${WR_UCLIBC_INSTALL:-$guest_root/uclibc-install}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
cross=$tool_dir/install/bin/c33-epson-elf-

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
rm -rf "$headers_dir" "$build_dir" "$install_dir"
mkdir -p "$headers_dir" "$build_dir" "$install_dir"
make -C "$linux_source" ARCH=c33 CROSS_COMPILE="$cross" \
	INSTALL_HDR_PATH="$headers_dir" headers_install

cp -R "$root/linux/uclibc/overlay/." "$uclibc_source/"
make -C "$uclibc_source" O="$build_dir" ARCH=c33 \
	CROSS_COMPILE="$cross" defconfig

# The generated configuration is deliberately adjusted here rather than
# committing build-machine paths into the architecture defconfig.
sed -i "s|^KERNEL_HEADERS=.*|KERNEL_HEADERS=\"$headers_dir/include\"|" \
	"$build_dir/.config"
make -C "$uclibc_source" O="$build_dir" ARCH=c33 \
	CROSS_COMPILE="$cross" olddefconfig
make -C "$uclibc_source" O="$build_dir" ARCH=c33 \
	CROSS_COMPILE="$cross" -j"$jobs"
make -C "$uclibc_source" O="$build_dir" ARCH=c33 \
	CROSS_COMPILE="$cross" PREFIX="$install_dir" install

test -f "$build_dir/lib/libc.a"
sysroot=$install_dir/usr/c33-linux-uclibc/usr
gcc_lib=$("${cross}gcc" -mc33pe -print-libgcc-file-name)
"${cross}gcc" -mc33pe -medda32 -mlong-calls -Os \
	-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -isystem "$sysroot/include" \
	-c "$root/linux/uclibc/smoke.c" -o "$build_dir/smoke.o"
"${cross}ld" --gc-sections --emit-relocs \
	-T "$root/linux/uclibc/static-flat.ld" -o "$build_dir/smoke.elf" \
	"$build_dir/lib/crt1.o" "$build_dir/lib/crti.o" \
	"$build_dir/smoke.o" --start-group "$build_dir/lib/libc.a" \
	"$gcc_lib" --end-group "$build_dir/lib/crtn.o"
undefined=$("${cross}nm" -u "$build_dir/smoke.elf")
if [ -n "$undefined" ]; then
	echo "static uClibc smoke test has undefined symbols:" >&2
	echo "$undefined" >&2
	exit 1
fi
python3 "$root/linux/initramfs/make-flat.py" "$build_dir/smoke.elf" \
	"$build_dir/uclibc-smoke"
chmod 755 "$build_dir/uclibc-smoke"
mkdir -p "$root/linux/artifacts"
cp "$build_dir/uclibc-smoke" "$root/linux/artifacts/uclibc-smoke"

libc_bytes=$(wc -c <"$build_dir/lib/libc.a")
printf '%s\n' "C33 libc.a: $libc_bytes bytes"
"${cross}size" "$build_dir/smoke.elf"
printf '%s\n' "C33 uClibc-ng installed at $install_dir"
