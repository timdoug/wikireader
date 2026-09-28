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
# sources, so always start clean: about 16 minutes, most of it host tools for
# X.
# Downloads are kept in the source tree's dl/.
rm -rf "$build_dir"
make -C "$source_dir" O="$build_dir" BR2_EXTERNAL="$external" \
	wikireader_defconfig
make -C "$build_dir"

mkdir -p "$root/linux/artifacts"
cp "$build_dir/images/rootfs.ext4" "$root/linux/artifacts/linux.img"
printf '%s\n' "WikiReader root filesystem installed at linux/artifacts/linux.img"
