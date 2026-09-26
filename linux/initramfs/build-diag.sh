#!/bin/sh
# Build the freestanding diagnostics that rcS runs under wr.selftest: two
# PID-1-style programs and a child, linked with no C library by the
# bare-metal compiler, with absolute C33 relocations in their text.
#
#   build-diag.sh CROSS_PREFIX BUILD_DIR
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 CROSS_PREFIX BUILD_DIR" >&2
	exit 2
fi

cross=$1
build=$2
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

mkdir -p "$build"
"${cross}as" -mc33pe "$here/init.S" -o "$build/diag-start.o"
"${cross}gcc" -mc33pe -Os -ffreestanding -fno-builtin \
	-medda32 \
	-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -c "$here/init.c" -o "$build/diag-init-c.o"
"${cross}objcopy" --remove-section=.debug_frame "$build/diag-init-c.o" \
	"$build/diag-init-code.o"
"${cross}ld" --gc-sections --emit-relocs -T "$here/init.ld" \
	-o "$build/diag-init.elf" "$build/diag-start.o" "$build/diag-init-code.o"
if ! "${cross}readelf" -rW "$build/diag-init.elf" | grep -q 'R_C33_H'; then
	echo "linked diagnostic init does not exercise C33 absolute relocations" >&2
	exit 1
fi
python3 "$here/make-flat.py" "$build/diag-init.elf" "$build/diag-init"
chmod 755 "$build/diag-init"

"${cross}gcc" -mc33pe -Os -ffreestanding -fno-builtin -DDIAG_ONESHOT \
	-medda32 \
	-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -c "$here/init.c" -o "$build/diag-test-c.o"
"${cross}objcopy" --remove-section=.debug_frame "$build/diag-test-c.o" \
	"$build/diag-test-code.o"
"${cross}ld" --gc-sections --emit-relocs -T "$here/init.ld" \
	-o "$build/diag-test.elf" "$build/diag-start.o" "$build/diag-test-code.o"
python3 "$here/make-flat.py" "$build/diag-test.elf" "$build/diag-test"
chmod 755 "$build/diag-test"

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
