#!/bin/sh
# No-MMU reproducers for the kernel fixes offered upstream, on QEMU. Runs
# inside the wr-linux VM.
#
#   ARCH=riscv: the virt machine with nommu_virt_defconfig (riscv64)
#   ARCH=m68k:  the mcf5208evb machine with m5208evb_defconfig (ColdFire)
#
#   run.sh build LINUX-TREE OUT     build the test programs and a kernel
#   run.sh test OUT TEST            boot OUT's kernel on TEST; log in OUT/TEST.log
#
# Tests are listed in init.c. QEMU's mcf5208evb machine passes no command
# line, so on m68k the test is built into the kernel: TEST=name at build
# time, "stack" by default.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
arch=${ARCH:-riscv}
jobs=${JOBS:-4}

case $arch in
riscv)
	cross=riscv64-linux-gnu-
	defconfig=nommu_virt_defconfig
	cflags_arch="-march=rv64imac -mabi=lp64"
	ldflags_arch="-static-pie -Wl,--no-relax"
	extra_src=
	# The programs run without a libc to relocate them.
	allowed_relocs=none
	image=arch/riscv/boot/Image
	target=Image
	;;
m68k)
	cross=m68k-linux-gnu-
	defconfig=m5208evb_defconfig
	cflags_arch="-mcpu=5208"
	# Debian's m68k GCC links -static-pie as ET_EXEC; the no-MMU loader
	# accepts only ET_DYN.
	ldflags_arch="-Wl,-pie,--no-dynamic-linker,-z,text"
	# The programs address data through the GOT; reloc.c applies the
	# R_68K_RELATIVE entries at startup.
	extra_src=$here/reloc.c
	allowed_relocs=R_68K_RELATIVE
	image=vmlinux
	target=vmlinux
	;;
*)
	echo "ARCH must be riscv or m68k" >&2
	exit 2
	;;
esac

kmake() {
	make -s -C "$tree" O="$out" ARCH="$arch" CROSS_COMPILE="$cross" "$@"
}

configure_kernel() {
	kmake "$defconfig"
	{
		cat "$here/nommu.config" "$here/nommu-$arch.config"
		echo "CONFIG_INITRAMFS_SOURCE=\"$out/initramfs.list\""
		if [ "$arch" = m68k ]; then
			echo "CONFIG_BOOTPARAM_STRING=\"console=ttyS0,115200 rdinit=/init -- ${TEST:-stack}\""
		fi
	} > "$out/fragment.config"
	"$tree/scripts/kconfig/merge_config.sh" -m -O "$out" "$out/.config" "$out/fragment.config" >/dev/null
	kmake olddefconfig
	# Every fragment line must survive olddefconfig.
	grep -v '^#' "$out/fragment.config" | while read -r line; do
		grep -qxF "$line" "$out/.config" || { echo "not set: $line" >&2; exit 1; }
	done
	kmake headers_install INSTALL_HDR_PATH="$out/hdr"
}

build_progs() {
	progs=$out/progs
	mkdir -p "$progs"
	cflags="-Os $cflags_arch -I$out/hdr/include -ffreestanding -fPIE -fno-stack-protector -fno-builtin -Wall -Wextra -Werror"
	ldflags="-nostdlib $ldflags_arch -Wl,-z,noexecstack"
	for p in init stack-victim stack-victim-64k stack-victim-default holder; do
		src=$p extra=
		case $p in
		stack-victim) extra=-Wl,-z,stack-size=8192 ;;
		stack-victim-64k) src=stack-victim extra=-Wl,-z,stack-size=65536 ;;
		stack-victim-default) src=stack-victim ;;
		esac
		${cross}gcc $cflags $ldflags $extra -o "$progs/$p" \
			"$here/start-$arch.S" "$here/$src.c" $extra_src
		if ! ${cross}readelf -h "$progs/$p" | grep -q 'Type: *DYN'; then
			echo "$p is not ET_DYN" >&2
			exit 1
		fi
		if ${cross}readelf -r "$progs/$p" | grep -E '^[0-9a-f]+ ' |
		   grep -v -w -e "$allowed_relocs" | grep -q .; then
			echo "$p contains relocations other than $allowed_relocs" >&2
			exit 1
		fi
	done
	printf 'ramfs data\n' > "$progs/data"
	sed -e "s|@PROGS@|$progs|" "$here/initramfs.list" > "$out/initramfs.list"
}

case ${1:-} in
build)
	tree=$2
	out=$(mkdir -p "$3" && cd "$3" && pwd)
	configure_kernel
	build_progs
	nice -n 19 make -s -C "$tree" O="$out" ARCH="$arch" CROSS_COMPILE="$cross" -j"$jobs" "$target"
	;;
test)
	out=$2 test=$3 log=$2/$3.log
	case $arch in
	riscv)
		set -- qemu-system-riscv64 -machine virt -cpu rv64 -smp 1 -m 128M \
			-bios none -append "earlycon=uart8250,mmio,0x10000000,115200n8 console=ttyS0 rdinit=/init -- $test"
		;;
	m68k)
		grep -q -- "-- $test\"" "$out/.config" || {
			echo "$out was built for another test; rebuild with TEST=$test" >&2
			exit 1
		}
		set -- qemu-system-m68k -machine mcf5208evb -cpu m5208
		;;
	esac
	"$@" -nographic -no-reboot -monitor none -serial stdio \
		-kernel "$out/$image" > "$log" 2>&1 &
	qemu=$!
	# Wait for the guest's last line or a panic, then for QEMU to exit
	# after the guest restarts; 300 s bounds a hang.
	n=0
	while kill -0 "$qemu" 2>/dev/null && [ $n -lt 1500 ]; do
		if grep -q -E 'TEST-DONE|Kernel panic' "$log"; then
			m=0
			while kill -0 "$qemu" 2>/dev/null && [ $m -lt 25 ]; do
				sleep 0.2
				m=$((m + 1))
			done
			break
		fi
		sleep 0.2
		n=$((n + 1))
	done
	kill "$qemu" 2>/dev/null || true
	wait "$qemu" 2>/dev/null || true
	grep -E 'TEST-|SWEEP-|sweep .*(FAIL|killed)|WARNING:|kernel BUG|Kernel panic|Oops|Unable to handle|/stack-victim:|mmap|child |execve|match|Allocation of length' "$log" || true
	;;
*)
	sed -n '2,14p' "$0" >&2
	exit 2
	;;
esac
