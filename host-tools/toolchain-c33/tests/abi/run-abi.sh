#!/bin/bash
# ABI cross-link test: does code from the new compiler still call, and get
# called by, code from the original one?
#
#   ./run-abi.sh
#
# The firmware links hand-written assembly and prebuilt objects from
# ROOT_IMAGE/ that were produced by gcc 3.3.2 and cannot be rebuilt.  An ABI
# disagreement there does not fail to link and does not crash -- it silently
# passes the wrong value.  gcc.c-torture cannot see it: every test there is
# self-contained and compiled by one compiler.
#
# So build the two halves of a program with the two toolchains in all four
# combinations and diff the output.  old/old is the reference.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
DT="${HERE}/../../../../emulator/difftest"
OLD=${OLD:-$(cd "${HERE}/../../../toolchain-install/bin" && pwd)}
NEW=${NEW:-$(cd "${HERE}/../../work/install/bin" 2>/dev/null && pwd)}
EMU=${EMU:-$(cd "${HERE}/../../../../emulator" && pwd)/wremu}
WORK=${WORK:-/tmp/c33abi}
OPTS=${OPTS:--O2}
LIBC=${LIBC:-$(cd "${HERE}/../../../../samo-lib/mini-libc" && pwd)/lib/libc.a}

mkdir -p "${WORK}"

# -mno-long-calls only exists in the new compiler; the old one defaults to
# short calls, so the two agree without it.
oldcc() { "${OLD}/c33-epson-elf-gcc" -mc33pe -w ${OPTS} "$@"; }
newcc() { "${NEW}/c33-epson-elf-gcc" -mc33pe -mno-long-calls -w ${OPTS} "$@"; }

build() {                       # build <name> <callee-cc> <caller-cc>
	local name=$1 cee=$2 cer=$3
	${cee} -I"${DT}/runtime" -c "${HERE}/abi-callee.c" -o "${WORK}/${name}-callee.o" \
		2>"${WORK}/${name}.log" || { echo "${name}: callee compile failed"; return 1; }
	# Link with the new ld either way: it is the one under test, and the old
	# one cannot read the new objects' DWARF.  Both libgcc archives are
	# supplied so whichever half needs a helper finds one, and mini-libc for
	# the memcpy that a struct copy expands to.
	local caller_tc="${OLD}" target_flags=""
	if [ "$cer" = newcc ]; then caller_tc="${NEW}"; target_flags="-mno-long-calls"; fi
	DT_TC="$caller_tc" DT_LD="${NEW}/c33-epson-elf-ld" DT_WREMU="${EMU}" \
	DT_WORK="${WORK}/${name}-grifo" DT_OPTS="${OPTS}" \
	python3 "${DT}/run.py" --source "${HERE}/abi-caller.c" --target-only \
		--target-flags="$target_flags" --extra-object "${WORK}/${name}-callee.o" \
		--extra-library "$LIBC" --extra-library "$(newcc -print-libgcc-file-name)" \
		--extra-library "$(oldcc -print-libgcc-file-name)" --output "${WORK}/${name}.out" \
		|| { echo "${name}: Grifo application failed"; return 1; }
}

fail=0
build oldold oldcc oldcc || fail=1
build newnew newcc newcc || fail=1
build oldcee-newcer oldcc newcc || fail=1
build newcee-oldcer newcc oldcc || fail=1

ref="${WORK}/oldold.out"
n=$(wc -l < "${ref}" | tr -d ' ')
echo "reference (3.3.2 both halves): ${n} values"
[ "${n}" -eq 0 ] && { echo "reference produced nothing -- harness is broken"; exit 2; }

for name in newnew oldcee-newcer newcee-oldcer; do
	if diff -q "${ref}" "${WORK}/${name}.out" >/dev/null 2>&1; then
		echo "  ${name}: ok"
	else
		echo "  ${name}: MISMATCH"
		diff "${ref}" "${WORK}/${name}.out" | head -10 | sed 's/^/      /'
		fail=1
	fi
done

exit ${fail}
