#!/bin/bash
# Differential test: same C source through the real c33 cross compiler into
# the emulator, and through the host compiler natively. Outputs must match.
#
# Usage: ./run.sh [first_seed] [last_seed]
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
# Which toolchain generates the target code.  Defaults to the original, which
# is the stronger oracle -- it has never seen the emulator.  Point DT_TC at
# the gcc 16 install to check the emulator against the instruction mix the
# *new* compiler emits: filled delay slots, post-increment addressing and ext
# prefixing that 3.3.2 never generated, and which the emulator has therefore
# never been differentially tested on.
TC="${DT_TC:-$HERE/../../host-tools/toolchain-install/bin}"
WREMU="$HERE/../wremu"
WORK="${DT_WORK:-/tmp/dt}"
OPTS="${DT_OPTS:--O2}"
NINSN="${DT_NINSN:-400000000}"

# The two toolchains put libgcc in different places, so ask rather than guess.
LIBGCC="$("$TC/c33-epson-elf-gcc" -mc33pe -print-libgcc-file-name)" || exit 2

first="${1:-1}"
last="${2:-$first}"

mkdir -p "$WORK"

# The startup stub is the same for every program.
"$TC/c33-epson-elf-as" -mc33pe "$HERE/runtime/start.s" -o "$WORK/start.o" || exit 2

pass=0 fail=0 failed_seeds=()

for seed in $(seq "$first" "$last"); do
	src="$WORK/t$seed.c"
	python3 "$HERE/gen.py" "$seed" > "$src" || exit 2

	# --- host reference
	if ! cc -DDT_HOST -w $OPTS -I "$HERE/runtime" "$src" -o "$WORK/t$seed.host" 2>"$WORK/t$seed.hosterr"; then
		echo "seed $seed: HOST COMPILE FAILED"; sed -n 1,5p "$WORK/t$seed.hosterr"; fail=$((fail+1)); continue
	fi
	"$WORK/t$seed.host" > "$WORK/t$seed.host.out" || true

	# --- target
	if ! "$TC/c33-epson-elf-gcc" -mc33pe -w $OPTS -I "$HERE/runtime" \
		-c "$src" -o "$WORK/t$seed.o" 2>"$WORK/t$seed.cerr"; then
		echo "seed $seed: TARGET COMPILE FAILED"; sed -n 1,5p "$WORK/t$seed.cerr"; fail=$((fail+1)); continue
	fi
	if ! "$TC/c33-epson-elf-ld" -T "$HERE/runtime/target.lds" \
		"$WORK/start.o" "$WORK/t$seed.o" "$LIBGCC" \
		-o "$WORK/t$seed.elf" 2>"$WORK/t$seed.lderr"; then
		echo "seed $seed: LINK FAILED"; sed -n 1,5p "$WORK/t$seed.lderr"; fail=$((fail+1)); continue
	fi

	"$WREMU" -n "$NINSN" "$WORK/t$seed.elf" 2>&1 \
		| grep -E '^[0-9a-f]{8}$' > "$WORK/t$seed.emu.out"

	if diff -q "$WORK/t$seed.host.out" "$WORK/t$seed.emu.out" >/dev/null 2>&1; then
		n=$(wc -l < "$WORK/t$seed.host.out" | tr -d ' ')
		echo "seed $seed: ok ($n values)"
		pass=$((pass+1))
	else
		hn=$(wc -l < "$WORK/t$seed.host.out" | tr -d ' ')
		en=$(wc -l < "$WORK/t$seed.emu.out" | tr -d ' ')
		echo "seed $seed: MISMATCH (host $hn values, emulator $en)"
		diff "$WORK/t$seed.host.out" "$WORK/t$seed.emu.out" | head -6 | sed 's/^/    /'
		fail=$((fail+1)); failed_seeds+=("$seed")
	fi
done

echo
echo "=== $pass passed, $fail failed ==="
[ ${#failed_seeds[@]} -gt 0 ] && echo "failing seeds: ${failed_seeds[*]}"
exit $((fail != 0))
