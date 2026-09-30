#!/bin/sh
# Build deliberately crashing FDPIC fixtures, and save their matching ELFs.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
guest_root=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
tool_dir=${C33_TOOLCHAIN_WORK:-$guest_root/toolchain}
cross=$tool_dir/install/bin/c33-linux-uclibc-
build=$guest_root/debug-build
symbols=$root/linux/artifacts/symbols/mnt/sd
mkdir -p "$build" "$symbols"
"${cross}gcc" -Os -g -Wall -Werror -shared -fPIC \
    "$root/linux/debug/crash-library.c" -Wl,-soname,crashlib.so -o "$build/crashlib.so"
"${cross}gcc" -Os -g -Wall -Werror "$root/linux/debug/crash-test.c" \
    -L"$build" -l:crashlib.so -o "$build/crashtst.bin"
for name in crashtst.bin crashlib.so; do
    cp "$build/$name" "$symbols/$name"
    cp "$build/$name" "$root/linux/artifacts/$name"
    "${cross}strip" --strip-debug "$root/linux/artifacts/$name"
done
