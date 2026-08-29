#!/bin/sh
# Build the (in-progress) C33 GCC backend from a pristine upstream tarball
# plus this directory's sources.
#
#   ./build.sh [workdir]
#
# NB this currently builds a compiler that works for simple functions but
# ICEs on calls and stack arguments -- see README.md.  It is here so the
# edit/build/test loop is reproducible, not because the result is usable.
#
# Build binutils first: the GCC build needs c33-epson-elf-as on PATH.

set -e

GCC_VERSION=16.2.0
TARGET=c33-epson-elf

HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS="${HERE}/tools"
WORK=${1:-${HERE}/work}
TARBALL="gcc-${GCC_VERSION}.tar.xz"
URL="https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/${TARBALL}"
SRC="${WORK}/gcc-${GCC_VERSION}"

BINUTILS_BIN="${HERE}/../binutils/work/install/bin"
if [ ! -x "${BINUTILS_BIN}/${TARGET}-as" ]; then
	echo "warning: ${TARGET}-as not found in ${BINUTILS_BIN}" >&2
	echo "         run ../binutils/build.sh first" >&2
fi
PATH="${BINUTILS_BIN}:${PATH}"
export PATH

mkdir -p "${WORK}"
cd "${WORK}"

if [ ! -f "${TARBALL}" ]; then
	echo "==> downloading ${TARBALL} (about 100 MB)"
	curl -fsSL -o "${TARBALL}" "${URL}"
fi

if [ ! -d "${SRC}" ]; then
	echo "==> extracting"
	tar xf "${TARBALL}"
fi

echo "==> installing C33 backend sources"
for f in $(cd "${HERE}/files" && find . -type f); do
	mkdir -p "${SRC}/$(dirname "${f}")"
	cp "${HERE}/files/${f}" "${SRC}/${f}"
done

echo "==> registering the c33 target"
python3 "${TOOLS}/gcc-glue.py" "${SRC}"

echo "==> configuring"
mkdir -p "${SRC}/build"
cd "${SRC}/build"

# GMP/MPFR/MPC: use Homebrew's if present, otherwise assume system paths.
CONFIG_MATH=""
for lib in gmp mpfr libmpc; do
	prefix=$(brew --prefix "${lib}" 2>/dev/null || true)
	if [ -n "${prefix}" ]; then
		case "${lib}" in
		gmp)    CONFIG_MATH="${CONFIG_MATH} --with-gmp=${prefix}" ;;
		mpfr)   CONFIG_MATH="${CONFIG_MATH} --with-mpfr=${prefix}" ;;
		libmpc) CONFIG_MATH="${CONFIG_MATH} --with-mpc=${prefix}" ;;
		esac
	fi
done

../configure \
	--target="${TARGET}" \
	--prefix="${WORK}/install" \
	--enable-languages=c \
	--without-headers --with-newlib \
	--enable-initfini-array \
	--disable-libssp --disable-libquadmath --disable-libatomic \
	--disable-libgomp --disable-nls --disable-shared --disable-threads \
	${CONFIG_MATH}

echo "==> building cc1"
# all-gcc only: libgcc needs a working compiler, which we do not have yet.
make all-gcc -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo
echo "Done.  Try it with:"
echo "  cd ${SRC}/build/gcc && ./xgcc -B. -S -O2 -o - ${HERE}/probes/args.c"
