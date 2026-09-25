#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
guest_root=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
tool_dir=${C33_TOOLCHAIN_WORK:-$guest_root/toolchain}
install_dir=${WR_UCLIBC_INSTALL:-$guest_root/uclibc-install}
headers_dir=${WR_LINUX_HEADERS:-$guest_root/linux-headers}
gcc=$tool_dir/install/bin/c33-epson-elf-gcc
sysroot=$install_dir/usr/c33-linux-uclibc
lib=$sysroot/usr/lib
link=yes

for arg in "$@"; do
	case $arg in
		-c|-E|-S|-r)
			link=no
			;;
	esac
done

common="-mc33pe -msep-data -mlong-calls -fno-stack-protector"
if [ "$link" = no ]; then
	exec "$gcc" $common -isystem "$sysroot/usr/include" \
		-isystem "$headers_dir/include" "$@"
fi

gcc_lib=$($gcc -mc33pe -msep-data -print-libgcc-file-name)
exec "$gcc" $common -isystem "$sysroot/usr/include" \
	-isystem "$headers_dir/include" -nostdlib -static \
	-Wl,-T,"$root/linux/uclibc/static-flat.ld" -Wl,--emit-relocs \
	-L"$lib" "$lib/crt1.o" "$lib/crti.o" "$@" \
	-Wl,--start-group "$lib/libc.a" "$gcc_lib" -Wl,--end-group \
	"$lib/crtn.o"
