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
# Both default into ../work, which gcc/rebuild.sh populates and .gitignore
# covers.  These used to point at /tmp; macOS purges /tmp nightly and gutted
# the source tree overnight, so do not put a build tree back there.
TCROOT="${HERE}/../work"
GCC=${GCC:-${TCROOT}/install/bin/c33-epson-elf-gcc}
EMU=${EMU:-$(cd "${HERE}/../../../emulator" && pwd)/wremu}
SRC=${SRC:-${TCROOT}/gcc-16.2.0/gcc/testsuite/gcc.c-torture}
WORK=${WORK:-/tmp/c33torture}
# Instruction budget per test.  Not a timing knob -- the emulator is
# deterministic -- but a runaway detector.  It has to clear the slowest
# honest test: vla-dealloc-1 and pr43220 spend ~490M instructions at -O0
# allocating a VLA a million times over, and at 200M they looked like
# hangs.  A test that has genuinely jumped into the weeds usually trips
# the emulator's own "256 consecutive zero words" check long before this.
LIMIT=${LIMIT:-1000000000}
JOBS=${JOBS:-8}

# The option sets upstream's c-torture.exp uses, verbatim.  An entry may be
# several words, so nothing here may quote "${opt}" as a single argument --
# it has to word-split on the command line, and the tag it names files with
# has to have the spaces squeezed out.
mode=${1:-execute}; shift || true
opts=("$@")
if [ ${#opts[@]} -eq 0 ]; then
	opts=(-O0 -O1 -O2 \
	      "-O3 -fomit-frame-pointer -funroll-loops -fpeel-loops -ftracer -finline-functions" \
	      "-O3 -g" -Os "-Og -g")
fi

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

# Runtime support the tests need and mini-libc does not have: putchar, the
# malloc family, setjmp/longjmp.  The allocator itself is grifo's, compiled
# straight from the firmware source rather than reimplemented -- so these
# tests also put real firmware code through the new compiler.
#
# -fgnu89-inline is what the firmware builds with, and for the same reason:
# mini-libc declares abs()/labs() "extern __inline__", which under C99
# semantics emits an external definition in every translation unit that
# includes <stdlib.h>.  It is confined to the runtime here rather than put
# in COMMON, so it cannot change the meaning of a test that is itself about
# inline semantics.
GRIFO="${HERE}/../../../samo-lib/grifo"
RT_INC="-I${GRIFO}/src -I${GRIFO}/common"
RT_CFLAGS="-fgnu89-inline"

# Built once per optimisation level rather than once per test -- 6768 tests
# would otherwise recompile four files apiece -- but still built at each
# level, so the firmware code in it is exercised the same way the tests are.
build_runtime() {                       # build_runtime <opt>; echoes the objects
	local opt=$1 d
	d="${WORK}/rt$(opt_tag "${opt}")"
	mkdir -p "${d}"
	if [ ! -f "${d}/stamp" ]; then
		${GCC} ${ARCH} ${opt} -c "${RT}/crt0.s"   -o "${d}/crt0.o"   || return 1
		${GCC} ${ARCH} ${opt} -c "${RT}/setjmp.s" -o "${d}/setjmp.o" || return 1
		${GCC} ${COMMON} ${RT_INC} ${RT_CFLAGS} ${opt} -c \
			"${RT}/runtime.c" -o "${d}/runtime.o" || return 1
		${GCC} ${COMMON} ${RT_INC} ${RT_CFLAGS} ${opt} -c \
			"${GRIFO}/src/memory.c" -o "${d}/memory.o" || return 1
		touch "${d}/stamp"
	fi
	echo "${d}/crt0.o ${d}/setjmp.o ${d}/runtime.o ${d}/memory.o"
}

mkdir -p "${WORK}"

# Things the test needs that this target or this runtime does not have.  None
# of these say anything about the backend, so reporting them as failures would
# only pad the numbers: a test that wants <math.h>, or __int128, or an x86
# register name, was never going to run here.
unsupported_p() {
	grep -qE "No such file or directory|is not supported on this target|not supported for this target|unknown register name|invalid register name|unknown type name '__u?int128_t'|expected expression before '__int128'|before '__declspec'|unrecognized command-line option|undefined reference|cannot find|'std(in|out|err)' undeclared" "$1"
}

# Tests that name the target they are for, in { dg-do compile { target ... } }.
# We only have to recognise that the selector is not us: a triplet always has
# a dash in it, and lp64 is plainly false on a 32-bit machine.  Effective
# targets we do satisfy (int32plus, alloca, ...) are left alone -- guessing at
# the whole dg effective-target vocabulary would throw away real coverage.
wrong_target_p() {
	local sel

	# "{ dg-skip-if ... { ! { i?86-*-* x86_64-*-* } } }" -- the test declaring
	# itself to belong to other architectures only.  Exactly one test in the
	# suite does this (990413-2, x87 inline asm), and we already reported it
	# UNSUPPORTED -- but only because the compile failed with "invalid
	# register name", which unsupported_p greps for.  Reading the test's own
	# directive is the honest route; a diagnostic string is upstream's to
	# change.  Deliberately narrow: it matches only the negated-triplet form.
	# The plain "{ dg-skip-if ... { avr-*-* } }" spelling names targets to
	# skip *on*, which is not us, and the "{ freestanding }" ones cover
	# tests we now pass -- neither must start being skipped here.
	sel=$(LC_ALL=C sed -n 's/.*{ *dg-skip-if [^{]*{ *! *{ \([^}]*\)}.*/\1/p' "$1" | head -1)
	case "${sel}" in
	"")            ;;
	*c33*)         ;;
	*-*)           return 0 ;;
	esac

	sel=$(LC_ALL=C sed -n 's/.*{ *dg-do *[a-z]* *{ *target \([^}]*\)}.*/\1/p' "$1" | head -1)
	case "${sel}" in
	"")            return 1 ;;
	*c33*)         return 1 ;;
	*-*|*lp64*)    return 0 ;;
	*)             return 1 ;;
	esac
}

# A compile test carrying dg-error is meant to be rejected; the .exp driver
# passes it when the expected diagnostics appear.  We do not match diagnostic
# text, so the most this harness can honestly check is the thing it is here
# for: that the compiler diagnosed rather than crashed.
expects_error_p() {
	grep -q "dg-error" "$1"
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
	# mini-libc's printf has no %f, and it is staying that way.  Adding it
	# pulls __adddf3/__subdf3 (858 bytes), __muldf3 (594), __divdf3 (348),
	# the df comparison set and __fixdfsi into *every* program that links
	# printf -- roughly 3 KB.  Neither wiki.app nor grifo.elf contains a
	# single double soft-float symbol today, and no format string anywhere
	# in samo-lib, wiki or host-tools uses %f, %e or %g.  Two tests is not
	# worth 3 KB in the shipped library.
	#
	# Neither test is really about float formatting anyway: 920501-8 is a
	# varargs test (a double in slot 2 then thirteen va_arg ints) and
	# 930513-1 calls sprintf through a K&R-declared function pointer.  The
	# argument passing both lean on is covered directly by tests/abi.
	920501-8|930513-1) echo "sprintf %f: deliberate, see comment" ;;
	# 20030125-1 checks that sin/floor fold; with no C99 libm declared, gcc
	# folds them at -O0/-O2/-O3 and not at -O1/-Os/-Og -- four sets pass and
	# three abort.  Upstream's own dg-require-effective-target c99_runtime
	# says not to run it here.
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

# A very short table of options a test needs but does not ask for.  Upstream
# put -std=gnu89 in dg-options on most of the pre-ANSI tests and missed some;
# comp-goto-1 declares "char *malloc();", which a C23 compiler reads as
# taking no arguments and rejects against its own builtin.  Giving it the
# dialect it was written for is better than skipping it: the test then runs
# and its computed gotos are actually exercised.
extra_options() {
	case "$1" in
	comp-goto-1) echo "-std=gnu89" ;;
	# Upstream's own escape hatch for targets with no signals:
	#   { dg-additional-options "-DSIGNAL_SUPPRESS" { target { ! signal } } }
	# The harness applies dg-additional-options but ignores the target
	# selector on them, so this one never got supplied and the test was
	# filed as UNSUPPORTED for a missing <signal.h> it does not need.
	# It passes at all seven sets with the flag.  Worth remembering that
	# "UNSUPPORTED" here can mean "the harness did not read the directive",
	# not "the target cannot do this".
	20101011-1)  echo "-DSIGNAL_SUPPRESS" ;;
	esac
}

# Short, stable name for an option set, for filenames and result lines.
opt_tag() {
	case "$1" in
	"-O3 -fomit-frame-pointer"*) echo "O3f" ;;
	"-O3 -g")                    echo "O3g" ;;
	"-Og -g")                    echo "Ogg" ;;
	*)                           echo "$1" | tr -d ' -' ;;
	esac
}

one() {                                 # one <mode> <opt> <file>
	local mode=$1 opt=$2 f=$3
	local b; b=$(basename "$f" .c)
	local tag; tag="${b}.$(opt_tag "${opt}")"
	local o="${WORK}/${tag}"
	local why
	if why=$(skip_reason "${b}"); then
		echo "UNSUPPORTED ${tag##*.} ${b}  (${why})"
		return
	fi
	if wrong_target_p "$f"; then
		echo "UNSUPPORTED ${tag##*.} ${b}  (written for another target)"
		return
	fi
	local dg; dg="$(dg_options "$f") $(extra_options "${b}")"

	if [ "${mode}" = compile ]; then
		if ${GCC} ${COMMON} ${dg} ${opt} -S "$f" -o "${o}.s" 2>"${o}.log"; then
			echo "PASS ${tag##*.} ${b}"
		elif grep -qE "internal compiler error|Segmentation fault" "${o}.log"; then
			echo "ICE ${tag##*.} ${b}"
		elif expects_error_p "$f"; then
			echo "PASS ${tag##*.} ${b}"
		elif unsupported_p "${o}.log"; then
			echo "UNSUPPORTED ${tag##*.} ${b}"
		else
			echo "FAIL ${tag##*.} ${b}"
		fi
		return
	fi

	if ! ${GCC} ${COMMON} ${dg} ${opt} -nostdlib -nostartfiles \
	     -T "${RT}/test.lds" ${RT_OBJS} "$f" ${LIBS} -o "${o}.elf" 2>"${o}.log"; then
		if grep -qE "internal compiler error|Segmentation fault" "${o}.log"; then
			echo "ICE ${tag##*.} ${b}"
		elif unsupported_p "${o}.log"; then
			echo "UNSUPPORTED ${tag##*.} ${b}"
		else
			echo "FAIL ${tag##*.} ${b}"
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
		echo "NORUN ${tag##*.} ${b}"
		return
	fi

	if [ "${pc}" = "10000002" ] && [ "${r4}" = "00000000" ]; then
		echo "PASS ${tag##*.} ${b}"
	elif [ "${pc}" = "10000002" ] && [ "${r4}" = "0000dead" ]; then
		echo "ABORT ${tag##*.} ${b}"
	elif echo "${out}" | grep -q "^fault:"; then
		echo "FAULT ${tag##*.} ${b}  $(echo "${out}" | grep -m1 '^fault:' | cut -c1-60)"
	elif [ "${pc}" = "10000002" ]; then
		echo "EXIT${r4} ${tag##*.} ${b}"
	else
		echo "TIMEOUT ${tag##*.} ${b}"
	fi
}
export -f one opt_tag dg_options extra_options unsupported_p skip_reason wrong_target_p expects_error_p
export WORK RT GCC EMU COMMON LIBS LIMIT RT_OBJS

for opt in "${opts[@]}"; do
	if [ "${mode}" != compile ]; then
		if ! RT_OBJS=$(build_runtime "${opt}"); then
			echo "FATAL ${opt}: runtime failed to build" >&2
			exit 1
		fi
		export RT_OBJS
	fi
	ls "${SRC}/${mode}"/*.c | \
		xargs -P "${JOBS}" -I{} bash -c 'one "$0" "$1" "$2"' "${mode}" "${opt}" {}
done
