#!/bin/sh
# prepare-source.sh SRC TARBALL FILES GLUE [PATCHES]
#
# Make the work tree SRC exactly the upstream TARBALL with this repository's
# FILES/ copied in, GLUE (a Python script taking the tree) run over it, and
# PATCHES/*/*.patch applied in number order across the groups -- and nothing
# else, so the repository is the only source of truth and the tree can be
# thrown away.  Build directories at SRC's top (build, build-*) are kept.
#
# The prepared tree is made afresh in SRC.prepared whenever those inputs
# change, then synced into SRC by content: only files whose contents differ
# are rewritten (with the current time, so make rebuilds what uses them),
# files the inputs no longer have are deleted, and the rest keep their
# timestamps, so an incremental build stays incremental.
set -eu
src=$1 tarball=$2 files=$3 glue=$4 patches=${5:-}
prepared=$src.prepared

patch_list() {
	[ -n "$patches" ] || return 0
	for p in "$patches"/*/*.patch; do
		[ -e "$p" ] && echo "${p##*/} $p"
	done | sort | cut -d' ' -f2
}

inputs=$(
	{
		cksum < "$tarball"
		(cd "$files" && find . -type f | LC_ALL=C sort | while read -r f; do
			echo "$f"; cksum < "$f"
		done)
		cksum < "$glue"
		cksum < "$0"
		for p in $(patch_list); do echo "${p##*/}"; cksum < "$p"; done
	} | cksum | cut -d' ' -f1
)

if [ "$(cat "$prepared/.c33-inputs" 2>/dev/null)" != "$inputs" ]; then
	echo "==> preparing ${src##*/} from the tarball, files/, glue and patches"
	rm -rf "$prepared" "$prepared.tmp"
	mkdir -p "$prepared.tmp"
	tar -xf "$tarball" -C "$prepared.tmp" --strip-components=1
	cp -R "$files/." "$prepared.tmp/"
	python3 "$glue" "$prepared.tmp" >/dev/null
	for p in $(patch_list); do
		patch -d "$prepared.tmp" -p1 --forward --silent < "$p" || {
			echo "cannot apply $p" >&2
			exit 1
		}
	done
	echo "$inputs" > "$prepared.tmp/.c33-inputs"
	mv "$prepared.tmp" "$prepared"
fi

mkdir -p "$src"
rsync -rlc --delete --exclude=/build --exclude='/build-*' \
	--exclude=/.c33-inputs "$prepared/" "$src/"
