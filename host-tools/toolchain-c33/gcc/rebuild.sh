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
#   ./rebuild.sh [workdir [compiler|final]]
#
# C33_TARGET=c33-linux-uclibc builds the no-MMU Linux compiler instead, into
# the same prefix, in the two passes a cross toolchain takes.  The first
# ("compiler", in build-c33-linux-uclibc-stage1) is C only, without threads
# or shared runtimes, against a sysroot holding the kernel and uClibc-ng
# headers: enough to build uClibc-ng.  The final pass ("final", in
# build-c33-linux-uclibc), once uClibc-ng is installed, builds C and C++ and
# the runtimes with their shared libraries: libgcc_s, libstdc++ and
# libatomic.  linux/toolchain.sh and linux/Makefile run them in order.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-${C33_TOOLCHAIN_WORK:-${HERE}/../work}}
PREFIX="${WORK}/install"
TARGET=${C33_TARGET:-c33-epson-elf}
STEP=${2:-compiler}
SRC="${WORK}/gcc-16.2.0"
TARBALL=gcc-16.2.0.tar.xz

# The target assembler and linker come from the same prefix.
export PATH="${PREFIX}/bin:${PATH}"
if [ ! -x "${PREFIX}/bin/${TARGET}-as" ]; then
	echo "no ${TARGET}-as in ${PREFIX}/bin -- run binutils/build.sh first" >&2
	exit 1
fi

mkdir -p "${WORK}"
cd "${WORK}"

"${HERE}/../../fetch-verified.sh" \
	"https://ftp.gnu.org/gnu/gcc/gcc-16.2.0/${TARBALL}" "${TARBALL}" \
	e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e

# The tarball, files/, gcc-glue.py and patches/ (applied in number order
# across fixes/ and port/, patches/README.md) are the source; the tree is
# made from them, rewriting only files whose contents change.
"${HERE}/../tools/prepare-source.sh" "${SRC}" "${WORK}/${TARBALL}" \
	"${HERE}/files" "${HERE}/tools/gcc-glue.py" "${HERE}/patches"

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

if [ "${TARGET}" = c33-epson-elf ]; then
	BUILD="${SRC}/build"
	# Bare metal: C only, no C library headers, newlib's type conventions.
	TARGET_CONFIG="--enable-languages=c --without-headers --with-newlib \
		--disable-threads"
	RUNTIMES="--disable-libatomic"
	SHARED=--disable-shared
elif [ "${STEP}" = final ]; then
	BUILD="${SRC}/build-${TARGET}"
	# The one multilib is chosen by c33/linux.h; the runtimes are built
	# against the installed uClibc-ng, and threads are its pthreads.
	TARGET_CONFIG="--enable-languages=c,c++ \
		--with-sysroot=${PREFIX}/${TARGET}/sysroot --disable-multilib \
		--enable-threads=posix --disable-libstdcxx-pch"
	RUNTIMES=
	SHARED=--enable-shared
else
	BUILD="${SRC}/build-${TARGET}-stage1"
	# Only for building uClibc-ng, whose headers the sysroot holds.
	TARGET_CONFIG="--enable-languages=c \
		--with-sysroot=${PREFIX}/${TARGET}/sysroot --disable-multilib \
		--disable-threads"
	RUNTIMES="--disable-libatomic"
	SHARED=--disable-shared
fi
CONFIG_ARGS="--target=${TARGET} --prefix=${PREFIX} ${TARGET_CONFIG} \
	--enable-initfini-array \
	--disable-libssp --disable-libquadmath ${RUNTIMES} \
	--disable-libgomp --disable-nls ${SHARED} ${CONFIG_MATH}"
CONFIG_ARGS=$(echo ${CONFIG_ARGS})

N=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}

# A build directory configured with other arguments is started afresh.
mkdir -p "${BUILD}"
cd "${BUILD}"
if [ ! -f Makefile ] || [ "$(cat .c33-configure-args 2>/dev/null)" != "${CONFIG_ARGS}" ]; then
	echo "==> configuring"
	cd "${SRC}"
	rm -rf "${BUILD}"
	mkdir -p "${BUILD}"
	cd "${BUILD}"
	../configure ${CONFIG_ARGS}
	echo "${CONFIG_ARGS}" > .c33-configure-args
fi

# multilib.h is stamped against the Makefile, not the t-c33 fragment that
# lists the multilibs, so a new multilib would otherwise go unnoticed.  It
# is regenerated with move-if-change, so this rebuilds nothing when the
# list is unchanged.
rm -f gcc/s-mlib

echo "==> building cc1"
make -j"${N}" all-gcc
make install-strip-gcc

# Force the runtimes to be rebuilt rather than trusting the stamps.  "make
# install" copies the old archive with a fresh timestamp, so a stale libgcc.a
# looks perfectly current; and the final pass's are configured against the C
# library, which may have changed.
LIBS=libgcc
[ "${STEP}" != final ] || LIBS="libgcc libstdc++-v3 libatomic"
echo "==> rebuilding ${LIBS} from scratch"
rm -rf "${TARGET}/c33pe" "${TARGET}/c33adv"
for lib in ${LIBS}; do
	rm -rf "${TARGET:?}/${lib}"
	rm -f "configure-target-${lib}" "all-target-${lib}" "install-target-${lib}"
done
for lib in ${LIBS}; do
	make -j"${N}" "all-target-${lib}"
done
for lib in ${LIBS}; do
	make "install-strip-target-${lib}"
done

if [ "${STEP}" = final ]; then
	# The shared runtimes go in the sysroot's lib/ as well, beside the C
	# library, where Buildroot and the card's image take them from.
	for lib in libgcc_s.so.1 'libstdc++.so.*' 'libatomic.so.*'; do
		cp -a "${PREFIX}/${TARGET}/lib/"${lib} "${PREFIX}/${TARGET}/sysroot/lib/"
	done
	rm -f "${PREFIX}/${TARGET}/sysroot/lib/"libstdc++.so.*-gdb.py
	python3 "${HERE}/tools/check-runtime.py" "${PREFIX}"
	# The first pass only bootstraps the C library; as cross builds do, it
	# goes once the final compiler is installed.
	rm -rf "${SRC}/build-${TARGET}-stage1"
fi

echo "==> DONE: ${PREFIX}/bin/${TARGET}-gcc"
