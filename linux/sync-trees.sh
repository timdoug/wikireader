#!/bin/sh
# sync-trees.sh [linux] [uclibc] [musl] [buildroot]: make each pinned upstream tree
# in the VM exactly its revision plus this repository's patches and overlay
# (sync-tree.py), so that nothing in the VM is a source.  fetch.sh fetches
# the revisions; every build syncs the trees it reads first.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=${WR_LINUX_GUEST_ROOT:-${WR_LINUX_WORK:-${HOME}/wr-linux}}
linux_src=${WR_LINUX_SOURCE:-$work/linux-src}
uclibc_src=${WR_UCLIBC_SOURCE:-$work/uclibc-ng}
musl_src=${WR_MUSL_SOURCE:-$work/musl}
buildroot_src=${WR_BUILDROOT_SOURCE:-$work/buildroot}

# Patches are grouped by kind (patches/README.md) and applied in number
# order across the groups: a later patch may build on an earlier one.
ordered() {
	for p in "$@"; do
		[ -e "$p" ] && echo "${p##*/} $p"
	done | sort | cut -d' ' -f2
}

[ $# -gt 0 ] || set -- linux uclibc musl buildroot
for t in "$@"; do
		case $t in
		linux)
			python3 "$here/sync-tree.py" "$linux_src" \
				--overlay "$here/overlay" \
				$(ordered "$here"/patches/*/*.patch) ;;
		uclibc)
			python3 "$here/sync-tree.py" "$uclibc_src" \
				--overlay "$here/uclibc/overlay" --overlay-first \
				$(ordered "$here"/uclibc/patches/*/*.patch) ;;
		musl)
			python3 "$here/sync-tree.py" "$musl_src" \
				--overlay "$here/musl/overlay" \
				$(ordered "$here"/musl/patches/*/*.patch) ;;
		buildroot)
			# Downloaded source tarballs in dl/ are kept.
			python3 "$here/sync-tree.py" "$buildroot_src" --keep /dl \
				$(ordered "$here"/buildroot/patches/*.patch) ;;
		*)
			echo "sync-trees.sh: no tree $t" >&2
			exit 1 ;;
		esac
done
