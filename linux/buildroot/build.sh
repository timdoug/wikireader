#!/bin/sh
# Build the WikiReader root filesystem with Buildroot, using the
# c33-linux-uclibc toolchain from linux/toolchain.sh, and leave it as the
# ext4 image linux/artifacts/linux.img, which goes on the SD card beside
# linux.app.  WR_LIBC=musl builds the same system with c33-linux-musl, in
# a build directory of its own, as linux/artifacts/linux-musl.img.
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
libc=${WR_LIBC:-uclibc}
case $libc in
uclibc)	image=linux.img symbols=symbols ;;
musl)	image=linux-musl.img symbols=symbols-musl
	build_dir=${WR_BUILDROOT_BUILD:-$guest_root/buildroot-build-musl} ;;
*)	echo "WR_LIBC is uclibc or musl" >&2; exit 1 ;;
esac

if [ ! -x "$tool_dir/install/bin/c33-linux-$libc-gcc" ]; then
	echo "c33-linux-$libc-gcc not found under $tool_dir/install" >&2
	echo "Run make -C linux toolchain libc (or musl-toolchain musl-libc) first." >&2
	exit 1
fi
if [ ! -d "$source_dir/.git" ]; then
	echo "Buildroot source not found at $source_dir" >&2
	echo "Run make -C linux fetch first." >&2
	exit 1
fi
"$root/linux/sync-trees.sh" buildroot

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
		"$build_dir/host/c33-buildroot-linux-$libc/sysroot"
fi
if [ "$libc" = uclibc ]; then
	make -C "$source_dir" O="$build_dir" BR2_EXTERNAL="$external" \
		wikireader_defconfig
else
	# The same system with the other toolchain; Buildroot gives a musl
	# toolchain its wide characters, threads and SSP itself.
	mkdir -p "$build_dir"
	sed -e 's/^\(BR2_TOOLCHAIN_EXTERNAL_CUSTOM_PREFIX\)=.*/\1="c33-linux-musl"/' \
		-e 's/^BR2_TOOLCHAIN_EXTERNAL_CUSTOM_UCLIBC=y/BR2_TOOLCHAIN_EXTERNAL_CUSTOM_MUSL=y/' \
		-e '/^BR2_TOOLCHAIN_EXTERNAL_HAS_C11_THREADS=y/d' \
		"$external/configs/wikireader_defconfig" > "$build_dir/musl_defconfig"
	echo 'BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES="$(BR2_EXTERNAL_WIKIREADER_PATH)/board/wikireader/busybox-musl.fragment"' \
		>> "$build_dir/musl_defconfig"
	make -C "$source_dir" O="$build_dir" BR2_EXTERNAL="$external" \
		BR2_DEFCONFIG="$build_dir/musl_defconfig" defconfig
fi
# JOBS caps the parallel jobs, as it does for the kernel and the C library.
make -C "$build_dir" ${JOBS:+BR2_JLEVEL=$JOBS}

mkdir -p "$root/linux/artifacts"
cp "$build_dir/images/rootfs.ext4" "$root/linux/artifacts/$image"
rm -rf "$root/linux/artifacts/$symbols"
cp -R "$build_dir/images/symbols" "$root/linux/artifacts/$symbols"
printf '%s\n' "WikiReader root filesystem installed at linux/artifacts/$image"
