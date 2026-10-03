#!/bin/bash
# Run wiki.app inside wremu against a pristine copy of the card image, booted
# the way the device boots it: MBR, the FLASH boot loader, grifo as
# kernel.elf off the card, init.app, wiki.app.
#
# The firmware writes back to the card (history, settings), so every run must
# start from the same snapshot or results drift between invocations.
#
# usage: run-firmware-test.sh <wiki.app> <output.txt> [keys] [wremu options...]
set -e

APP="$1"
OUT="$2"
KEYS="${3:-LOVE}"
if [ "$#" -ge 3 ]; then
	shift 3
else
	shift "$#"
fi
WREMU_ARGS=("$@")
LIMIT="${LIMIT:-500000000}"
KEY_AT="${KEY_AT:-120000000}"

SNAP="${CARD_SNAPSHOT:-/tmp/card_snap.img}"
IMG=$(mktemp /tmp/card_test.XXXXXX.img)
FLASH=$(mktemp -u /tmp/flash.XXXXXX.rom)
MNT=$(mktemp -d /tmp/wrmnt.XXXXXX)
HERE=$(cd "$(dirname "$0")/.." && pwd)
GRIFO="${GRIFO:-$HERE/../samo-lib/grifo/grifo.elf}"

cleanup() {
	diskutil unmount force "$MNT" >/dev/null 2>&1 || true
	[ -n "$DEV" ] && hdiutil detach "$DEV" -force >/dev/null 2>&1 || true
	rm -rf "$IMG" "$FLASH" "$MNT"
}
trap cleanup EXIT

cp "$SNAP" "$IMG"
DEV=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$IMG" | awk 'NR==1 {print $1}')
VOL="$DEV"
[ -e "${DEV}s1" ] && VOL="${DEV}s1"	# a card with a partition table
diskutil mount -mountPoint "$MNT" "$VOL" >/dev/null
cp "$APP" "$MNT/wiki.app"
cp "$GRIFO" "$MNT/kernel.elf"
[ -n "$EXTRA_LOGO" ] && cp "$EXTRA_LOGO" "$MNT/logo.xbm"
sync
diskutil unmount "$MNT" >/dev/null
hdiutil detach "$DEV" >/dev/null
DEV=

python3 "$HERE/../samo-lib/mbr/make-flash.py" "$FLASH" >/dev/null

# Type well after the app is up, and run long enough afterwards for the search
# to settle.  Two traps if you retune these numbers:
#
#  - Taps delivered while grifo is still loading wiki.app wedge the boot.
#  - The screen is dumped wherever the instruction budget happens to stop, so
#    an unsettled UI compares unequal even between two identical builds.
#
# Both shift with the app's size, so any code change moves them.  A diff here
# only means something when both sides have settled (~46 non-blank lines for
# a search that returns hits).
if [ "${FULL_OUTPUT:-NO}" = YES ]; then
	"$HERE/wremu" -R -e "$FLASH" -c "$IMG" -n "$LIMIT" \
		-K "$KEY_AT,$KEYS" "${WREMU_ARGS[@]}" > "$OUT" 2>&1
else
	"$HERE/wremu" -R -e "$FLASH" -c "$IMG" -n "$LIMIT" \
		-K "$KEY_AT,$KEYS" "${WREMU_ARGS[@]}" 2>&1 \
		| sed -n '/lcd:/,/serial output/p' > "$OUT"
fi
