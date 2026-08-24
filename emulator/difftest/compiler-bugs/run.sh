#!/bin/bash
# Demonstrate the gcc 3.3.2 (c33) narrowing-cast miscompilation.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TC="$HERE/../../../host-tools/toolchain-install/bin"
RT="$HERE/../runtime"
W=/tmp/dt-known; mkdir -p "$W"
"$TC/c33-epson-elf-as" -mc33pe "$RT/start.s" -o "$W/start.o" || exit 2
"$TC/c33-epson-elf-gcc" -mc33pe -w -O2 -I "$RT" -S "$HERE/gcc-drops-narrowing-cast.c" -o "$W/bug.s" || exit 2
"$TC/c33-epson-elf-gcc" -mc33pe -w -O2 -I "$RT" -c "$HERE/gcc-drops-narrowing-cast.c" -o "$W/bug.o" || exit 2
"$TC/c33-epson-elf-ld" -T "$RT/target.lds" "$W/start.o" "$W/bug.o" \
	-L"$TC/../lib/gcc-lib/c33-epson-elf/3.3.2" -lgcc -o "$W/bug.elf" || exit 2

echo "--- generated code for main ---"
sed -n '/^main:/,/^\tret/p' "$W/bug.s"
echo
echo "--- executed ---"
"$HERE/../../wremu" -n 20000000 "$W/bug.elf" 2>/dev/null | grep -E '^[0-9a-f]{8}$' \
 | awk 'NR==1{print "  compiler-folded : "$1} NR==2{print "  run-time        : "$1} NR==3{print "  (signed char) x : "$1}'
echo "  C requires      : 00000168   (0x78 * 3 = 360)"
