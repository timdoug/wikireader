#!/bin/sh
# Update the launcher applications on a WikiReader card made by
# zim/make-card-image: NuttX, and native Linux with its system image.
#
# Run it with the card's boot partition mounted; it verifies every file
# after writing and again after a remount, because a copy that lands only
# in the page cache leaves a card that looks right and boots wrong.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
boot=${1:-/Volumes/WRBOOT}
[ -d "$boot" ] || { echo "no boot partition at $boot" >&2; exit 1; }

# card name and source, one per line
files="nuttx.app ${here}/nuttx/nuttx.app
nuttx.ico ${here}/nuttx/nuttx.ico
linux.app ${here}/linux/artifacts/linux.app
linux.ico ${here}/linux/artifacts/linux.ico
linux.img ${here}/linux/artifacts/linux.img
bin/t ${here}/linux/card/bin/t
bin/check ${here}/linux/card/bin/check
$(grep -v '^#' "${here}/linux/card/tests.list" |
	while read -r name src; do echo "$name ${here}/linux/artifacts/$src"; done)"

echo "installing to $boot"
mkdir -p "$boot/bin"
echo "$files" | while read -r name src; do
	[ -f "$src" ] || { echo "missing: $src" >&2; exit 1; }
	if [ "$name" = linux.img ] && [ -f "$boot/$name" ]; then
		# Written in place, so it keeps its clusters: every read into the
		# system walks this file's FAT chain, and cp may reallocate it.
		dd if="$src" of="$boot/$name" bs=1m conv=notrunc 2>/dev/null
	else
		cp "$src" "$boot/$name"
	fi
	cmp -s "$src" "$boot/$name" || { echo "write failed: $name" >&2; exit 1; }
	echo "  $name"
done

# The RISC-V emulator's entries, from before Linux ran natively: take them
# off a card that still has them.
for f in riscv.app riscv.ico rvbench.bin rvbench.txt rvlinux.bin rvlinux.dtb \
         rvlinux.ico rvsdr.app rvsdr.ico rvsdr.txt rva0.app rva0.ico rva0.txt \
         rva0s.app rva0s.ico rva0s.txt; do
	[ -f "$boot/$f" ] && { rm -f "$boot/$f"; echo "  removed $f"; }
done

# init.ini's order is the order of the launcher's grid: drop the removed
# entries, and append the ones this installs only when they are missing.
grep -v -E '^(riscv|rvlinux|rvsdr|rva0|rva0s)\.ico ' "$boot/init.ini" \
	> "$boot/init.new" || true
for entry in 'nuttx.ico : nuttx.app started-from-init' \
	     'linux.ico : linux.app started-from-init'; do
	grep -q "^${entry%% *} " "$boot/init.new" || printf '%s\n' "$entry" >> "$boot/init.new"
done
if cmp -s "$boot/init.new" "$boot/init.ini"; then
	rm "$boot/init.new"
else
	mv "$boot/init.new" "$boot/init.ini"
	echo "  init.ini updated"
fi

sync
echo "verifying after remount"
device=$(df "$boot" | tail -1 | cut -d' ' -f1)
diskutil unmount "$boot" >/dev/null
diskutil mount "${device#/dev/}" >/dev/null
echo "$files" | while read -r name src; do
	cmp -s "$src" "$boot/$name" && echo "  ok $name" || { echo "  BAD $name" >&2; exit 1; }
done
echo "done"
