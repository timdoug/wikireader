#!/bin/bash
# Run gcc.c-torture against the C33 backend, under the emulator.
#
#   run-torture.sh execute [-O2 -Os ...]      compile, link, run, check status
#   run-torture.sh compile [-O2 ...]          compile only; passing means no ICE
#
# A test passes if the emulator reaches the exit breakpoint with %r4 == 0.
# %r4 == 0xdead is abort(), which is how a torture test reports a wrong
# answer -- that is the miscompilation signal and the reason to run this.
#
# Tests needing a libc this runtime does not have are reported UNSUPPORTED,
# not failed; they are a gap in the harness, not in the compiler.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
RT="${HERE}/runtime"
GCC=${GCC:-/tmp/c33port/gccinstall/bin/c33-epson-elf-gcc}
EMU=${EMU:-$(cd "${HERE}/../../../emulator" && pwd)/wremu}
SRC=${SRC:-/tmp/c33port/gcc-16.2.0/gcc/testsuite/gcc.c-torture}
WORK=${WORK:-/tmp/c33torture}
LIMIT=${LIMIT:-200000000}
JOBS=${JOBS:-8}

mode=${1:-execute}; shift || true
opts=("$@"); [ ${#opts[@]} -eq 0 ] && opts=(-O0 -O1 -O2 -Os)

ARCH="-mc33pe -mno-long-calls"
# mini-libc supplies the headers and the string/memory routines the torture
# tests expect; there is no newlib for this target.
LIBC=${LIBC:-$(cd "${HERE}/../../../samo-lib/mini-libc" && pwd)}
# These tests predate C23.  Many call exit() without declaring it and use
# other constructs gcc 14 and later reject outright, no matter the -std;
# -fpermissive is the documented way back to a diagnostic.  Without it the
# harness reports the compiler's strictness as a backend failure.
COMMON="-w -fno-builtin -std=gnu17 -fpermissive ${ARCH} -I${LIBC}/include"
# -nostdlib drops libgcc too, and without it anything using long long
# division or double arithmetic fails to link rather than failing to run.
LIBGCC=$(${GCC} ${ARCH} -print-libgcc-file-name)
LIBS="${LIBC}/lib/libc.a ${LIBGCC}"

mkdir -p "${WORK}"

one() {                                 # one <mode> <opt> <file>
	local mode=$1 opt=$2 f=$3
	local b; b=$(basename "$f" .c)
	local tag="${b}.${opt#-}"
	local o="${WORK}/${tag}"

	if [ "${mode}" = compile ]; then
		if ${GCC} ${COMMON} "${opt}" -S "$f" -o "${o}.s" 2>"${o}.log"; then
			echo "PASS ${opt} ${b}"
		elif grep -qE "internal compiler error|Segmentation fault" "${o}.log"; then
			echo "ICE ${opt} ${b}"
		else
			echo "FAIL ${opt} ${b}"
		fi
		return
	fi

	if ! ${GCC} ${COMMON} "${opt}" -nostdlib -nostartfiles \
	     -T "${RT}/test.lds" "${RT}/crt0.s" "$f" ${LIBS} -o "${o}.elf" 2>"${o}.log"; then
		if grep -qE "internal compiler error|Segmentation fault" "${o}.log"; then
			echo "ICE ${opt} ${b}"
		elif grep -qE "undefined reference|cannot find" "${o}.log"; then
			echo "UNSUPPORTED ${opt} ${b}"
		else
			echo "FAIL ${opt} ${b}"
		fi
		return
	fi

	local out; out=$(${EMU} -n "${LIMIT}" -b 0x10000002 "${o}.elf" 2>&1)
	local pc r4
	pc=$(echo "${out}" | awk -F= '/^pc=/{print $2}' | awk '{print $1}')
	r4=$(echo "${out}" | awk '/^r4 /{print $2}')

	if [ "${pc}" = "10000002" ] && [ "${r4}" = "00000000" ]; then
		echo "PASS ${opt} ${b}"
	elif [ "${pc}" = "10000002" ] && [ "${r4}" = "0000dead" ]; then
		echo "ABORT ${opt} ${b}"
	elif echo "${out}" | grep -q "^fault:"; then
		echo "FAULT ${opt} ${b}  $(echo "${out}" | grep -m1 '^fault:' | cut -c1-60)"
	elif [ "${pc}" = "10000002" ]; then
		echo "EXIT${r4} ${opt} ${b}"
	else
		echo "TIMEOUT ${opt} ${b}"
	fi
}
export -f one
export WORK RT GCC EMU COMMON LIBS LIMIT

for opt in "${opts[@]}"; do
	ls "${SRC}/${mode}"/*.c | \
		xargs -P "${JOBS}" -I{} bash -c 'one "$0" "$1" "$2"' "${mode}" "${opt}" {}
done
