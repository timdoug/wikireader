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
"${cross}as" -mc33pe "$here/init.S" -o "$build/init-start.o"
"${cross}gcc" -mc33pe -Os -ffreestanding -fno-builtin \
	-medda32 \
	-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -c "$here/init.c" -o "$build/init-c.o"
"${cross}objcopy" --remove-section=.debug_frame "$build/init-c.o" \
	"$build/init-c-code.o"
"${cross}ld" --gc-sections --emit-relocs -T "$here/init.ld" -o "$build/init.elf" \
	"$build/init-start.o" "$build/init-c-code.o"
if ! "${cross}readelf" -rW "$build/init.elf" | grep -q 'R_C33_H'; then
	echo "linked init does not exercise C33 absolute relocations" >&2
	exit 1
fi
python3 "$here/make-flat.py" "$build/init.elf" "$build/init"
chmod 755 "$build/init"

"${cross}as" -mc33pe "$here/child.S" -o "$build/child-start.o"
"${cross}gcc" -mc33pe -Os -ffreestanding -fno-builtin \
	-medda32 \
	-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -c "$here/child.c" -o "$build/child-c.o"
"${cross}objcopy" --remove-section=.debug_frame "$build/child-c.o" \
	"$build/child-c-code.o"
"${cross}ld" --gc-sections --emit-relocs -T "$here/init.ld" \
	-o "$build/child.elf" "$build/child-start.o" "$build/child-c-code.o"
python3 "$here/make-flat.py" "$build/child.elf" "$build/child"
chmod 755 "$build/child"

printf 'dir /dev 0755 0 0\nnod /dev/console 0600 0 0 c 5 1\nfile /init %s/init 0755 0 0\nfile /child %s/child 0755 0 0\n' \
	"$build" "$build" >"$manifest"
