#!/bin/sh
# One DejaGnu pass of GCC's testsuite for c33-linux-uclibc, in the VM:
#
#   run-gcc.sh [runtest selectors]	(default: the execute torture)
#
# Programs to execute that have no recorded result are queued in
# artifacts/gcc-test/queue for run.py to run in wremu; the pass's
# gcc.sum is in artifacts/gcc-test/results.  run.py repeats the pass until
# nothing is queued.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
gcc_src=$work/toolchain/gcc-16.2.0
out=$root/linux/artifacts/gcc-test
export PATH=$work/toolchain/install/bin:$PATH
export C33L_GCC=$work/toolchain/install/bin/c33-linux-uclibc-gcc
export C33L_BUILD_SITE=$gcc_src/build-c33-linux-uclibc/gcc/site.exp
export C33L_QUEUE=$out/queue
export C33L_RESULTS=$out/executed
export C33L_OBJDIR=$out/results
export DEJAGNU=$root/linux/gcc-test/site.exp
mkdir -p "$C33L_QUEUE" "$C33L_RESULTS" "$C33L_OBJDIR"
# GCC's build-site settings (target tools such as objdump, which the
# effective-target checks use) come from its build tree.
[ -f "$C33L_BUILD_SITE" ] || make -C "$(dirname "$C33L_BUILD_SITE")" site.exp >/dev/null
[ -f "$C33L_BUILD_SITE" ] || { echo "no $C33L_BUILD_SITE" >&2; exit 1; }
[ "$#" -gt 0 ] || set -- gcc.c-torture/execute/execute.exp
cd "$C33L_OBJDIR"
runtest --tool gcc --target c33-linux-uclibc --target_board=c33-linux \
	--srcdir "$gcc_src/gcc/testsuite" --objdir "$C33L_OBJDIR" \
	--tool_exec "$C33L_GCC" "$@" >/dev/null 2>&1 || true
grep "^# of\|^=== " gcc.sum | tail -12
echo "queued: $(ls "$C33L_QUEUE" | wc -l)"
