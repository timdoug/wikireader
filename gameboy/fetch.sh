#!/bin/sh
# Check out the pinned Peanut-GB used by the WikiReader port, and patch it.
# SPDX-License-Identifier: MIT
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work="$here/work/Peanut-GB"
revision=$(sed -n '1p' "$here/revision")

if [ ! -d "$work/.git" ]; then
	mkdir -p "$here/work"
	git clone https://github.com/deltabeard/Peanut-GB.git "$work"
fi
if ! git -C "$work" cat-file -e "$revision^{commit}" 2>/dev/null; then
	git -C "$work" fetch --depth 1 origin "$revision"
fi
git -C "$work" checkout --quiet --detach --force "$revision"
test "$(git -C "$work" rev-parse HEAD)" = "$revision"
for patch in "$here"/patches/*.patch; do
	git -C "$work" apply "$patch"
done
