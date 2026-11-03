#!/bin/sh
# Build GDB for the Seiko Epson C33 (S1C33) from a pristine upstream tarball
# plus this directory's ported sources, to run on this host.
#
#   ./build.sh [workdir]
#
# Produces c33-linux-musl-gdb in $workdir/install: it reads C33 programs,
# their core files and FDPIC shared libraries.  C33_TARGET selects another
# triplet, such as c33-epson-elf for bare metal.  GDB's BFD and opcodes are
# binutils' C33 ones (../binutils), which glue.py copies in.

set -e

GDB_VERSION=18.1
TARGET=${C33_TARGET:-c33-linux-musl}

HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS="${HERE}/../tools"
WORK=${1:-${HERE}/../work}
TARBALL="gdb-${GDB_VERSION}.tar.xz"
URL="https://ftp.gnu.org/gnu/gdb/${TARBALL}"
SHA256=cd9fc3fe2b47743840e42c1592d3d87f8302eb18639c0b8b4ba0898002e2348f
SRC="${WORK}/gdb-${GDB_VERSION}"

mkdir -p "${WORK}"
cd "${WORK}"

"${HERE}/../../fetch-verified.sh" "${URL}" "${TARBALL}" "${SHA256}"

PREPARE_EXTRA="${HERE}/../binutils/files:${TOOLS}/glue.py" \
	"${TOOLS}/prepare-source.sh" "${SRC}" "${WORK}/${TARBALL}" \
	"${HERE}/files" "${HERE}/glue.py"

# GDB needs GMP and MPFR; use Homebrew's if present.
CONFIG_MATH=
for lib in gmp mpfr; do
	prefix=$(brew --prefix "${lib}" 2>/dev/null || true)
	if [ -n "${prefix}" ] && [ -d "${prefix}" ]; then
		CONFIG_MATH="${CONFIG_MATH} --with-${lib}=${prefix}"
	fi
done

echo "==> configuring"
BUILD="${SRC}/build-${TARGET}"
mkdir -p "${BUILD}"
cd "${BUILD}"
[ -f config.status ] || ../configure \
	--target="${TARGET}" \
	--prefix="${WORK}/install" \
	--disable-nls --disable-werror --disable-sim --disable-gdbserver \
	--disable-binutils --disable-ld --disable-gas --disable-gprof \
	--disable-gprofng --without-python --without-guile \
	--with-system-zlib ${CONFIG_MATH}

echo "==> building"
make -j"${JOBS:-4}" all-gdb
make install-gdb

echo
echo "Done.  ${TARGET}-gdb is in ${WORK}/install/bin"
