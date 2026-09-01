#!/bin/sh
# Build the C33 GCC backend from a pristine upstream tarball plus this
# directory's sources.
#
#   ./build.sh [workdir]
#
# This stops after all-gcc for a quick compiler edit/build/test loop. Use
# rebuild.sh for an installed compiler with freshly rebuilt libgcc multilibs.
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

echo "==> applying GCC correctness patches"
for patchfile in "${HERE}"/patches/*.patch; do
	if patch -d "${SRC}" -p1 --forward --dry-run --silent < "${patchfile}"; then
		patch -d "${SRC}" -p1 --forward --silent < "${patchfile}"
	elif patch -d "${SRC}" -p1 --reverse --dry-run --silent < "${patchfile}"; then
		# Already applied in this persistent work tree.
		:
	else
		echo "cannot apply ${patchfile}" >&2
		exit 1
	fi
done

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
