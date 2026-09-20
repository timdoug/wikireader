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

set -- $(sed -e 's/#.*//' "${here}/revisions" | awk '$1 == "linux" { print $2, $3 }')
url=$1
revision=$2

if [ ! -d "${src}/.git" ]; then
	mkdir -p "${work}"
	git clone --depth=1 --branch="${revision}" "${url}" "${src}"
else
	git -C "${src}" fetch --depth=1 origin "${revision}"
	git -C "${src}" checkout --detach FETCH_HEAD
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
