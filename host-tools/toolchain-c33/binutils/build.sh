#!/bin/sh
# Build a modern binutils for the Seiko Epson C33 (S1C33) from a pristine
# upstream tarball plus this directory's ported sources.
#
#   ./build.sh [workdir]
#
# Produces c33-epson-elf-{objdump,readelf,as,ld,...} in $workdir/install.
# C33_TARGET selects another triplet, such as c33-linux-musl, built in its
# own directory beside the default one; C33_BINUTILS_CONFIGURE adds
# configure options for it.

set -e

BINUTILS_VERSION=2.47
TARGET=${C33_TARGET:-c33-epson-elf}

HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS="${HERE}/../tools"
WORK=${1:-${HERE}/work}
TARBALL="binutils-${BINUTILS_VERSION}.tar.xz"
URL="https://ftp.gnu.org/gnu/binutils/${TARBALL}"
SHA256=154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff
SRC="${WORK}/binutils-${BINUTILS_VERSION}"

mkdir -p "${WORK}"
cd "${WORK}"

"${HERE}/../../fetch-verified.sh" "${URL}" "${TARBALL}" "${SHA256}"

# The tarball, files/ and glue.py are the source; the tree is made from them.
"${TOOLS}/prepare-source.sh" "${SRC}" "${WORK}/${TARBALL}" "${HERE}/files" \
	"${TOOLS}/glue.py"

echo "==> configuring"
BUILD="${SRC}/build"
[ "${TARGET}" = c33-epson-elf ] || BUILD="${SRC}/build-${TARGET}"
mkdir -p "${BUILD}"
cd "${BUILD}"
../configure \
	--target="${TARGET}" \
	--prefix="${WORK}/install" \
	--disable-nls --disable-werror --disable-gdb --disable-sim \
	--disable-gprofng ${C33_BINUTILS_CONFIGURE:-}

echo "==> building"
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
make install-strip

echo
echo "Done.  Tools are in ${WORK}/install/bin"
