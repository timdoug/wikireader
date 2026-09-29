#!/bin/sh
# Build a program with no C library as a static FDPIC ELF: its own entry
# point and syscall veneers in START.S, the rest in C.  The entry relocates
# the program with the C library's crtreloc.o, as crt1 does.
#
#   build-freestanding.sh CROSS_PREFIX OUTPUT START.S C_FILE [CFLAGS...]
#
# CROSS_PREFIX is the Linux compiler's, e.g. .../bin/c33-linux-uclibc-.
set -eu

if [ "$#" -lt 4 ]; then
	echo "usage: $0 CROSS_PREFIX OUTPUT START.S C_FILE [CFLAGS...]" >&2
	exit 2
fi

cross=$1
out=$2
start=$3
source=$4
shift 4

"${cross}gcc" -Os -ffreestanding -fno-builtin -fno-stack-protector \
	-fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections "$@" \
	-static -nostdlib -s -Wl,--gc-sections -o "$out" "$start" "$source" \
	"$("${cross}gcc" -print-file-name=crtreloc.o)" -lgcc
