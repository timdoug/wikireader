#!/bin/sh
# Put a one-tap reader benchmark on a WikiReader card.
#
#   zim/bench-device.sh [BOOT_VOLUME]      build both readers and install
#   zim/bench-device.sh --report [BOOT]    compare the results on the card
#
# Installs this tree's reader as zim.app and the baseline (BENCH_BASE,
# default 5d87bb2b, the build before the 2026-10-31 image work) as
# zimold.app, both carrying the same benchmark driver (zim_bench.c), and
# adds an inverted-kiwi icon to the launcher.  Tapping it runs the baseline,
# which chains to the new build; each loads the articles in zim/zimbench.lst
# cold, once by default, and appends its timings to zimbench.txt.  The new build then shows the comparison; a tap returns to
# the launcher.  Run --report with the card back in this machine.
#
# zim/zimbench.lst says which articles; it is copied to the card each time.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
base=${BENCH_BASE:-5d87bb2b}
out=$here/build/bench

if [ "${1:-}" = --report ]; then
	boot=${2:-/Volumes/WRBOOT}
	exec python3 "$here/bench-report.py" "$boot/zimbench.txt"
fi
boot=${1:-/Volumes/WRBOOT}
[ -d "$boot" ] || { echo "no boot partition at $boot" >&2; exit 1; }
[ -f "$boot/init.ini" ] || { echo "$boot has no init.ini" >&2; exit 1; }

mkdir -p "$out"
echo "building the new reader"
make -C "$here" > "$out/new-build.log" 2>&1 ||
	{ tail -20 "$out/new-build.log" >&2; exit 1; }
cp "$here/zim.app" "$out/zim.app"

echo "building the baseline reader ($base)"
tree=$(mktemp -d)/wr
git -C "$root" worktree add --detach --quiet "$tree" "$base"
trap 'git -C "$root" worktree remove --force "$tree"' EXIT
# The toolchain and the libraries are build products; the baseline uses
# this tree's.
ln -s "$root/host-tools/toolchain-c33/work" "$tree/host-tools/toolchain-c33/work"
for d in samo-lib/grifo/include samo-lib/grifo/lib samo-lib/mini-libc/lib; do
	mkdir -p "$tree/$d"
	cp -R "$root/$d/." "$tree/$d/"
done
# The driver and its hooks, in files the image work did not touch: apart
# from the commit that added the driver, they must match the baseline.
added=$(git -C "$root" log --diff-filter=A --format=%H -1 -- zim/zim_bench.c)
for f in zim/zim_bench.c zim/zim_bench.h zim/zim.c zim/search.c \
	 zim/zim_page.c wiki/wikilib.c; do
	if [ "$f" != zim/zim_bench.c ] && [ "$f" != zim/zim_bench.h ] &&
	   ! git -C "$root" diff --quiet "$base" "$added^" -- "$f"; then
		echo "$f changed since $base other than for the benchmark" >&2
		exit 1
	fi
	cp "$root/$f" "$tree/$f"
done
# After the last source, as here, so the benchmark moves no existing code.
awk 'FNR == NR { if ($0 ~ /^SOURCES \+= /) last = FNR; next }
     { print } FNR == last { print "SOURCES += zim_bench.c" }' \
    "$tree/zim/Makefile" "$tree/zim/Makefile" > "$tree/zim/Makefile.new"
mv "$tree/zim/Makefile.new" "$tree/zim/Makefile"
make -C "$tree/zim" > "$out/old-build.log" 2>&1 ||
	{ tail -20 "$out/old-build.log" >&2; exit 1; }
cp "$tree/zim/zim.app" "$out/zimold.app"

# The launcher icon: the reader's kiwi, inverted.
python3 -c '
import sys
s = open(sys.argv[1]).read()
s = s.replace("c #FFFFFF", "c #TMP").replace("c #000000", "c #FFFFFF")
open(sys.argv[2], "w").write(s.replace("c #TMP", "c #000000"))
' "$here/zim.xpm" "$out/zimbench.xpm"
python3 "$root/samo-lib/grifo/scripts/xpm2icon" --icon="$out/zimbench.ico" \
	"$out/zimbench.xpm"

echo "installing to $boot"
copy() {
	cp "$1" "$boot/$2"
	cmp -s "$1" "$boot/$2" || { echo "write failed: $2" >&2; exit 1; }
	echo "  $2"
}
copy "$out/zim.app" zim.app
copy "$out/zimold.app" zimold.app
copy "$out/zimbench.ico" zimbench.ico
copy "$here/zimbench.lst" zimbench.lst
entry='zimbench.ico : zimold.app bench old zim.app new'
if grep -q '^zimbench.ico ' "$boot/init.ini"; then
	awk -v line="$entry" '$1 == "zimbench.ico" { print line; next } { print }' \
	    "$boot/init.ini" > "$boot/init.new"
else
	cp "$boot/init.ini" "$boot/init.new"
	printf '%s\n' "$entry" >> "$boot/init.new"
fi
mv "$boot/init.new" "$boot/init.ini"
echo "  init.ini: $entry"

# A copy that lands only in the page cache leaves a card that looks right
# and boots wrong (install-card.sh): check again after a remount.
sync
device=$(df "$boot" | tail -1 | cut -d' ' -f1)
diskutil unmount "$boot" > /dev/null
sleep 2
diskutil mount "${device#/dev/}" > /dev/null
sleep 2
for f in zim.app zimold.app zimbench.ico; do
	cmp -s "$out/$f" "$boot/$f" || { echo "  BAD $f" >&2; exit 1; }
	echo "  ok $f"
done
grep -q "^$entry\$" "$boot/init.ini" || { echo "  BAD init.ini" >&2; exit 1; }
echo "  ok init.ini"
echo "done: eject the card, put it in the WikiReader and tap the inverted kiwi"
echo "then run: zim/bench-device.sh --report"
