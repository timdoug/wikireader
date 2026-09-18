#!/bin/sh
# Put the translator probe on a WikiReader card, and take it off again.
#
# The probe is a developer measurement, not an application: it asks the
# hardware what translated code would cost, which is the one number the
# emulator's model cannot be trusted for.  So it goes on the panel the way
# the placement variants did -- while it is being measured, and not after.
# `install-jit-probe.sh --remove` is the second half of that sentence.
#
# It also refreshes riscv.app, which both the RV32 and the Tux entries run:
# the probe lives inside that application, chosen by its "jit" argument.
# The interpreter in it is unchanged and at the same addresses, so the
# benchmark on this card still measures what rv*-device.txt measured.
#
# Run it with the card's boot partition mounted.  Every write is compared
# back, and again after a remount, because a copy that lands only in the
# page cache leaves a card that looks right and boots wrong.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
remove=no
if [ "${1:-}" = "--remove" ]; then
	remove=yes
	shift
fi
boot=${1:-/Volumes/WRBOOT}
[ -d "$boot" ] || { echo "no boot partition at $boot" >&2; exit 1; }
[ -f "$boot/init.ini" ] || { echo "$boot has no init.ini" >&2; exit 1; }

entry='rvjit.ico : riscv.app jit hold'

copy() {
	[ -f "$1" ] || { echo "missing: $1" >&2; return 1; }
	cp "$1" "$boot/$2"
	cmp -s "$1" "$boot/$2" || { echo "write failed: $2" >&2; return 1; }
	echo "  $2"
}

[ -f "$boot/initini.bak" ] || cp "$boot/init.ini" "$boot/initini.bak"

if [ "$remove" = yes ]; then
	echo "removing the probe from $boot"
	if grep -q '^rvjit.ico ' "$boot/init.ini"; then
		grep -v '^rvjit.ico ' "$boot/init.ini" > "$boot/init.new"
		mv "$boot/init.new" "$boot/init.ini"
		echo "  init.ini -= rvjit.ico"
	fi
	for f in rvjit.ico; do
		[ -f "$boot/$f" ] && { rm -f "$boot/$f"; echo "  removed $f"; }
	done
	echo "  rvjit.txt left in place; it is the measurement"
else
	echo "installing the probe to $boot"
	copy "${here}/riscv.app" riscv.app
	copy "${here}/rvjit.ico" rvjit.ico
	if grep -qxF "$entry" "$boot/init.ini"; then
		echo "  init.ini already has it"
	else
		# Appended, so it takes the next free square and leaves every
		# existing icon where the user expects to find it.
		grep -v '^rvjit.ico ' "$boot/init.ini" > "$boot/init.new"
		printf '%s\n' "$entry" >> "$boot/init.new"
		mv "$boot/init.new" "$boot/init.ini"
		echo "  init.ini += $entry"
	fi
fi

sync
echo "verifying after remount"
device=$(df "$boot" | tail -1 | cut -d' ' -f1)
diskutil unmount "$boot" >/dev/null
sleep 2
diskutil mount "${device#/dev/}" >/dev/null
sleep 2
if [ "$remove" = yes ]; then
	! grep -q '^rvjit.ico ' "$boot/init.ini" || { echo "  BAD init.ini" >&2; exit 1; }
	[ ! -f "$boot/rvjit.ico" ] || { echo "  BAD rvjit.ico still there" >&2; exit 1; }
	echo "  ok, the panel is back to what it was"
else
	cmp -s "${here}/riscv.app" "$boot/riscv.app" || { echo "  BAD riscv.app" >&2; exit 1; }
	cmp -s "${here}/rvjit.ico" "$boot/rvjit.ico" || { echo "  BAD rvjit.ico" >&2; exit 1; }
	grep -qxF "$entry" "$boot/init.ini" || { echo "  BAD init.ini" >&2; exit 1; }
	echo "  ok riscv.app, rvjit.ico, init.ini"
	echo
	echo "on the device: the JIT PROB icon runs nine templates and writes"
	echo "rvjit.txt to this card.  The panel holds the last line until a key"
	echo "is pressed; the table is wider than 40 columns and is in the file."
fi
