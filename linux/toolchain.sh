#!/bin/sh
# Build a Linux-hosted C33 cross toolchain without touching the macOS build.
set -eu

if [ "$(uname -s)" != Linux ]; then
	echo "toolchain.sh must run inside the wr-linux VM" >&2
	exit 1
fi

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "${here}/.." && pwd)
work=${WR_LINUX_WORK:-${HOME}/wr-linux}
toolwork=${work}/toolchain

"${root}/host-tools/toolchain-c33/binutils/build.sh" "${toolwork}"
"${root}/host-tools/toolchain-c33/gcc/rebuild.sh" "${toolwork}"

"${toolwork}/install/bin/c33-epson-elf-gcc" --version | head -1
