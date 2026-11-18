#!/bin/sh
# No-MMU reproducers for the kernel fixes offered upstream, on QEMU's RISC-V
# virt machine with nommu_virt_defconfig. Runs inside the wr-linux VM.
#
#   run.sh build LINUX-TREE OUT     build the test programs and a kernel
#   run.sh test OUT TEST            boot OUT's kernel on TEST; log in OUT/TEST.log
#
# Tests are listed in init.c.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
cross=riscv64-linux-gnu-
jobs=${JOBS:-4}

build_progs() {
	progs=$1/progs
	mkdir -p "$progs"
	cflags="-Os -march=rv64imac -mabi=lp64 -ffreestanding -fPIE -fno-stack-protector -fno-builtin -Wall -Wextra -Werror"
	ldflags="-nostdlib -static-pie -Wl,--no-relax -Wl,-z,noexecstack"
	for p in init stack-victim holder; do
		extra=
		[ "$p" = stack-victim ] && extra=-Wl,-z,stack-size=8192
		${cross}gcc $cflags $ldflags $extra -o "$progs/$p" \
			"$here/start.S" "$here/$p.c"
		# These programs run without a libc to relocate them, so they
		# must contain no relocations.
		if ${cross}readelf -r "$progs/$p" | grep -q 'Relocation section'; then
			echo "$p contains relocations" >&2
			exit 1
		fi
	done
	printf 'ramfs data\n' > "$progs/data"
	sed -e "s|@PROGS@|$progs|" "$here/initramfs.list" > "$1/initramfs.list"
}

build_kernel() {
	tree=$1 out=$2
	make -s -C "$tree" O="$out" ARCH=riscv CROSS_COMPILE=$cross nommu_virt_defconfig
	{
		cat "$here/nommu.config"
		echo "CONFIG_INITRAMFS_SOURCE=\"$out/initramfs.list\""
	} > "$out/fragment.config"
	"$tree/scripts/kconfig/merge_config.sh" -m -O "$out" "$out/.config" "$out/fragment.config" >/dev/null
	make -s -C "$tree" O="$out" ARCH=riscv CROSS_COMPILE=$cross olddefconfig
	# Every fragment line must survive olddefconfig.
	grep -v '^#' "$out/fragment.config" | while read -r line; do
		grep -qxF "$line" "$out/.config" || { echo "not set: $line" >&2; exit 1; }
	done
	nice -n 19 make -s -C "$tree" O="$out" ARCH=riscv CROSS_COMPILE=$cross -j"$jobs" Image
}

case ${1:-} in
build)
	out=$(mkdir -p "$3" && cd "$3" && pwd)
	build_progs "$out"
	build_kernel "$2" "$out"
	;;
test)
	out=$2 test=$3
	# The guest powers off after TEST-DONE; the timeout bounds a hang.
	timeout 120 qemu-system-riscv64 -machine virt -cpu rv64 -smp 1 -m 128M \
		-bios none -nographic -no-reboot -monitor none -serial stdio \
		-kernel "$out/arch/riscv/boot/Image" \
		-append "earlycon=uart8250,mmio,0x10000000,115200n8 console=ttyS0 rdinit=/init -- $test" \
		> "$out/$test.log" 2>&1 || true
	grep -E 'TEST-|WARNING:|kernel BUG|Kernel panic|Oops|Unable to handle|/stack-victim:|mmap|child |execve|match|Allocation of length' "$out/$test.log" || true
	;;
*)
	sed -n '2,10p' "$0" >&2
	exit 2
	;;
esac
