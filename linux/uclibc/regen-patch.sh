#!/bin/sh
# regen-patch.sh NNNN EDIT [HEADER]: rewrite uClibc-ng patch NNNN as its
# current change plus what the shell script EDIT does (run in the tree, with
# the series before NNNN and NNNN itself applied), keeping its header, or
# taking HEADER's text instead.  Then check that the whole series applies to
# the pinned revision.  Runs in the VM, in a scratch worktree of uclibc-ng.
set -eu
num=$1 edit=$(realpath "$2") header=${3:+$(realpath "$3")}
here=$(cd "$(dirname "$0")" && pwd)
repo=${WR_UCLIBC_SOURCE:-${WR_LINUX_GUEST_ROOT:-$HOME/wr-linux}/uclibc-ng}
work=$(mktemp -d /tmp/regen-patch.XXXXXX)
trap 'rm -rf "$work"; git -C "$repo" worktree prune' EXIT

series=$(for f in "$here"/patches/*/*.patch; do echo "${f##*/} $f"; done |
	sort | cut -d' ' -f2)
git -C "$repo" worktree add -q --detach "$work/tree" HEAD
cd "$work/tree"
git config user.email regen@localhost
git config user.name regen-patch
found=
for p in $series; do
	case "${p##*/}" in
	"$num"-*)
		found=$p
		git add -A
		git commit -qm base --allow-empty
		git apply "$p"
		sh "$edit"
		if [ -n "$header" ]; then
			cat "$header" > "$work/new"
		else
			sed '/^diff --git /,$d' "$p" > "$work/new"
		fi
		git add -A
		git diff --cached >> "$work/new"
		mv "$work/new" "$p"
		git commit -qm "$num" ;;
	*)
		git apply "$p" ;;
	esac
done
[ -n "$found" ] || { echo "no patch $num" >&2; exit 1; }

cd "$work"
rm -rf tree
git -C "$repo" worktree prune
git -C "$repo" worktree add -q --detach "$work/check" HEAD
for p in $series; do
	git -C "$work/check" apply "$p" || { echo "$p no longer applies" >&2; exit 1; }
done
echo "rewrote ${found##*/}; the series applies"
