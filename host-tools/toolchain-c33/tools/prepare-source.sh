#!/bin/sh
# prepare-source.sh SRC TARBALL FILES GLUE [PATCHES]
#
# Make the work tree SRC exactly the upstream TARBALL with this repository's
# FILES/ copied in, GLUE (a Python script taking the tree) run over it, and
# PATCHES/*/*.patch applied in number order across the groups -- and nothing
# else, so the repository is the only source of truth and the tree can be
# thrown away.  Build directories at SRC's top (build, build-*) are kept.
#
# PREPARE_EXTRA, a colon-separated list of files and directories, names
# further inputs a GLUE reads, so that a change to them remakes the tree.
#
# SRC.state records the inputs and every file's size and time as this left
# the tree.  When both still match, the tree is left alone.  Otherwise a
# fresh tree is made from the inputs and synced into SRC by content: only
# files whose contents differ are rewritten (with the current time, so make
# rebuilds what uses them), files the inputs no longer have are deleted,
# and the rest keep their timestamps, so an incremental build stays
# incremental.
set -eu
src=$1 tarball=$2 files=$3 glue=$4 patches=${5:-}
state=$src.state

patch_list() {
	[ -n "$patches" ] || return 0
	for p in "$patches"/*/*.patch; do
		[ -e "$p" ] && echo "${p##*/} $p"
	done | sort | cut -d' ' -f2
}

# Each file's size and modification time, build directories aside.
tree_stat() {
	[ -d "$src" ] || { echo none; return; }
	python3 - "$src" <<'EOF'
import os, sys, zlib
top = sys.argv[1]
h = 0
for root, dirs, files in os.walk(top):
    if root == top:
        dirs[:] = [d for d in dirs if d != "build" and not d.startswith("build-")]
    dirs.sort()
    for name in sorted(files):
        st = os.lstat(os.path.join(root, name))
        h = zlib.crc32(f"{root}/{name} {st.st_size} {st.st_mtime_ns}\n".encode(), h)
print(h)
EOF
}

inputs=$(
	{
		cksum < "$tarball"
		(cd "$files" && find . -type f | LC_ALL=C sort | while read -r f; do
			echo "$f"; cksum < "$f"
		done)
		cksum < "$glue"
		cksum < "$0"
		for x in $(printf '%s' "${PREPARE_EXTRA:-}" | tr ':' ' '); do
			if [ -d "$x" ]; then
				(cd "$x" && find . -type f | LC_ALL=C sort | while read -r f; do
					echo "$f"; cksum < "$f"
				done)
			else
				echo "$x"; cksum < "$x"
			fi
		done
		for p in $(patch_list); do echo "${p##*/}"; cksum < "$p"; done
	} | cksum | cut -d' ' -f1
)

if [ "$(cat "$state" 2>/dev/null)" = "$inputs $(tree_stat)" ]; then
	exit 0
fi

echo "==> making ${src##*/} from the tarball, files/, glue and patches"
fresh=$src.fresh
rm -rf "$fresh"
mkdir -p "$fresh" "$src"
tar -xf "$tarball" -C "$fresh" --strip-components=1
cp -R "$files/." "$fresh/"
python3 "$glue" "$fresh" >/dev/null
for p in $(patch_list); do
	patch -d "$fresh" -p1 --forward --silent < "$p" || {
		echo "cannot apply $p" >&2
		exit 1
	}
done
rsync -rlc --delete --exclude=/build --exclude='/build-*' "$fresh/" "$src/"
rm -rf "$fresh"
echo "$inputs $(tree_stat)" > "$state"
