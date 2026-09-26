#!/bin/sh
# Build the Linux-hosted C33 cross toolchains without touching the macOS
# build, both into one prefix:
#
#   c33-epson-elf-     bare metal, for the kernel;
#   c33-linux-uclibc-  no-MMU Linux userspace: PE core, -msep-data and
#                      -mlong-calls by default, static uClibc-ng, and an ld
#                      that writes bFLT when given -elf2flt.
#
# The userspace compiler is configured against a sysroot holding the kernel's
# UAPI headers and uClibc-ng's headers, so make -C linux fetch has to come
# first; make -C linux libc then puts the library itself in the sysroot.
#
#   toolchain.sh [all|elf|linux]
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

if [ "${which}" != linux ]; then
	"${root}/host-tools/toolchain-c33/binutils/build.sh" "${toolwork}"
	"${root}/host-tools/toolchain-c33/gcc/rebuild.sh" "${toolwork}"
	"${prefix}/bin/c33-epson-elf-gcc" --version | head -1
fi
[ "${which}" != elf ] || exit 0

C33_TARGET=${linux_target} C33_BINUTILS_CONFIGURE="--with-sysroot=${sysroot}" \
	"${root}/host-tools/toolchain-c33/binutils/build.sh" "${toolwork}"

# The elf2flt convention: the real linkers move aside to *.real and the
# wrapper takes their names, in bin/ and in the target's tooldir.
for ld in "${prefix}/bin/${linux_target}-ld" "${prefix}/bin/${linux_target}-ld.bfd" \
	"${prefix}/${linux_target}/bin/ld" "${prefix}/${linux_target}/bin/ld.bfd"
do
	if [ -f "${ld}" ] && head -c 4 "${ld}" | grep -q ELF; then
		mv -f "${ld}" "${ld}.real"
	fi
	install -m 755 "${here}/elf2flt/ld-elf2flt" "${ld}"
done
mkdir -p "${prefix}/${linux_target}/lib"
install -m 644 "${here}/uclibc/static-flat.ld" \
	"${prefix}/${linux_target}/lib/elf2flt.ld"
install -m 755 "${here}/initramfs/make-flat.py" \
	"${prefix}/${linux_target}/lib/make-flat.py"

# GCC's libgcc is built against the C library's headers.
"${here}/uclibc/build.sh" headers
C33_TARGET=${linux_target} "${root}/host-tools/toolchain-c33/gcc/rebuild.sh" "${toolwork}"
"${prefix}/bin/${linux_target}-gcc" --version | head -1
