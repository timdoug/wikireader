#!/bin/bash
# Run wiki.app inside wremu against a pristine copy of the card image.
#
# The firmware writes back to the card (history, settings), so every run must
# start from the same snapshot or results drift between invocations.
#
# usage: run-firmware-test.sh <wiki.app> <output.txt> [keys]
set -e

APP="$1"
OUT="$2"
KEYS="${3:-LOVE}"

SNAP="${CARD_SNAPSHOT:-/tmp/card_snap.img}"
IMG=$(mktemp /tmp/card_test.XXXXXX.img)
MNT=$(mktemp -d /tmp/wrmnt.XXXXXX)
HERE=$(cd "$(dirname "$0")/.." && pwd)

cleanup() {
	diskutil unmount force "$MNT" >/dev/null 2>&1 || true
	[ -n "$DEV" ] && hdiutil detach "$DEV" -force >/dev/null 2>&1 || true
	rm -rf "$IMG" "$MNT"
}
trap cleanup EXIT

cp "$SNAP" "$IMG"
DEV=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$IMG" | awk 'NR==1 {print $1}')
diskutil mount -mountPoint "$MNT" "$DEV" >/dev/null
cp "$APP" "$MNT/wiki.app"
sync
diskutil unmount "$MNT" >/dev/null
hdiutil detach "$DEV" >/dev/null
DEV=

"$HERE/wremu" -c "$IMG" -n 200000000 -K 20000000,"$KEYS" "$HERE/images/grifo.elf" 2>&1 \
	| sed -n '/lcd:/,$p' > "$OUT"
