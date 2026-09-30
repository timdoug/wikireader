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
#   ./rebuild.sh [workdir [libstdc++|libatomic|libgcc-shared]]
#
# C33_TARGET=c33-linux-uclibc builds the no-MMU Linux compiler instead, in
# its own build directory and into the same prefix, for C and C++.  It needs
# that triplet's binutils and a sysroot holding the kernel and uClibc-ng
# headers (linux/toolchain.sh does all three).  libstdc++ links against the
# C library, so it is left out of that build and made afterwards, once
# uClibc-ng is installed, by the libstdc++ step. libatomic is also built
# after libc, with its own step, using GCC's generic POSIX implementation.

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
	# Preserving the timestamp of an identical destination matters here:
	# c33.h is included through tm.h by almost every GCC source file, so a
	# blind cp turns a one-file backend edit into a full compiler rebuild.
	if [ ! -f "${SRC}/${f}" ] || ! cmp -s "${HERE}/files/${f}" "${SRC}/${f}"; then
		cp "${HERE}/files/${f}" "${SRC}/${f}"
	fi
done

echo "==> registering the c33 target"
python3 "${HERE}/tools/gcc-glue.py" "${SRC}"

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
else
	BUILD="${SRC}/build-${TARGET}"
	# The one multilib is chosen by c33/linux.h; libgcc is built against
	# the sysroot's uClibc-ng headers, and threads are uClibc-ng's pthreads.
	TARGET_CONFIG="--enable-languages=c,c++ \
		--with-sysroot=${PREFIX}/${TARGET}/sysroot --disable-multilib \
		--enable-threads=posix --disable-libstdcxx-pch"
fi
CONFIG_ARGS="--target=${TARGET} --prefix=${PREFIX} ${TARGET_CONFIG} \
	--enable-initfini-array \
	--disable-libssp --disable-libquadmath --disable-libatomic \
	--disable-libgomp --disable-nls --disable-shared ${CONFIG_MATH}"
CONFIG_ARGS=$(echo ${CONFIG_ARGS})

N=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

build_shared_libgcc() {
	[ "${TARGET}" = c33-linux-uclibc ] || exit 1
	# The compiler bootstrap has only libc headers. Build its shared
	# runtime after libc is installed, using libgcc's standard shared rules.
	# Keep the compiler and libstdc++ bootstrap configured for static runtimes.
	# The bootstrap archive includes EH objects; make does not track that
	# enable_shared=yes changes its membership. Force the archive split.
	rm -f "${BUILD}/${TARGET}/libgcc/libgcc.a" \
		"${BUILD}/${TARGET}/libgcc/libgcc_eh.a"
	make -C "${BUILD}/${TARGET}/libgcc" -j"${N}" enable_shared=yes
	make -C "${BUILD}/${TARGET}/libgcc" enable_shared=yes install
	test -f "${PREFIX}/${TARGET}/lib/libgcc_s.so.1"
	python3 "${HERE}/tools/check-runtime.py" "${PREFIX}"
	if [ -d "${PREFIX}/${TARGET}/sysroot/lib" ]; then
		cp "${PREFIX}/${TARGET}/lib/libgcc_s.so.1" \
			"${PREFIX}/${TARGET}/sysroot/lib/"
	fi
}

if [ "${STEP}" = libgcc-shared ]; then
	build_shared_libgcc
	exit 0
fi

if [ "${STEP}" = libatomic ]; then
	[ "${TARGET}" = c33-linux-uclibc ] || {
		echo "libatomic needs the Linux compiler and installed libc" >&2
		exit 1
	}
	# Like libstdc++, this runtime needs the installed C library. Build it
	# separately so the compiler/header bootstrap does not depend on libc.
	# GCC's generic POSIX implementation uses pthread locks for operations
	# wider than the backend's inline atomics. Keep the archive static, as
	# libstdc++ is, and let --gc-sections keep just each program's operations.
	ATOMIC_BUILD="${SRC}/build-libatomic-${TARGET}"
	rm -rf "${ATOMIC_BUILD}"
	mkdir -p "${ATOMIC_BUILD}/gcc"
	cd "${ATOMIC_BUILD}"
	"${SRC}/libatomic/configure" --build="$("${SRC}/config.guess")" \
		--host="${TARGET}" --target="${TARGET}" --prefix="${PREFIX}" \
		--libdir="${PREFIX}/${TARGET}/lib" --disable-multilib \
		--disable-shared --enable-static \
		CC="${PREFIX}/bin/${TARGET}-gcc" \
		CFLAGS="-Os -ffunction-sections -fdata-sections"
	# GCC 16 also installs a copy into its compiler build directory. This
	# standalone build keeps that temporary copy here instead.
	make -j"${N}" gcc_objdir="${ATOMIC_BUILD}/gcc"
	make install
	test -f "${PREFIX}/${TARGET}/lib/libatomic.a"
	echo "==> DONE: ${PREFIX}/${TARGET}/lib/libatomic.a"
	exit 0
fi

if [ "${STEP}" = libstdc++ ]; then
	cd "${BUILD}"
	# Rebuilt from scratch every time, for the reason libgcc is below: it
	# was configured against the C library, which may have changed.
	echo "==> rebuilding libstdc++ from scratch"
	rm -rf "${TARGET}/libstdc++-v3"
	rm -f configure-target-libstdc++-v3 all-target-libstdc++-v3 \
		install-target-libstdc++-v3
	make -j"${N}" all-target-libstdc++-v3
	make install-target-libstdc++-v3
	# The top-level static bootstrap configuration rebuilds/reinstalls its
	# combined libgcc while building libstdc++. Restore the runtime split
	# before the installed compiler links any applications.
	if [ "${TARGET}" = c33-linux-uclibc ]; then
		build_shared_libgcc
	fi
	echo "==> DONE: ${PREFIX}/${TARGET}/lib/libstdc++.a"
	exit 0
fi

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
make install-gcc

# Force libgcc to be rebuilt rather than trusting the stamps.  "make install"
# copies the old archive with a fresh timestamp, so a stale libgcc.a looks
# perfectly current.
echo "==> rebuilding libgcc from scratch"
rm -rf "${TARGET}/libgcc" "${TARGET}/c33pe" "${TARGET}/c33adv"
rm -f configure-target-libgcc all-target-libgcc install-target-libgcc
make -j"${N}" all-target-libgcc
make install-target-libgcc

echo "==> DONE: ${PREFIX}/bin/${TARGET}-gcc"
