#!/bin/sh
# Every test on a built world (linux/world.sh), one after another: the
# regression tests, then the long suites.  Each step's output goes to
# linux/artifacts/battery/NAME.log, and its exit status and minutes to
# summary.txt; a failing step does not stop the ones after it.  The GCC
# suites take more than a day; linux/README.md's "Expected failures" names
# every result that does not pass.
#
#   linux/battery.sh
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root/linux"
out=artifacts/battery
mkdir -p "$out"
: > "$out/summary.txt"
vm() {
	limactl shell --workdir "$root" "${WR_LINUX_VM:-wr-linux}" \
		env JOBS="${JOBS:-4}" nice -n 19 "$@"
}
step() {
	name=$1
	shift
	s=$(date +%s)
	echo "=== $(date +%T) $name" >> "$out/summary.txt"
	"$@" > "$out/$name.log" 2>&1
	echo "    exit $?, $(( ($(date +%s) - s) / 60 )) min" >> "$out/summary.txt"
}
step boot ./boot-test.sh
step app python3 ./app-test.py
step gdb python3 ./gdb-test.py
step core python3 ./debug/core-test.py
step signal python3 ./signal-test.py
step x11-build vm sh ./linux/x11/build.sh
step x11 python3 ./x11/run.py
step dt-check vm ./linux/dt-check.sh
step binutils vm sh -c 'make -C "$HOME/wr-linux/toolchain/binutils-2.47/build-c33-linux-musl" -k check-gas check-binutils check-ld'
step upstream-build vm python3 ./linux/upstream-tests/build.py
step libc-test nice -n 19 python3 upstream-tests/run.py libc --output "$out/libc-test"
step busybox nice -n 19 python3 upstream-tests/run.py busybox --output "$out/busybox"
step ltp-port-build vm python3 ./linux/ltp/build.py --port-tests --output ./linux/artifacts/ltp-port
step ltp-port nice -n 19 python3 ltp/run.py --binaries ./artifacts/ltp-port --output ./artifacts/ltp-port-run
step ltp-all-build vm python3 ./linux/ltp/build.py --all --output ./linux/artifacts/ltp-all
step ltp-all nice -n 19 python3 ltp/sweep.py --timeouts ./ltp/timeouts.json --wall-timeout 2400
step libffi-build vm ./linux/libffi/build.sh
step libffi nice -n 19 python3 libffi/run.py
step unicode-build vm sh ./linux/console/build-tests.sh
step unicode nice -n 19 python3 console/unicode-test.py
step package-build vm python3 ./linux/package-suites/build.py
step package-suites nice -n 19 python3 package-suites/run.py
step gcc env C33L_OUT=artifacts/gcc-test nice -n 19 python3 gcc-test/run.py all
step g++ env C33L_OUT=artifacts/gcc-test nice -n 19 python3 gcc-test/run.py --tool=g++
step libatomic env C33L_OUT=artifacts/libatomic-test nice -n 19 python3 gcc-test/run.py --tool=libatomic all
step libstdc++ env C33L_OUT=artifacts/libstdc++-test nice -n 19 python3 gcc-test/run.py --tool=libstdc++ all
echo "=== $(date +%T) battery done" >> "$out/summary.txt"
