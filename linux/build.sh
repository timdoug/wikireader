#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
guest_root=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
source_dir=${WR_LINUX_SOURCE:-$guest_root/linux-src}
tool_dir=${C33_TOOLCHAIN_WORK:-$guest_root/toolchain}
build_dir=${WR_LINUX_BUILD:-$guest_root/linux-build}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
cross=$tool_dir/install/bin/c33-epson-elf-

# The same sources build the same kernel.  Left alone, kbuild records who
# built it, on which host, when, and how many times (uname -v and the
# initramfs dates); fix all four, the date to the tree's last commit.
: "${SOURCE_DATE_EPOCH:=$(git -c safe.directory='*' -C "$root" log -1 --format=%ct)}"
export SOURCE_DATE_EPOCH
KBUILD_BUILD_TIMESTAMP=$(LC_ALL=C date -u -d "@$SOURCE_DATE_EPOCH")
export KBUILD_BUILD_TIMESTAMP
export KBUILD_BUILD_USER=wikireader KBUILD_BUILD_HOST=wikireader
export KBUILD_BUILD_VERSION=1

if [ ! -x "${cross}gcc" ]; then
	echo "C33 compiler not found at ${cross}gcc" >&2
	echo "Run make -C linux toolchain first." >&2
	exit 1
fi

# The tree is the pinned kernel plus patches/ and overlay/, and nothing else.
"$root/linux/sync-trees.sh" linux

mkdir -p "$build_dir" "$root/linux/artifacts"
make -C "$source_dir" O="$build_dir" ARCH=c33 CROSS_COMPILE="$cross" wikireader_defconfig

# The system is linux.img on the SD card (make -C linux rootfs); the kernel's
# own initramfs holds only rootstart, which mounts it, and the device nodes
# rootstart needs before devtmpfs.
initramfs=$build_dir/c33-initramfs
here=$root/linux/initramfs
mkdir -p "$initramfs"
"$here/build-freestanding.sh" "$tool_dir/install/bin/c33-linux-uclibc-" \
	"$initramfs/rootstart" "$here/rootstart.S" "$here/rootstart.c" -Wall -Werror
cat >"$initramfs.list" <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/mmcblk0p1 0600 0 0 b 179 1
nod /dev/loop0 0600 0 0 b 7 0
nod /dev/urandom 0600 0 0 c 1 9
dir /mnt 0755 0 0
dir /newroot 0755 0 0
file /init $initramfs/rootstart 0755 0 0
EOF
"$source_dir/scripts/config" --file "$build_dir/.config" --enable \
	BLK_DEV_INITRD
"$source_dir/scripts/config" --file "$build_dir/.config" --set-str \
	INITRAMFS_SOURCE "$initramfs.list"
make -C "$source_dir" O="$build_dir" ARCH=c33 CROSS_COMPILE="$cross" olddefconfig
make -C "$source_dir" O="$build_dir" ARCH=c33 CROSS_COMPILE="$cross" -j"$jobs" vmlinux
cp "$build_dir/vmlinux" "$root/linux/artifacts/vmlinux"
"${cross}objcopy" -O binary "$build_dir/vmlinux" "$root/linux/artifacts/vmlinux.bin"
cp "$build_dir/vmlinux.map" "$root/linux/artifacts/vmlinux.map"
# Grifo loads an ELF by its section addresses, and A0 RAM's sections are
# linked where they run, which is not RAM until head.S has switched IVRAM
# in: the launcher image gives them their load addresses in SDRAM instead,
# where head.S copies them from.
a0=$("${cross}objdump" -h "$build_dir/vmlinux" | awk '
	$2 == ".a0.text" || $2 == ".a0.data" {
		printf " --change-section-vma %s=0x%s", $2, $5 }')
"${cross}objcopy" --strip-all $a0 "$build_dir/vmlinux" \
	"$root/linux/artifacts/linux.app"
python3 "$root/samo-lib/grifo/scripts/xpm2icon" \
	--icon="$root/linux/artifacts/linux.ico" "$root/riscv/rvlinux.xpm"
"${cross}size" "$build_dir/vmlinux"
printf '%s\n' "Grifo launcher image installed at linux/artifacts/linux.app"
