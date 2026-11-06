#!/bin/sh
# Build libgccjit for the C33, the code generator rustc_codegen_gcc drives.
#
#   ./build-gccjit.sh                  bare metal, for Grifo applications
#   C33_TARGET=c33-linux-musl ./build-gccjit.sh   Linux userspace
#
# The source is GCC 16.2 with the C33 port's patches
# (host-tools/toolchain-c33/gcc) plus gcc/patches/jit here, in a tree of its
# own under work/, so the toolchain's compiler is not rebuilt.  The target
# assembler and linker are the toolchain's; build it first.
#
# The library goes to work/install/lib/libgccjit.so (a Mach-O dylib on
# macOS under the name rustc_codegen_gcc loads), or for Linux to
# work/install-c33-linux-musl/lib/, configured as the toolchain's final
# Linux pass is, against its musl sysroot.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
TC="${HERE}/../host-tools/toolchain-c33"
TCBIN="${TC}/work/install/bin"
WORK="${HERE}/work"
TARGET=${C33_TARGET:-c33-epson-elf}
TARBALL=gcc-16.2.0.tar.xz
SRC="${WORK}/gcc-16.2.0"
if [ "${TARGET}" = c33-epson-elf ]; then
	BUILD="${SRC}/build-jit"
	PREFIX="${WORK}/install"
	# Bare metal: no C library headers, newlib's type conventions.
	TARGET_CONFIG="--without-headers --with-newlib"
	THREADS=--disable-threads
else
	BUILD="${SRC}/build-jit-${TARGET}"
	PREFIX="${WORK}/install-${TARGET}"
	TARGET_CONFIG="--with-sysroot=${TC}/work/install/${TARGET}/sysroot \
		--disable-multilib"
	THREADS=--enable-threads=posix
fi

if [ ! -x "${TCBIN}/${TARGET}-as" ]; then
	echo "no ${TARGET}-as in ${TCBIN} -- build the toolchain first" >&2
	exit 1
fi
export PATH="${TCBIN}:${PATH}"

mkdir -p "${WORK}"
[ -f "${WORK}/${TARBALL}" ] || cp "${TC}/work/${TARBALL}" "${WORK}/"
"${HERE}/../host-tools/fetch-verified.sh" \
	"https://ftp.gnu.org/gnu/gcc/gcc-16.2.0/${TARBALL}" "${WORK}/${TARBALL}" \
	e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e
"${TC}/tools/prepare-source.sh" "${SRC}" "${WORK}/${TARBALL}" \
	"${TC}/gcc/files" "${TC}/gcc/patches:${HERE}/gcc/patches"

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

CONFIG_ARGS="--target=${TARGET} --prefix=${PREFIX} \
	--enable-languages=jit --enable-host-shared --disable-bootstrap \
	--with-as=${TCBIN}/${TARGET}-as --with-ld=${TCBIN}/${TARGET}-ld \
	${TARGET_CONFIG} --enable-initfini-array \
	--disable-analyzer --disable-libssp --disable-libquadmath \
	--disable-libatomic --disable-libgomp --disable-nls \
	--disable-shared ${THREADS} ${CONFIG_MATH}"
CONFIG_ARGS=$(echo ${CONFIG_ARGS})

if [ ! -f "${BUILD}/Makefile" ] ||
   [ "$(cat "${BUILD}/.c33-configure-args" 2>/dev/null)" != "${CONFIG_ARGS}" ]; then
	rm -rf "${BUILD}"
	mkdir -p "${BUILD}"
	cd "${BUILD}"
	../configure ${CONFIG_ARGS}
	echo "${CONFIG_ARGS}" > .c33-configure-args
fi
cd "${BUILD}"
# Darwin's export list is not among the library's make dependencies.
for f in gcc/libgccjit.0.dylib; do
	[ ! -f "${f}" ] || [ ! "${SRC}/gcc/jit/libgccjit.exports" -nt "${f}" ] ||
		rm -f gcc/libgccjit*.dylib
done
make -j"${JOBS:-4}" all-gcc
make install-gcc

# rustc_codegen_gcc loads libgccjit.so by that name on every host.
cd "${PREFIX}/lib"
for f in libgccjit.0.dylib libgccjit.dylib; do
	if [ -f "${f}" ]; then
		ln -sf "${f}" libgccjit.so
		break
	fi
done
echo "==> DONE: ${PREFIX}/lib/libgccjit.so"
