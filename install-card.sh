#!/bin/sh
# Copy the launcher applications and their icons onto a WikiReader card.
#
# Run it with the card's boot partition mounted; it verifies every file
# after writing and again after a remount, because a copy that lands only
# in the page cache leaves a card that looks right and boots wrong.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
boot=${1:-/Volumes/WRBOOT}
[ -d "$boot" ] || { echo "no boot partition at $boot" >&2; exit 1; }

copy() {
	[ -f "$1" ] || { echo "missing: $1" >&2; return 1; }
	cp "$1" "$boot/$2"
	cmp -s "$1" "$boot/$2" || { echo "write failed: $2" >&2; return 1; }
	echo "  $2"
}

echo "installing to $boot"
copy "${here}/riscv/riscv.app"          riscv.app
copy "${here}/riscv/riscv.ico"          riscv.ico
copy "${here}/riscv/rvlinux.ico"        rvlinux.ico
copy "${here}/riscv/build/rvbench.bin"  rvbench.bin
# The three extra placements of the interpreter were on here to be measured
# against the emulator's model; they have been, the reports are checked in as
# riscv/rv*-device.txt, and a developer benchmark does not want a permanent
# square of the launcher.  Take them off a card that still has them.
for v in rvsdr rva0 rva0s; do
	for f in "${v}.app" "${v}.ico" "${v}.txt"; do
		[ -f "$boot/$f" ] && { rm -f "$boot/$f"; echo "  removed $f"; }
	done
done
copy "${here}/riscv/linux/Image"        rvlinux.bin
copy "${here}/riscv/linux/wr-sh.dtb"    rvlinux.dtb
copy "${here}/doom/doom.ico"            doom.ico
copy "${here}/nuttx/nuttx.app"          nuttx.app
copy "${here}/nuttx/nuttx.ico"          nuttx.ico

# ...and out of init.ini, whose order is the order of the launcher's grid.
for gone in rvsdr.ico rva0.ico rva0s.ico; do
	grep -q "^${gone} " "$boot/init.ini" || continue
	[ -f "$boot/initini.bak" ] || cp "$boot/init.ini" "$boot/initini.bak"
	grep -v "^${gone} " "$boot/init.ini" > "$boot/init.new"
	mv "$boot/init.new" "$boot/init.ini"
	echo "  init.ini -= $gone"
done

# "hold" keeps the benchmark's summary on the panel until a key is
# pressed; the run itself is under two seconds.  It also writes the full
# table to rvbench.txt on this card, which is the copy to read later.
for entry in \
	'riscv.ico : riscv.app rvbench.bin hold' \
	'rvlinux.ico : riscv.app rvlinux.bin'; do
	icon=${entry%% *}
	grep -qxF "$entry" "$boot/init.ini" && continue
	[ -f "$boot/initini.bak" ] || cp "$boot/init.ini" "$boot/initini.bak"
	# An icon already listed is rewritten where it stands: the file's order
	# is the order of the launcher's grid, so appending instead would move
	# the entry to another square every time its arguments changed.
	if grep -q "^${icon} " "$boot/init.ini"; then
		awk -v icon="$icon" -v line="$entry" \
		    '$1 == icon { print line; next } { print }' \
		    "$boot/init.ini" > "$boot/init.new"
	else
		cp "$boot/init.ini" "$boot/init.new"
		printf '%s\n' "$entry" >> "$boot/init.new"
	fi
	mv "$boot/init.new" "$boot/init.ini"
	echo "  init.ini: $entry"
done

sync
echo "verifying after remount"
device=$(df "$boot" | tail -1 | cut -d' ' -f1)
diskutil unmount "$boot" >/dev/null
sleep 2
diskutil mount "${device#/dev/}" >/dev/null
sleep 2
for f in riscv.app rvbench.bin \
         rvlinux.bin rvlinux.dtb doom.ico nuttx.app nuttx.ico; do
	case $f in
	riscv.app)   src=${here}/riscv/riscv.app ;;
	rvbench.bin) src=${here}/riscv/build/rvbench.bin ;;
	rvlinux.bin) src=${here}/riscv/linux/Image ;;
	rvlinux.dtb) src=${here}/riscv/linux/wr-sh.dtb ;;
	doom.ico)    src=${here}/doom/doom.ico ;;
	nuttx.app)   src=${here}/nuttx/nuttx.app ;;
	nuttx.ico)   src=${here}/nuttx/nuttx.ico ;;
	esac
	cmp -s "$src" "$boot/$f" && echo "  ok $f" || { echo "  BAD $f" >&2; exit 1; }
done
echo "done"
echo "the RV32 benchmark writes rvbench.txt to this card; read it back here"
echo "afterwards to compare the device against the emulator's model"
