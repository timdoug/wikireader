#!/bin/sh
# Build libffi's call, complex and bhaible tests for the C33 from Buildroot's
# libffi build tree, against its staging libffi, into artifacts/libffi-tests
# (with ffitests.tar for the card).  The closure tests are left out: the
# port has no closures.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
b=${WR_BUILDROOT_BUILD:-$work/buildroot-build}
src=$(echo "$b"/build/libffi-*)
t=$src/testsuite
cc=$b/host/bin/c33-linux-musl-gcc
cfg=$src/c33-buildroot-linux-musl
out=$root/linux/artifacts/libffi-tests
rm -rf "$out"
mkdir -p "$out/bin"
for f in "$t"/libffi.call/*.c; do
	n=$(basename "$f" .c)
	grep -q "ffi_closure\|ffi_prep_closure\|FFI_CLOSURES" "$f" && continue
	"$cc" -O2 -w -I"$cfg" -I"$cfg/include" -I"$src/include" -I"$t/libffi.call" -o "$out/bin/call-$n" "$f" -lffi
done
for f in "$t"/libffi.complex/complex_*.c "$t"/libffi.complex/many_complex_*.c \
	 "$t"/libffi.complex/return_complex*.c; do
	n=$(basename "$f" .c)
	"$cc" -O2 -w -I"$cfg" -I"$t/libffi.complex" -o "$out/bin/complex-$n" "$f" -lffi -lm
done
for i in $(seq 1 81); do
	"$cc" -O2 -w -I"$cfg" -DDGTEST="$i" -I"$t/libffi.bhaible" \
		-o "$out/bin/bhaible-$i" "$t/libffi.bhaible/test-call.c" -lffi
done
tar -cf "$out/ffitests.tar" -C "$out/bin" .
echo "built $(ls "$out/bin" | wc -l) libffi tests in $out"
