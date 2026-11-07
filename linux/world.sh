#!/bin/sh
# Everything the Linux port runs on, from this checkout and pinned upstream
# sources, in order: on the Mac, the bare-metal C33 toolchain, the firmware
# a card boots through (MBR flash, Grifo and init.app) and wremu; in the
# wr-linux VM, every source fetched and hash-checked, the Linux toolchain,
# musl, Buildroot's root image, the kernel and the debug tools.
#
#   linux/world.sh
#
# A VM work tree from an earlier build is moved to ~/wr-linux.old first, so
# every VM build directory is new; delete it once the new tree passes.  The
# Mac's toolchain and firmware build in place: for those from nothing too,
# start from a clean checkout (git clean -dfX).  JOBS sets the parallelism,
# 4 by default; every step runs at the lowest priority.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
JOBS=${JOBS:-4}
export JOBS
tools=$root/host-tools/toolchain-c33
vm_name=${WR_LINUX_VM:-wr-linux}
step() {
	echo "=== $(date +%T) $*"
	"$@"
}
vm() {
	limactl shell --workdir "$root" "$vm_name" env JOBS="$JOBS" nice -n 19 "$@"
}

step nice -n 19 "$tools/binutils/build.sh" "$tools/work"
step nice -n 19 "$tools/gcc/rebuild.sh"
step nice -n 19 make -j"$JOBS" TOOLCHAIN_BIN="$tools/work/install/bin" \
	mini-libc fatfs drivers grifo mbr
step nice -n 19 make -C emulator -j"$JOBS" LTO=1

step ./linux/vm.sh
limactl shell "$vm_name" sh -c '
	if [ -e ~/wr-linux.old ]; then
		echo "world.sh: ~/wr-linux.old exists; delete it first" >&2
		exit 1
	fi
	if [ -e ~/wr-linux ]; then mv ~/wr-linux ~/wr-linux.old; fi'
step vm ./linux/provision.sh
step vm ./linux/fetch.sh
step vm ./linux/toolchain.sh
step vm ./linux/musl/build.sh
step vm ./linux/toolchain.sh final
step vm ./linux/musl/tests.sh
step vm ./linux/buildroot/build.sh --full
step vm ./linux/build.sh
step vm ./linux/debug/build.sh
echo "=== $(date +%T) world done"
