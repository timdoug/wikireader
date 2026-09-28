#!/bin/sh
# Build the Linux-hosted C33 cross toolchains without touching the macOS
# build, both into one prefix:
#
#   c33-epson-elf-     bare metal, for the kernel;
#   c33-linux-uclibc-  no-MMU Linux userspace: PE core, FDPIC ELF with
#                      shared libraries, and uClibc-ng.
#
# The userspace compiler is configured against a sysroot holding the kernel's
# UAPI headers and uClibc-ng's headers, so make -C linux fetch has to come
# first; make -C linux libc then puts the library itself in the sysroot and
# builds libstdc++ against it, with the libstdc++ step here.
#
#   toolchain.sh [all|elf|linux|libstdc++]
set -eu

if [ "$(uname -s)" != Linux ]; then
	echo "toolchain.sh must run inside the wr-linux VM" >&2
	exit 1
fi

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "${here}/.." && pwd)
work=${WR_LINUX_WORK:-${HOME}/wr-linux}
toolwork=${work}/toolchain
prefix=${toolwork}/install
linux_target=c33-linux-uclibc
sysroot=${prefix}/${linux_target}/sysroot
which=${1:-all}

if [ "${which}" = libstdc++ ]; then
	C33_TARGET=${linux_target} \
		"${root}/host-tools/toolchain-c33/gcc/rebuild.sh" "${toolwork}" libstdc++
	# app-test.py runs this from the card.
	mkdir -p "${root}/linux/artifacts"
	"${prefix}/bin/${linux_target}-g++" -Os -Wall -Werror -pthread \
		"${here}/uclibc/cxx-test.cc" -o "${root}/linux/artifacts/cxx-test" \
		-Wl,-elf2flt=--shared-text
	exit 0
fi

if [ "${which}" != linux ]; then
	"${root}/host-tools/toolchain-c33/binutils/build.sh" "${toolwork}"
	"${root}/host-tools/toolchain-c33/gcc/rebuild.sh" "${toolwork}"
	"${prefix}/bin/c33-epson-elf-gcc" --version | head -1
fi
[ "${which}" != elf ] || exit 0

C33_TARGET=${linux_target} C33_BINUTILS_CONFIGURE="--with-sysroot=${sysroot}" \
	"${root}/host-tools/toolchain-c33/binutils/build.sh" "${toolwork}"

# Earlier toolchains put a bFLT-writing wrapper in the linker's place and
# the linker beside it as *.real.  Userspace is FDPIC ELF now, which the
# linker writes itself.
for ld in "${prefix}/bin/${linux_target}-ld" "${prefix}/bin/${linux_target}-ld.bfd" \
	"${prefix}/${linux_target}/bin/ld" "${prefix}/${linux_target}/bin/ld.bfd"
do
	rm -f "${ld}.real"
done

# GCC's libgcc is built against the C library's headers.
"${here}/uclibc/build.sh" headers
C33_TARGET=${linux_target} "${root}/host-tools/toolchain-c33/gcc/rebuild.sh" "${toolwork}"
"${prefix}/bin/${linux_target}-gcc" --version | head -1
