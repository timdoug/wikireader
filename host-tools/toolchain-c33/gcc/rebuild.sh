#!/bin/bash
# Rebuild the C33 gcc from a pristine tarball, all the way through libgcc.
#
# build.sh stops at all-gcc, which is right for bringing the backend up but
# not for testing it: after an ABI or calling-convention change a stale
# libgcc.a is silently wrong rather than broken, and that has cost real time
# more than once.  This does the whole thing and installs.
#
# Everything lives under host-tools/toolchain-c33/work, which .gitignore
# covers.  Do not put it in /tmp: macOS purges /tmp nightly and will gut the
# source tree out from under you, leaving empty directories and a build that
# fails in confusing ways.
#
#   ./rebuild.sh

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
WORK="${HERE}/../work"
PREFIX="${WORK}/install"
SRC="${WORK}/gcc-16.2.0"
TARBALL=gcc-16.2.0.tar.xz

# The target assembler and linker come from the same prefix.
export PATH="${PREFIX}/bin:${PATH}"
if [ ! -x "${PREFIX}/bin/c33-epson-elf-as" ]; then
	echo "no c33-epson-elf-as in ${PREFIX}/bin -- run binutils/build.sh first" >&2
	exit 1
fi

mkdir -p "${WORK}"
cd "${WORK}"

if [ ! -f "${TARBALL}" ]; then
	echo "==> downloading ${TARBALL} (about 100 MB)"
	curl -fsSL -o "${TARBALL}.tmp" \
		"https://ftp.gnu.org/gnu/gcc/gcc-16.2.0/${TARBALL}"
	mv "${TARBALL}.tmp" "${TARBALL}"
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
python3 "${HERE}/tools/gcc-glue.py" "${SRC}"

CONFIG_MATH=""
for lib in gmp mpfr libmpc; do
	p=$(brew --prefix "${lib}" 2>/dev/null || true)
	[ -n "${p}" ] || continue
	case "${lib}" in
	gmp)    CONFIG_MATH="${CONFIG_MATH} --with-gmp=${p}" ;;
	mpfr)   CONFIG_MATH="${CONFIG_MATH} --with-mpfr=${p}" ;;
	libmpc) CONFIG_MATH="${CONFIG_MATH} --with-mpc=${p}" ;;
	esac
done

mkdir -p "${SRC}/build"
cd "${SRC}/build"
if [ ! -f Makefile ]; then
	echo "==> configuring"
	../configure --target=c33-epson-elf --prefix="${PREFIX}" \
		--enable-languages=c --without-headers --with-newlib \
		--disable-libssp --disable-libquadmath --disable-libatomic \
		--disable-libgomp --disable-nls --disable-shared --disable-threads \
		${CONFIG_MATH}
fi

N=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

echo "==> building cc1"
make -j"${N}" all-gcc
make install-gcc

# Force libgcc to be rebuilt rather than trusting the stamps.  "make install"
# copies the old archive with a fresh timestamp, so a stale libgcc.a looks
# perfectly current -- see HANDOFF.md.
echo "==> rebuilding libgcc from scratch"
rm -rf c33-epson-elf/libgcc c33-epson-elf/c33pe c33-epson-elf/c33adv
rm -f configure-target-libgcc all-target-libgcc install-target-libgcc
make -j"${N}" all-target-libgcc
make install-target-libgcc

echo "==> DONE: ${PREFIX}/bin/c33-epson-elf-gcc"
