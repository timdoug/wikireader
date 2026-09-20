#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
	echo "usage: $0 CROSS_PREFIX BUILD_DIR OUTPUT.list" >&2
	exit 2
fi

cross=$1
build=$2
manifest=$3
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

mkdir -p "$build"
"${cross}as" -mc33pe "$here/init.S" -o "$build/init.o"
if "${cross}readelf" -r "$build/init.o" | grep -q 'R_C33'; then
	echo "init contains relocations and cannot use the minimal bFLT wrapper" >&2
	exit 1
fi
"${cross}objcopy" -O binary --only-section=.text "$build/init.o" \
	"$build/init.text"
python3 "$here/make-flat.py" "$build/init.text" "$build/init"
chmod 755 "$build/init"
printf 'dir /dev 0755 0 0\nnod /dev/console 0600 0 0 c 5 1\nfile /init %s/init 0755 0 0\n' \
	"$build" >"$manifest"
