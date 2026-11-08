#!/bin/sh
# One DejaGnu pass of a GCC runtime library's testsuite, libstdc++ or
# libatomic, for c33-linux-musl, in the VM:
#
#   run-lib.sh --tool=libstdc++|libatomic [runtest arguments]
#
# The suite runs from its build tree, where its support code is built, with
# the build tree's compiler and this directory's board, which queues each
# program to execute that has no recorded result.  run.py runs the queue in
# wremu and repeats the pass until nothing is queued; the pass's summary is
# copied to the run's results.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
target=$work/toolchain/gcc-16.2.0/build-c33-linux-musl/c33-linux-musl
out=${C33L_OUT:?run.py sets C33L_OUT}
case ${1:-} in
--tool=libstdc++) tool=libstdc++ dir=$target/libstdc++-v3/testsuite ;;
--tool=libatomic) tool=libatomic dir=$target/libatomic/testsuite ;;
*) echo "run-lib.sh: --tool=libstdc++ or --tool=libatomic" >&2; exit 1 ;;
esac
shift
[ "${1:-}" != all ] || shift
export PATH=$work/toolchain/install/bin:$PATH
export C33L_GCC=$work/toolchain/install/bin/c33-linux-musl-gcc
export C33L_QUEUE=$out/queue
export C33L_RESULTS=$out/executed
export C33L_OBJDIR=$out/results
export DEJAGNU=$root/linux/gcc-test/site.exp
mkdir -p "$C33L_QUEUE" "$C33L_RESULTS" "$C33L_OBJDIR"
make -C "$dir" check-DEJAGNU \
	RUNTESTFLAGS="--target_board=c33-linux $*" >/dev/null 2>&1 || true
cp "$dir/$tool.sum" "$dir/$tool.log" "$C33L_OBJDIR/"
grep "^# of\|^=== " "$C33L_OBJDIR/$tool.sum" | tail -12
echo "queued: $(ls "$C33L_QUEUE" | grep -cv '\.files$')"
