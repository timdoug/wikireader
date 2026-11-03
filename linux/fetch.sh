#!/bin/sh
# Fetch the pinned upstream kernel, uClibc-ng, musl and Buildroot onto the VM's
# native filesystem, and make each its revision plus this repository's work.
set -eu

if [ "$(uname -s)" != Linux ]; then
	echo "fetch.sh must run inside the wr-linux VM" >&2
	exit 1
fi

here=$(cd "$(dirname "$0")" && pwd)
work=${WR_LINUX_WORK:-${HOME}/wr-linux}
src=${work}/linux-src
uclibc_src=${work}/uclibc-ng
musl_src=${work}/musl
buildroot_src=${work}/buildroot

set -- $(sed -e 's/#.*//' "${here}/revisions" | awk '$1 == "linux" { print $2, $3 }')
url=$1
revision=$2

if [ ! -d "${src}/.git" ]; then
	mkdir -p "${work}"
	git clone --depth=1 --branch="${revision}" "${url}" "${src}"
else
	git -C "${src}" fetch --depth=1 origin "${revision}"
	git -C "${src}" checkout --force --detach FETCH_HEAD
fi


set -- $(sed -e 's/#.*//' "${here}/revisions" | awk '$1 == "uclibc" { print $2, $3 }')
uclibc_url=$1
uclibc_revision=$2

if [ ! -d "${uclibc_src}/.git" ]; then
	git clone --depth=1 --branch="${uclibc_revision}" "${uclibc_url}" "${uclibc_src}"
else
	git -C "${uclibc_src}" fetch --depth=1 origin "${uclibc_revision}"
	git -C "${uclibc_src}" checkout --force --detach FETCH_HEAD
fi


set -- $(sed -e 's/#.*//' "${here}/revisions" | awk '$1 == "musl" { print $2, $3 }')
musl_url=$1
musl_revision=$2

if [ ! -d "${musl_src}/.git" ]; then
	git clone --depth=1 --branch="${musl_revision}" "${musl_url}" "${musl_src}"
else
	git -C "${musl_src}" fetch --depth=1 origin "${musl_revision}"
	git -C "${musl_src}" checkout --force --detach FETCH_HEAD
fi


set -- $(sed -e 's/#.*//' "${here}/revisions" | awk '$1 == "buildroot" { print $2, $3 }')
buildroot_url=$1
buildroot_revision=$2

if [ ! -d "${buildroot_src}/.git" ]; then
	git clone --depth=1 --branch="${buildroot_revision}" \
		"${buildroot_url}" "${buildroot_src}"
else
	git -C "${buildroot_src}" fetch --depth=1 origin "${buildroot_revision}"
	git -C "${buildroot_src}" checkout --force --detach FETCH_HEAD
fi

# Each tree becomes exactly its revision plus this repository's patches and
# overlay; builds do the same before they read a tree.
"${here}/sync-trees.sh" linux uclibc musl buildroot
