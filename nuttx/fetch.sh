#!/bin/sh
# Fetch the upstream trees this port builds against and lay the port over them.
#
# Copyright (c) 2026 Tim Douglas
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The wikireader repository carries only the port: overlay/ holds the files
# that do not exist upstream, patches/ holds diffs against the revisions named
# in revisions.  This script reconstructs a buildable tree in work/:
#
#     work/nuttx          apache/nuttx        + patches/nuttx.patch  + overlay/nuttx
#     work/nuttx/apps     apache/nuttx-apps                          + overlay/apps
#     work/nuttx/tinycc   repo.or.cz/tinycc   + patches/tinycc.patch + overlay/tinycc
#
# apps/ and tinycc/ sit inside the NuttX root because that is where
# tools/configure.sh -a apps and CONFIG_INTERPRETERS_TCC_SRCDIR look for them.
#
# Re-running this is safe: an already-applied patch is detected and skipped and
# the overlay copy is idempotent.  It will not, however, overwrite work-tree
# edits with the committed port unless --force is given -- see "make
# update-port" for the other direction.

set -e

here=$(cd "$(dirname "$0")" && pwd)
work="${here}/work"
force=no
venv=yes

usage()
{
	cat <<EOF
usage: $0 [--force] [--no-venv]

  --force     re-apply overlay/ and patches/ over an existing work tree,
              discarding any local edits to the files the port owns
  --no-venv   skip creating work/nuttx/.venv (kconfiglib and kconfig-tweak)
EOF
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	--force)   force=yes ;;
	--no-venv) venv=no ;;
	-h|--help) usage ;;
	*)         echo "$0: unknown option '$1'" >&2; usage ;;
	esac
	shift
done

note() { printf '==> %s\n' "$*"; }

# The revision list drives everything.  Skip comments and blank lines.
revisions()
{
	sed -e 's/#.*//' "${here}/revisions" | awk 'NF == 3 { print }'
}

# The upstream URL for a component, so that a clone taken from a local mirror
# still ends up with origin pointing at the real thing.
revision_url()
{
	revisions | awk -v n="$1" '$1 == n { print $2 }'
}

# Where each component's tree lives, relative to work/.
tree_for()
{
	case "$1" in
	nuttx)  echo "${work}/nuttx" ;;
	apps)   echo "${work}/nuttx/apps" ;;
	tinycc) echo "${work}/nuttx/tinycc" ;;
	*)      echo "${here}: no path known for component '$1'" >&2; exit 1 ;;
	esac
}

clone()
{
	name="$1" url="$2" rev="$3" dir="$4"

	# NuttX alone is several hundred megabytes.  If a copy of the same
	# repository is already on this machine, clone from that instead, which
	# also makes a rebuild possible with no network at all.
	if [ -n "${NUTTX_GIT_MIRROR_DIR:-}" ] &&
	   [ -d "${NUTTX_GIT_MIRROR_DIR}/${name}.git" ]; then
		url="${NUTTX_GIT_MIRROR_DIR}/${name}.git"
	fi

	note "cloning ${name} from ${url}"
	mkdir -p "$(dirname "${dir}")"
	git clone "${url}" "${dir}"
	git -C "${dir}" remote set-url origin "$(revision_url "${name}")"
	git -C "${dir}" checkout --quiet --detach "${rev}"
}

# Apply patches/<name>.patch unless it is already in place.  git apply
# --reverse --check succeeds exactly when the patch has been applied, which is
# what makes re-running this script harmless.
apply_patch()
{
	name="$1" dir="$2"
	patch="${here}/patches/${name}.patch"

	[ -f "${patch}" ] || return 0

	if git -C "${dir}" apply --reverse --check "${patch}" 2>/dev/null; then
		note "${name}: patch already applied"
		return 0
	fi

	note "${name}: applying $(basename "${patch}")"
	git -C "${dir}" apply "${patch}"
}

copy_overlay()
{
	name="$1" dir="$2"
	src="${here}/overlay/${name}"

	[ -d "${src}" ] || return 0

	note "${name}: copying overlay"
	# tar rather than cp -R so that the copy merges into existing
	# directories on every platform and preserves modes.
	( cd "${src}" && tar cf - . ) | ( cd "${dir}" && tar xf - )
}

revisions | while read -r name url rev; do
	dir=$(tree_for "${name}")

	if [ ! -d "${dir}/.git" ]; then
		clone "${name}" "${url}" "${rev}" "${dir}"
	else
		have=$(git -C "${dir}" rev-parse HEAD)
		if [ "${have}" != "${rev}" ]; then
			note "${name}: at ${have}, want ${rev}"
			if [ "${force}" = no ]; then
				echo "  refusing to move an existing tree; re-run with --force" >&2
				exit 1
			fi
			git -C "${dir}" fetch --quiet origin "${rev}" ||
				git -C "${dir}" fetch --quiet origin
			git -C "${dir}" checkout --quiet --detach "${rev}"
		elif [ "${force}" = no ] &&
		     ! git -C "${dir}" diff --quiet; then
			note "${name}: work tree has local edits, leaving it alone"
			note "${name}: re-run with --force to overwrite, or use 'make update-port'"
			continue
		fi
	fi

	apply_patch "${name}" "${dir}"
	copy_overlay "${name}" "${dir}"
done

# kconfiglib supplies the configuration front end and kconfig-tweak, which
# NuttX's tools/configure.sh calls.  Neither is packaged on macOS; a private
# virtual environment beside the NuttX root keeps them off the system Python.
# The kconfig-tweak template comes from patacongo/tools, the source NuttX's
# own installation guide points at.
if [ "${venv}" = yes ] && [ ! -x "${work}/nuttx/.venv/bin/kconfig-tweak" ]; then
	note "creating work/nuttx/.venv (kconfiglib, kconfig-tweak)"
	python3 -m venv "${work}/nuttx/.venv"
	"${work}/nuttx/.venv/bin/python" -m pip install --quiet kconfiglib==14.1.0

	tools="${work}/kconfig-tools"
	if [ ! -d "${tools}/.git" ]; then
		git clone --quiet https://github.com/patacongo/tools.git "${tools}"
	fi
	git -C "${tools}" checkout --quiet --detach \
		9484147c12d051014f854852d59c21d75a9616bd
	sed 's/@CONFIG_@/CONFIG_/g' \
		"${tools}/kconfig-frontends/utils/kconfig-tweak.in" \
		> "${work}/nuttx/.venv/bin/kconfig-tweak"
	chmod +x "${work}/nuttx/.venv/bin/kconfig-tweak"
fi

note "work tree ready: ${work}/nuttx"
