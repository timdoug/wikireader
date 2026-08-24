#!/bin/sh
# Build a modern binutils for the Seiko Epson C33 (S1C33) from a pristine
# upstream tarball plus this directory's ported sources.
#
#   ./build.sh [workdir]
#
# Produces c33-epson-elf-{objdump,readelf,as,ld,...} in $workdir/install.

set -e

BINUTILS_VERSION=2.47
TARGET=c33-epson-elf

HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS="${HERE}/../tools"
WORK=${1:-${HERE}/work}
TARBALL="binutils-${BINUTILS_VERSION}.tar.xz"
URL="https://ftp.gnu.org/gnu/binutils/${TARBALL}"
SRC="${WORK}/binutils-${BINUTILS_VERSION}"

mkdir -p "${WORK}"
cd "${WORK}"

if [ ! -f "${TARBALL}" ]; then
	echo "==> downloading ${TARBALL}"
	curl -fsSL -o "${TARBALL}" "${URL}"
fi

if [ ! -d "${SRC}" ]; then
	echo "==> extracting"
	tar xf "${TARBALL}"
fi

echo "==> installing C33 sources"
for f in $(cd "${HERE}/files" && find . -type f); do
	mkdir -p "${SRC}/$(dirname "${f}")"
	cp "${HERE}/files/${f}" "${SRC}/${f}"
done

echo "==> registering the c33 target in the upstream build system"
python3 "${TOOLS}/glue.py" "${SRC}"

echo "==> configuring"
mkdir -p "${SRC}/build"
cd "${SRC}/build"
../configure \
	--target="${TARGET}" \
	--prefix="${WORK}/install" \
	--disable-nls --disable-werror --disable-gdb --disable-sim \
	--disable-gprofng --disable-libctf

echo "==> building"
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
make install

echo
echo "Done.  Tools are in ${WORK}/install/bin"
