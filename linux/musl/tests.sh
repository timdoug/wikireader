#!/bin/sh
# Build the C library's regression programs (linux/uclibc/*.c, which are
# plain POSIX) against musl, once musl and the final compiler are
# installed, into linux/artifacts/musl under the 8.3 names the card's FAT
# partition needs.  Until musl is the root image's C library they ride on
# the card with musl's ld.so beside them, at /mnt/sd/ldmusl.so;
# musl/run-tests.py runs them there.
set -eu

if [ "$(uname -s)" != Linux ]; then
	echo "musl/tests.sh must run inside the wr-linux VM" >&2
	exit 1
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
guest_root=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
tool_dir=${C33_TOOLCHAIN_WORK:-$guest_root/toolchain}
target=c33-linux-musl
prefix=$tool_dir/install
cross=$prefix/bin/$target-
sysroot=$prefix/$target/sysroot
src=$root/linux/uclibc
out=$root/linux/artifacts/musl
cc="${cross}gcc -Wl,-dynamic-linker,/mnt/sd/ldmusl.so"
cxx="${cross}g++ -Wl,-dynamic-linker,/mnt/sd/ldmusl.so"

rm -rf "$out"
mkdir -p "$out"
"${cross}strip" -o "$out/ldmusl.so" "$sysroot/usr/lib/libc.so"
# The shared runtimes, which run-tests.py copies to /tmp under their names.
"${cross}strip" -o "$out/gcc_s.so" "$prefix/$target/lib/libgcc_s.so.1"
"${cross}strip" -o "$out/stdcxx.so" "$(readlink -f "$prefix/$target"/lib/libstdc++.so.6)"
"${cross}strip" -o "$out/atomic.so" "$(readlink -f "$prefix/$target"/lib/libatomic.so.1)"

# The smoke test links statically, so that its undefined symbols show what
# the static library fails to supply.
"${cross}gcc" -Os -static "$src/smoke.c" -o "$out/smokes.bin"
undefined=$("${cross}nm" -u "$out/smokes.bin" | grep -v '^ *w ' || true)
if [ -n "$undefined" ]; then
	echo "static musl smoke test has undefined symbols:" >&2
	echo "$undefined" >&2
	exit 1
fi
$cc -Os "$src/smoke.c" -o "$out/smoke.bin"
$cc -O2 -Wall -Werror "$src/relocation-test.c" -o "$out/reloc.bin"
$cc -O2 -Wall -Werror "$src/origin-test.c" \
	-Wl,--enable-new-dtags,-rpath,'$ORIGIN' -o "$out/orign.bin"
$cc -O2 -Wall -Werror -Wl,-z,stack-size=32768 "$src/exec-stack-test.c" \
	-o "$out/execstk.bin"
$cc -O2 -Wall -Werror "$src/devmem-test.c" -o "$out/devmem.bin"
$cc -O2 -Wall -Werror "$src/glob-test.c" -o "$out/glob.bin"
$cc -O2 -Wall -Werror "$src/time64-test.c" -o "$out/time64.bin"
$cc -O2 -Wall -Werror "$src/malloc-test.c" -o "$out/malloc.bin"
$cc -Os -Wall -Werror -pthread "$src/signal-test.c" "$src/signal-entry.S" \
	-o "$out/sigtest.bin"
$cc -O2 -Wall -Werror -pthread "$src/pthread-test.c" -o "$out/pthtest.bin"
"${cross}gcc" -O2 -Wall -Werror -pthread -static "$src/pthread-test.c" \
	-o "$out/pthstat.bin"
$cc -Os -Wall -Werror -pthread "$src/runtime-bench.c" -o "$out/bench.bin"
$cc -Os -Wall -Werror -fPIC -shared "$src/origin-library.c" -o "$out/origin.so"
"${cross}gcc" -nostdlib -static "$src/tls-exec-test.S" -o "$out/tlsexec.bin"
$cc -Os -Wall -Werror -fPIC -shared -Wl,-soname,tlslib.so \
	"$src/tls-library.c" -o "$out/tlslib.so"
$cc -Os -Wall -Werror -pthread "$src/nptl-test.c" "$out/tlslib.so" \
	-o "$out/nptl.bin"
$cc -Os -Wall -Werror -fPIC -shared -Wl,-soname,tlslate.so \
	"$src/tls-late-library.c" -o "$out/tlslate.so"
$cc -std=gnu11 -Os -Wall -Werror -pthread "$src/ipc-test.c" -o "$out/ipctest.bin"
$cc -std=gnu11 -Os -Wall -Werror -pthread "$src/atomic-test.c" -latomic \
	-o "$out/atomtest.bin"
$cc -Os -Wall -Werror -fPIC -shared -funwind-tables "$src/unwind-library.c" \
	-o "$out/unwind.so"
python3 "$src/legacy-eh-header.py" "$out/unwind.so" "$out/unwind0.so"
$cxx -Os -Wall -Werror -pthread "$src/cxx-test.cc" -latomic -o "$out/cxxtest.bin"
C33_TARGET=$target python3 "$root/host-tools/toolchain-c33/gcc/tools/check-runtime.py" \
	"$prefix" "$out/cxxtest.bin"
"${cross}size" "$out/smoke.bin" "$out/smokes.bin"
