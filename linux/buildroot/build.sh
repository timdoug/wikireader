#!/bin/sh
# Build the WikiReader root filesystem with Buildroot, using the
# c33-linux-uclibc toolchain from linux/toolchain.sh, and leave it as the
# ext4 image linux/artifacts/linux.img, which goes on the SD card beside
# linux.app.
set -eu

if [ "$(uname -s)" != Linux ]; then
	echo "buildroot/build.sh must run inside the wr-linux VM" >&2
	exit 1
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
guest_root=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
source_dir=${WR_BUILDROOT_SOURCE:-$guest_root/buildroot}
build_dir=${WR_BUILDROOT_BUILD:-$guest_root/buildroot-build}
tool_dir=${C33_TOOLCHAIN_WORK:-$guest_root/toolchain}
external=$root/linux/buildroot/external

if [ ! -x "$tool_dir/install/bin/c33-linux-uclibc-gcc" ]; then
	echo "c33-linux-uclibc-gcc not found under $tool_dir/install" >&2
	echo "Run make -C linux toolchain libc first." >&2
	exit 1
fi
if [ ! -d "$source_dir/.git" ]; then
	echo "Buildroot source not found at $source_dir" >&2
	echo "Run make -C linux fetch first." >&2
	exit 1
fi

# Buildroot does not notice a rebuilt C library or edited local package
# sources, so every target package is built afresh each time.  The host
# tools (Meson, Ninja, Python, bison and the rest, built with the VM's own
# compiler) depend on nothing of the C33's, so they are kept: host/ and the
# host-* build directories stay, and everything built for the target goes,
# the staging sysroot included.  --full starts from nothing, as a change to
# a host package's options needs.  Downloads are kept in the source tree's
# dl/.
if [ "${1:-}" = --full ] || [ ! -d "$build_dir/host" ]; then
	rm -rf "$build_dir"
else
	for d in "$build_dir"/build/*; do
		case ${d##*/} in
		host-*|packages-file-list-host.txt) ;;
		*) rm -rf "$d" ;;
		esac
	done
	rm -rf "$build_dir/target" "$build_dir/images" \
		"$build_dir/host/c33-buildroot-linux-uclibc/sysroot"
fi
make -C "$source_dir" O="$build_dir" BR2_EXTERNAL="$external" \
	wikireader_defconfig
make -C "$build_dir"

mkdir -p "$root/linux/artifacts"
cp "$build_dir/images/rootfs.ext4" "$root/linux/artifacts/linux.img"
rm -rf "$root/linux/artifacts/symbols"
cp -R "$build_dir/images/symbols" "$root/linux/artifacts/symbols"
printf '%s\n' "WikiReader root filesystem installed at linux/artifacts/linux.img"
