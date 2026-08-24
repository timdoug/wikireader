#!/bin/bash
# Which opcodes the differential tests actually execute, versus the firmware.
# Requires a prior run.sh (the ELFs are reused from $DT_WORK).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WREMU="$HERE/../wremu"
for w in "$@"; do
	for elf in "$w"/t*.elf; do
		[ -e "$elf" ] || continue
		"$WREMU" -P -n 400000000 "$elf" 2>/dev/null | grep '^OP '
	done
done | awk '{c[$2]+=$3} END {for (k in c) printf "%s %d\n", k, c[k]}'
