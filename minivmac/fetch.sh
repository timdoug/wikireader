#!/bin/sh
# Reconstruct the pinned Mini vMac source tree used by the WikiReader port.
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work="$here/work/minivmac"
revision=$(sed -n '1p' "$here/revision")

if [ ! -d "$work/.git" ]; then
	mkdir -p "$here/work"
	git clone https://github.com/minivmac/minivmac.git "$work"
fi

if ! git -C "$work" cat-file -e "$revision^{commit}" 2>/dev/null; then
	git -C "$work" fetch --depth 1 origin "$revision"
fi
git -C "$work" checkout --detach --force "$revision"
git -C "$work" apply "$here/patches/wikireader-fast-m68k.patch"

cc -O2 -o "$work/setup_t" "$work/setup/tool.c"
(
	cd "$work"
	./setup_t -t ndsa -m Plus -mem 4M -sound 0 -speed a -sony-tag 1 \
		-fullscreen 1 -var-fullscreen 0 -magnify 0 -lang eng > setup.sh
	sh setup.sh
)

test "$(git -C "$work" rev-parse HEAD)" = "$revision"
printf 'Mini vMac source ready at %s\n' "$work"
