#!/bin/sh
# Build libffi's call, complex and bhaible tests for the C33, with the
# library they test, into artifacts/libffi-tests: bin/ and lib/, and both in
# ffitests.tar for the card.  The system image has no libffi (nothing on it
# uses one), so Buildroot builds its libffi package, with the port's patch,
# in an output directory of its own; the closure tests are left out, as the
# port has no closures.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
source_dir=${WR_BUILDROOT_SOURCE:-$work/buildroot}
b=${WR_LIBFFI_BUILD:-$work/libffi-build}
external=$root/linux/buildroot/external

"$root/linux/sync-trees.sh" buildroot
# As buildroot/build.sh does: host tools stay, everything for the target is
# built afresh, since Buildroot does not notice a rebuilt C library.
if [ -d "$b/build" ]; then
	for d in "$b"/build/*; do
		case ${d##*/} in
		host-*|packages-file-list-host.txt) ;;
		*) rm -rf "$d" ;;
		esac
	done
	rm -rf "$b/target" "$(make -C "$b" -s --no-print-directory \
		printvars VARS=STAGING_DIR | sed 's/^STAGING_DIR=//')"
fi
make -C "$source_dir" O="$b" BR2_EXTERNAL="$external" wikireader_defconfig
echo BR2_PACKAGE_LIBFFI=y >> "$b/.config"
make -C "$b" olddefconfig
make -C "$b" ${JOBS:+BR2_JLEVEL=$JOBS} libffi

src=$(echo "$b"/build/libffi-*)
t=$src/testsuite
cc=$b/host/bin/c33-linux-musl-gcc
cfg=$src/c33-buildroot-linux-musl
out=$root/linux/artifacts/libffi-tests
rm -rf "$out"
mkdir -p "$out/bin" "$out/lib"
staging=$(make -C "$b" -s --no-print-directory printvars VARS=STAGING_DIR |
	sed 's/^STAGING_DIR=//')
lib=$(readlink -f "$staging/usr/lib/libffi.so")
soname=$("${cc%gcc}readelf" -d "$lib" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')
cp "$lib" "$out/lib/$soname"
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
tar -cf "$out/ffitests.tar" -C "$out" bin lib
echo "built $(ls "$out/bin" | wc -l) libffi tests in $out"
