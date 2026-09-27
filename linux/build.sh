#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
guest_root=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
source_dir=${WR_LINUX_SOURCE:-$guest_root/linux-src}
tool_dir=${C33_TOOLCHAIN_WORK:-$guest_root/toolchain}
build_dir=${WR_LINUX_BUILD:-$guest_root/linux-build}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
cross=$tool_dir/install/bin/c33-epson-elf-

if [ ! -x "${cross}gcc" ]; then
	echo "C33 compiler not found at ${cross}gcc" >&2
	echo "Run make -C linux toolchain first." >&2
	exit 1
fi

# Keep iterative architecture work in sync without reconstructing the pinned
# upstream tree on every build.
cp -R "$root/linux/overlay/." "$source_dir/"

mkdir -p "$build_dir" "$root/linux/artifacts"
make -C "$source_dir" O="$build_dir" ARCH=c33 CROSS_COMPILE="$cross" wikireader_defconfig

# The system is linux.img on the SD card (make -C linux rootfs); the kernel's
# own initramfs holds only rootstart, which mounts it, and the device nodes
# rootstart needs before devtmpfs.
initramfs=$build_dir/c33-initramfs
here=$root/linux/initramfs
mkdir -p "$initramfs"
"${cross}as" -mc33pe "$here/rootstart.S" -o "$initramfs/rootstart-start.o"
"${cross}gcc" -mc33pe -Os -ffreestanding -fno-builtin -medda32 \
	-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -Wall -Werror \
	-c "$here/rootstart.c" -o "$initramfs/rootstart-c.o"
"${cross}objcopy" --remove-section=.debug_frame "$initramfs/rootstart-c.o" \
	"$initramfs/rootstart-code.o"
"${cross}ld" --gc-sections --emit-relocs -T "$here/init.ld" \
	-o "$initramfs/rootstart.elf" "$initramfs/rootstart-start.o" \
	"$initramfs/rootstart-code.o" "$("${cross}gcc" -mc33pe -print-libgcc-file-name)"
python3 "$here/make-flat.py" "$initramfs/rootstart.elf" "$initramfs/rootstart"
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
"${cross}strip" -o "$root/linux/artifacts/linux.app" "$build_dir/vmlinux"
python3 "$root/samo-lib/grifo/scripts/xpm2icon" \
	--icon="$root/linux/artifacts/linux.ico" "$root/riscv/rvlinux.xpm"
"${cross}size" "$build_dir/vmlinux"
printf '%s\n' "Grifo launcher image installed at linux/artifacts/linux.app"
