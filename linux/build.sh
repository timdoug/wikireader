#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
guest_root=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
source_dir=${WR_LINUX_SOURCE:-$guest_root/linux-src}
tool_dir=${C33_TOOLCHAIN_WORK:-$guest_root/toolchain}
build_dir=${WR_LINUX_BUILD:-$guest_root/linux-build}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
cross=$tool_dir/install/bin/c33-epson-elf-
rootfs=$root/linux/artifacts/rootfs.cpio

if [ ! -x "${cross}gcc" ]; then
	echo "C33 compiler not found at ${cross}gcc" >&2
	echo "Run make -C linux toolchain first." >&2
	exit 1
fi
if [ ! -f "$rootfs" ]; then
	echo "Root filesystem not found at $rootfs" >&2
	echo "Run make -C linux rootfs first." >&2
	exit 1
fi

# Keep iterative architecture work in sync without reconstructing the pinned
# upstream tree on every build.
cp -R "$root/linux/overlay/." "$source_dir/"

mkdir -p "$build_dir" "$root/linux/artifacts"
make -C "$source_dir" O="$build_dir" ARCH=c33 CROSS_COMPILE="$cross" wikireader_defconfig
"$source_dir/scripts/config" --file "$build_dir/.config" --enable \
	BLK_DEV_INITRD
"$source_dir/scripts/config" --file "$build_dir/.config" --set-str \
	INITRAMFS_SOURCE "$rootfs"
make -C "$source_dir" O="$build_dir" ARCH=c33 CROSS_COMPILE="$cross" olddefconfig
make -C "$source_dir" O="$build_dir" ARCH=c33 CROSS_COMPILE="$cross" -j"$jobs" vmlinux
cp "$build_dir/vmlinux" "$root/linux/artifacts/vmlinux"
"${cross}objcopy" -O binary "$build_dir/vmlinux" "$root/linux/artifacts/vmlinux.bin"
"${cross}strip" -o "$root/linux/artifacts/linux.app" "$build_dir/vmlinux"
python3 "$root/samo-lib/grifo/scripts/xpm2icon" \
	--icon="$root/linux/artifacts/linux.ico" "$root/riscv/rvlinux.xpm"
"${cross}size" "$build_dir/vmlinux"
printf '%s\n' "Grifo launcher image installed at linux/artifacts/linux.app"
