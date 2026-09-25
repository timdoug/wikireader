#!/bin/sh
set -eu

if [ "$(uname -s)" != Linux ]; then
	echo "console/build.sh must run inside the wr-linux VM" >&2
	exit 1
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
guest_root=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
build_dir=${WR_CONSOLE_BUILD:-$guest_root/console-build}
tool_dir=${C33_TOOLCHAIN_WORK:-$guest_root/toolchain}
cross=$tool_dir/install/bin/c33-epson-elf-
cc=$root/linux/busybox/c33-uclibc-gcc.sh

mkdir -p "$build_dir" "$root/linux/artifacts"
"$cc" -Os -Wall -Wextra -Werror -fno-unwind-tables \
	-fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections \
	"$root/linux/console/wr-console.c" -o "$build_dir/wr-console.elf"

undefined=$("${cross}nm" -u "$build_dir/wr-console.elf" |
	awk '$1 !~ /^[wWvV]$/')
if [ -n "$undefined" ]; then
	echo "userspace console has undefined symbols:" >&2
	echo "$undefined" >&2
	exit 1
fi
python3 "$root/linux/initramfs/make-flat.py" --shared-text \
	"$build_dir/wr-console.elf" "$build_dir/wr-console"
chmod 755 "$build_dir/wr-console"
cp "$build_dir/wr-console" "$root/linux/artifacts/wr-console"

"${cross}size" "$build_dir/wr-console.elf"
echo "WikiReader userspace console installed at linux/artifacts/wr-console"
