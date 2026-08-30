#!/bin/bash
# Run the upstream GCC driver against the C33 board.
#
#   run-dejagnu.sh execute [runtest selectors/options]
#   run-dejagnu.sh compile [runtest selectors/options]
#   run-dejagnu.sh gcc.dg [runtest selectors/options]
#   run-dejagnu.sh all [runtest selectors/options]
#
# Examples:
#   ./run-dejagnu.sh execute execute.exp=pr61725.c
#   ./run-dejagnu.sh compile compile.exp=20000112-1.c
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
TESTS=$(cd "${HERE}/.." && pwd)
TOOLCHAIN=$(cd "${TESTS}/.." && pwd)
REPO=$(cd "${TOOLCHAIN}/../.." && pwd)
TCROOT=${TCROOT:-${TOOLCHAIN}/work}

export C33_GCC=${GCC:-${TCROOT}/install/bin/c33-epson-elf-gcc}
C33_TOOL_BINDIR=${TOOL_BINDIR:-$(dirname "${C33_GCC}")}
PATH="${C33_TOOL_BINDIR}:${PATH}"
export PATH
export C33_GCOV=${GCOV:-${C33_TOOL_BINDIR}/c33-epson-elf-gcov}
export C33_EMU=${EMU:-${REPO}/emulator/wremu}
export C33_RT=${RT:-${TESTS}/runtime}
export C33_LIBC_INC=${LIBC_INC:-${REPO}/samo-lib/mini-libc/include}
C33_LIBC_DIR=${LIBC:-${REPO}/samo-lib/mini-libc}
export C33_LIBC=${C33_LIBC_ARCHIVE:-${C33_LIBC_DIR}/lib/libc.a}
export C33_LIBGCC=${LIBGCC:-$("${C33_GCC}" -mc33pe -mno-long-calls -print-libgcc-file-name)}
export C33_RUNTIME_DIR=${RUNTIME_DIR:-${HERE}/work/runtime}
export C33_LIMIT=${LIMIT:-3200000000}
export C33_HOST_TIMEOUT=${HOST_TIMEOUT:-300}
export C33_BUILD_SITE=${GCC_BUILD_SITE:-${TCROOT}/gcc-16.2.0/build/gcc/site.exp}

GCC_TESTSUITE=${SRC:-${TCROOT}/gcc-16.2.0/gcc/testsuite}
OBJDIR=${WORK:-${HERE}/work/results}
GRIFO=${GRIFO:-${REPO}/samo-lib/grifo}
export DEJAGNU=${HERE}/site.exp

for path in "${C33_GCC}" "${C33_GCOV}" "${C33_EMU}" "${C33_LIBC}" "${C33_LIBGCC}" \
    "${C33_BUILD_SITE}" \
    "${C33_RT}/crt0.s" "${C33_RT}/setjmp.s" "${C33_RT}/runtime.c" \
    "${C33_RT}/test.lds" "${GRIFO}/src/memory.c"; do
	if [ ! -e "${path}" ]; then
		echo "missing prerequisite: ${path}" >&2
		exit 1
	fi
done
if ! command -v runtest >/dev/null 2>&1; then
	echo "runtest is not installed (on macOS: brew install deja-gnu)" >&2
	exit 1
fi

mkdir -p "${C33_RUNTIME_DIR}" "${OBJDIR}"
# Drivers which build multi-source executables reuse objdir in output paths.
# Make it absolute before changing directory below so a relative WORK value
# cannot turn those paths into an unintended nested directory.
OBJDIR=$(cd "${OBJDIR}" && pwd)
export C33_OBJDIR=${OBJDIR}

build_runtime() {
	"${C33_GCC}" -mc33pe -mno-long-calls -O0 -c \
		"${C33_RT}/crt0.s" -o "${C33_RUNTIME_DIR}/crt0.o"
	"${C33_GCC}" -mc33pe -mno-long-calls -O0 -c \
		"${C33_RT}/setjmp.s" -o "${C33_RUNTIME_DIR}/setjmp.o"
	"${C33_GCC}" -w -fpermissive -mc33pe -mno-long-calls \
		-I"${C33_RT}/include" -I"${C33_LIBC_INC}" \
		-I"${GRIFO}/src" -I"${GRIFO}/common" -fgnu89-inline -O0 -c \
		"${C33_RT}/runtime.c" -o "${C33_RUNTIME_DIR}/runtime.o"
	"${C33_GCC}" -w -fpermissive -mc33pe -mno-long-calls \
		-I"${C33_RT}/include" -I"${C33_LIBC_INC}" \
		-I"${GRIFO}/src" -I"${GRIFO}/common" -fgnu89-inline -O0 -c \
		"${GRIFO}/src/memory.c" -o "${C33_RUNTIME_DIR}/memory.o"
}

build_runtime

mode=${1:-execute}
shift || true
case "${mode}" in
execute)
	default_exp=gcc.c-torture/execute/execute.exp
	;;
compile)
	default_exp=gcc.c-torture/compile/compile.exp
	;;
gcc.dg)
	default_exp=gcc.dg/dg.exp
	;;
all)
	default_exp=
	;;
*)
	echo "usage: $0 {execute|compile|gcc.dg|all} [runtest selectors/options]" >&2
	exit 2
	;;
esac

if [ "$#" -eq 0 ] && [ -n "${default_exp}" ]; then
	set -- "${default_exp}"
fi

cd "${OBJDIR}"
if runtest --tool gcc --target c33-epson-elf --target_board=c33-sim \
    --build aarch64-apple-darwin --host aarch64-apple-darwin \
    --srcdir "${GCC_TESTSUITE}" --objdir "${OBJDIR}" \
    --tool_exec "${C33_GCC}" "$@"; then
	status=0
else
	status=$?
fi

exit "${status}"
