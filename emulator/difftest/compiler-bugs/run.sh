#!/bin/bash
# Demonstrate the gcc 3.3.2 (c33) narrowing-cast miscompilation.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
TC="$HERE/../../../host-tools/toolchain-install/bin"
RT="$HERE/../runtime"
W="${DT_WORK:-/tmp/dt-known}"; mkdir -p "$W"
"$TC/c33-epson-elf-gcc" -mc33pe -w -O2 -I "$RT" -S "$HERE/gcc-drops-narrowing-cast.c" -o "$W/bug.s" || exit 2

echo "--- generated code for main ---"
sed -n '/^main:/,/^\tret/p' "$W/bug.s"
echo
echo "--- executed ---"
DT_WORK="$W" DT_TC="$TC" python3 "$HERE/../run.py" \
 --source "$HERE/gcc-drops-narrowing-cast.c" --output "$W/bug.out"
cat "$W/bug.out" \
 | awk 'NR==1{print "  compiler-folded : "$1} NR==2{print "  run-time        : "$1} NR==3{print "  (signed char) x : "$1}'
echo "  C requires      : 00000168   (0x78 * 3 = 360)"
