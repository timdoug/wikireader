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
# Instruction budget per test.  Not a timing knob -- the emulator is
# deterministic -- but a runaway detector.  It has to clear the slowest
# honest test: vla-dealloc-1 and pr43220 spend ~490M instructions at -O0
# allocating a VLA a million times over, and at 200M they looked like
# hangs.  A test that has genuinely jumped into the weeds usually trips
# the emulator's own "256 consecutive zero words" check long before this.
LIMIT=${LIMIT:-1000000000}
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
# harness reports the compiler's strictness as a backend failure.  The ones
# that actually need an older dialect ask for it themselves, in dg-options.
#
# Builtins are left ON.  An earlier version passed -fno-builtin, which quietly
# broke every test whose point is that a builtin folds: 20021127-1 defines an
# llabs() that calls abort() and passes only if gcc never emits the call.
#
# ${RT}/include holds the few headers mini-libc does not have and the firmware
# does not want -- <assert.h>, and a <stdint.h> that forwards to gcc's own.
COMMON="-w -fpermissive ${ARCH} -I${RT}/include -I${LIBC}/include"
# -nostdlib drops libgcc too, and without it anything using long long
# division or double arithmetic fails to link rather than failing to run.
LIBGCC=$(${GCC} ${ARCH} -print-libgcc-file-name)
LIBS="${LIBC}/lib/libc.a ${LIBGCC}"

mkdir -p "${WORK}"

# Things the test needs that this target or this runtime does not have.  None
# of these say anything about the backend, so reporting them as failures would
# only pad the numbers: a test that wants <math.h>, or __int128, or an x86
# register name, was never going to run here.
unsupported_p() {
	grep -qE "No such file or directory|is not supported on this target|not supported for this target|unknown register name|undefined reference|cannot find|'std(in|out|err)' undeclared" "$1"
}

# Tests that build and run but need a libc feature mini-libc does not have,
# so the harness cannot tell them apart from a wrong answer -- they abort
# just like a miscompilation would.  Each one is listed with what it wants.
# Upstream skips most of these itself, via dg-skip-if { freestanding } or
# dg-require-effective-target c99_runtime; the harness does not model
# effective targets, and a blanket skip on those directives would also throw
# away a dozen tests that do pass here.
skip_reason() {
	case "$1" in
	920501-8|930513-1) echo "sprintf %f: mini-libc printf has no float" ;;
	pr79327)           echo "sprintf %#hho/%#hhx: mini-libc printf has no # or hh" ;;
	20030125-1)        echo "needs a C99 math library to fold sin/floor against" ;;
	*)                 return 1 ;;
	esac
}

# The .exp driver reads dg-options/dg-additional-options out of the test and
# adds them to the command line; several tests are meaningless without them
# (eeprof-1 measures -finstrument-functions, and a good many predate C99 and
# say so with -std=gnu89).  Take the unconditional ones only: anything with a
# trailing { target ... } selector is aimed at a specific target, and we are
# not it.
dg_options() {
	# LC_ALL=C: a few of these files are not UTF-8 (20000227-1.c), and BSD
	# sed refuses to match at all rather than skipping the bad bytes.
	LC_ALL=C sed -n -e 's/.*{ *dg-options *"\([^"]*\)" *}.*/\1/p' \
	       -e 's/.*{ *dg-additional-options *"\([^"]*\)" *}.*/\1/p' \
	       -e 's/.*{ *dg-options *{ *"\([^"]*\)" *} *}.*/\1/p' "$1" |
		tr '\n' ' '
}

one() {                                 # one <mode> <opt> <file>
	local mode=$1 opt=$2 f=$3
	local b; b=$(basename "$f" .c)
	local tag="${b}.${opt#-}"
	local o="${WORK}/${tag}"
	local why
	if why=$(skip_reason "${b}"); then
		echo "UNSUPPORTED ${opt} ${b}  (${why})"
		return
	fi
	local dg; dg=$(dg_options "$f")

	if [ "${mode}" = compile ]; then
		if ${GCC} ${COMMON} ${dg} "${opt}" -S "$f" -o "${o}.s" 2>"${o}.log"; then
			echo "PASS ${opt} ${b}"
		elif grep -qE "internal compiler error|Segmentation fault" "${o}.log"; then
			echo "ICE ${opt} ${b}"
		elif unsupported_p "${o}.log"; then
			echo "UNSUPPORTED ${opt} ${b}"
		else
			echo "FAIL ${opt} ${b}"
		fi
		return
	fi

	if ! ${GCC} ${COMMON} ${dg} "${opt}" -nostdlib -nostartfiles \
	     -T "${RT}/test.lds" "${RT}/crt0.s" "$f" ${LIBS} -o "${o}.elf" 2>"${o}.log"; then
		if grep -qE "internal compiler error|Segmentation fault" "${o}.log"; then
			echo "ICE ${opt} ${b}"
		elif unsupported_p "${o}.log"; then
			echo "UNSUPPORTED ${opt} ${b}"
		else
			echo "FAIL ${opt} ${b}"
		fi
		return
	fi

	local out; out=$(cd "${WORK}" && ${EMU} -n "${LIMIT}" -b 0x10000002 "${o}.elf" 2>&1)
	local pc r4
	pc=$(echo "${out}" | awk -F= '/^pc=/{print $2}' | awk '{print $1}')
	r4=$(echo "${out}" | awk '/^r4 /{print $2}')

	# No register dump at all means the emulator never ran to completion --
	# it was killed, or could not start.  That is the harness failing, not
	# the test, and it must not be filed under TIMEOUT: a whole block of
	# them once looked like a compiler regression when the machine was
	# simply too busy to fork.
	if [ -z "${pc}" ]; then
		echo "NORUN ${opt} ${b}"
		return
	fi

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
export -f one dg_options unsupported_p skip_reason
export WORK RT GCC EMU COMMON LIBS LIMIT

for opt in "${opts[@]}"; do
	ls "${SRC}/${mode}"/*.c | \
		xargs -P "${JOBS}" -I{} bash -c 'one "$0" "$1" "$2"' "${mode}" "${opt}" {}
done
