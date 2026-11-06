#!/bin/sh
# prepare-source.sh SRC TARBALL OVERLAYS [PATCHES]
#
# Make the work tree SRC exactly the upstream TARBALL with this repository's
# overlays copied in, the port's own new files, and PATCHES/*/*.patch, its
# changes to upstream's files, applied in number order across the groups --
# and nothing else, so the repository is the only source of truth and the
# tree can be thrown away.  Build directories at SRC's top (build, build-*)
# are kept.
#
# OVERLAYS is a colon-separated list of directories, each copied over the
# tree's top, or DIR=SUB, copied into the tree's SUB.  PATCHES is one too,
# of patch directories, whose */*.patch are taken, and single patches; all
# are applied in the order of their file names.
#
# SRC.state records the inputs and every file's size and time as this left
# the tree.  When both still match, the tree is left alone.  Otherwise a
# fresh tree is made from the inputs and synced into SRC by content: only
# files whose contents differ are rewritten (with the current time, so make
# rebuilds what uses them), files the inputs no longer have are deleted,
# and the rest keep their timestamps, so an incremental build stays
# incremental.
set -eu
src=$1 tarball=$2 overlays=$3 patches=${4:-}
state=$src.state

overlay_list() {
	printf '%s\n' "$overlays" | tr ':' '\n' | while read -r o; do
		case $o in
		*=*) echo "${o%%=*} ${o#*=}" ;;
		?*) echo "$o ." ;;
		esac
	done
}

patch_list() {
	printf '%s\n' "$patches" | tr ':' '\n' | while read -r d; do
		if [ -d "$d" ]; then
			for p in "$d"/*/*.patch; do
				[ -e "$p" ] && echo "${p##*/} $p"
			done
		elif [ -n "$d" ]; then
			echo "${d##*/} $d"
		fi
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
		cksum < "$0"
		overlay_list | while read -r dir sub; do
			echo "overlay $sub"
			(cd "$dir" && find . -type f | LC_ALL=C sort | while read -r f; do
				echo "$f"; cksum < "$f"
			done)
		done
		for p in $(patch_list); do echo "${p##*/}"; cksum < "$p"; done
	} | cksum | cut -d' ' -f1
)

if [ "$(cat "$state" 2>/dev/null)" = "$inputs $(tree_stat)" ]; then
	exit 0
fi

echo "==> making ${src##*/} from the tarball, overlays and patches"
fresh=$src.fresh
rm -rf "$fresh"
mkdir -p "$fresh" "$src"
tar -xf "$tarball" -C "$fresh" --strip-components=1
overlay_list | while read -r dir sub; do
	mkdir -p "$fresh/$sub"
	cp -R "$dir/." "$fresh/$sub/"
done
for p in $(patch_list); do
	patch -d "$fresh" -p1 --forward --silent --no-backup-if-mismatch < "$p" || {
		echo "cannot apply $p" >&2
		exit 1
	}
done
rsync -rlc --delete --exclude=/build --exclude='/build-*' "$fresh/" "$src/"
rm -rf "$fresh"
echo "$inputs $(tree_stat)" > "$state"
