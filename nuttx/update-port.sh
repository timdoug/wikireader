#!/bin/sh
# Write the work tree's changes back into overlay/ and patches/.
#
# Copyright (c) 2026 Tim Douglas
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This is the inverse of fetch.sh and the normal way to record a change: edit
# in work/, build and test there, then run this (make update-port) and commit
# the result.  Every changed file is classified against the pinned revision in
# revisions:
#
#   a file that does not exist upstream  -> copied whole into overlay/<name>/
#   a file that does                     -> diffed into patches/<name>.patch
#
# Committed work-tree changes and uncommitted ones are both picked up, so the
# nested checkouts do not have to be kept tidy.  Files git ignores are skipped,
# which is what keeps build output out of the port.
#
# Deleting a file needs a hand: remove it from overlay/ yourself.  Nothing here
# can tell "deleted on purpose" from "never existed".

set -e

here=$(cd "$(dirname "$0")" && pwd)
work="${here}/work"

note() { printf '==> %s\n' "$*"; }

tree_for()
{
	case "$1" in
	nuttx)  echo "${work}/nuttx" ;;
	apps)   echo "${work}/nuttx/apps" ;;
	tinycc) echo "${work}/nuttx/tinycc" ;;
	*)      echo "${here}: no path known for component '$1'" >&2; exit 1 ;;
	esac
}

sed -e 's/#.*//' "${here}/revisions" | awk 'NF == 3 { print }' |
while read -r name url rev; do
	dir=$(tree_for "${name}")
	if [ ! -d "${dir}/.git" ]; then
		echo "$0: ${dir} is not a checkout; run ./fetch.sh first" >&2
		exit 1
	fi

	# Everything the port touches, as two lists.  "git diff <rev>" compares
	# the working tree against the pinned revision, so it covers committed
	# and uncommitted changes in one pass; untracked files are added
	# separately because a diff cannot see them.
	added=$( {
		git -C "${dir}" diff --diff-filter=A --name-only "${rev}"
		git -C "${dir}" ls-files --others --exclude-standard
	} | sort -u )
	modified=$(git -C "${dir}" diff --diff-filter=M --name-only "${rev}" | sort -u)

	# Refresh overlay/<name>/ from the added files.  The directory is
	# rebuilt rather than merged so that a file that stopped being part of
	# the port disappears from the overlay too.
	overlay="${here}/overlay/${name}"
	rm -rf "${overlay}"
	count=0
	for f in ${added}; do
		mkdir -p "${overlay}/$(dirname "${f}")"
		cp "${dir}/${f}" "${overlay}/${f}"
		count=$((count + 1))
	done

	# ...and patches/<name>.patch from the modified ones.  An empty patch
	# is removed rather than committed, so a component with nothing but new
	# files (apps) carries no patch file at all.
	patch="${here}/patches/${name}.patch"
	if [ -n "${modified}" ]; then
		mkdir -p "${here}/patches"
		# shellcheck disable=SC2086  # modified is a deliberate file list
		git -C "${dir}" diff --binary "${rev}" -- ${modified} > "${patch}"
		lines=$(wc -l < "${patch}" | tr -d ' ')
	else
		rm -f "${patch}"
		lines=0
	fi

	note "${name}: ${count} overlay files, ${lines} patch lines"
done

note "port updated; review with 'git -C $(dirname "${here}") diff'"
