#!/bin/sh
# Reconstruct the pinned upstream Linux tree on the VM's native filesystem.
set -eu

if [ "$(uname -s)" != Linux ]; then
	echo "fetch.sh must run inside the wr-linux VM" >&2
	exit 1
fi

here=$(cd "$(dirname "$0")" && pwd)
work=${WR_LINUX_WORK:-${HOME}/wr-linux}
src=${work}/linux-src
uclibc_src=${work}/uclibc-ng
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

git -C "${src}" reset --hard HEAD
git -C "${src}" clean -fdx

for patch in "${here}"/patches/*.patch; do
	[ -e "${patch}" ] || break
	git -C "${src}" apply "${patch}"
done

if [ -d "${here}/overlay" ]; then
	cp -R "${here}/overlay/." "${src}/"
fi

printf '%s\n' "Linux source ready at ${src}"

set -- $(sed -e 's/#.*//' "${here}/revisions" | awk '$1 == "uclibc" { print $2, $3 }')
uclibc_url=$1
uclibc_revision=$2

if [ ! -d "${uclibc_src}/.git" ]; then
	git clone --depth=1 --branch="${uclibc_revision}" "${uclibc_url}" "${uclibc_src}"
else
	git -C "${uclibc_src}" fetch --depth=1 origin "${uclibc_revision}"
	git -C "${uclibc_src}" checkout --force --detach FETCH_HEAD
fi

git -C "${uclibc_src}" reset --hard HEAD
git -C "${uclibc_src}" clean -fdx

if [ -d "${here}/uclibc/overlay" ]; then
	cp -R "${here}/uclibc/overlay/." "${uclibc_src}/"
fi

for patch in "${here}"/uclibc/patches/*.patch; do
	[ -e "${patch}" ] || break
	git -C "${uclibc_src}" apply "${patch}"
done

printf '%s\n' "uClibc-ng source ready at ${uclibc_src}"

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

# Downloaded source tarballs in dl/ are kept; everything else is reset.
git -C "${buildroot_src}" reset --hard HEAD
git -C "${buildroot_src}" clean -fdx -e dl

for patch in "${here}"/buildroot/patches/*.patch; do
	[ -e "${patch}" ] || break
	git -C "${buildroot_src}" apply "${patch}"
done

printf '%s\n' "Buildroot source ready at ${buildroot_src}"
