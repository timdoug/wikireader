#!/bin/sh
# apply-patches.sh SRC PATCHES: apply PATCHES/*/*.patch to the GCC tree SRC,
# in number order across the groups (patches/README.md).  The tree is kept
# between builds, so SRC/.c33-patches lists the patches applied to it, by
# checksum and name, as quilt keeps its series.  A patch the list lacks but
# the tree already has, from before the list was kept, is recorded too.
set -e
src=$1 dir=$2 list=$1/.c33-patches
touch "$list"
for p in $(for f in "$dir"/*/*.patch; do echo "${f##*/} $f"; done | sort | cut -d' ' -f2); do
	id="$(cksum < "$p" | cut -d' ' -f1) ${p##*/}"
	if grep -qxF "$id" "$list"; then
		continue
	elif patch -d "$src" -p1 --forward --dry-run --silent < "$p" >/dev/null; then
		patch -d "$src" -p1 --forward --silent < "$p"
	elif ! patch -d "$src" -p1 --reverse --dry-run --silent < "$p" >/dev/null; then
		echo "cannot apply $p to $src: extract the tree afresh" >&2
		exit 1
	fi
	echo "$id" >> "$list"
done
