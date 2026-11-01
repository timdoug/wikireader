#!/bin/sh
# Build drmbench for the WikiReader, inside the wr-linux VM, against the
# Buildroot staging tree's kernel headers.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
tool=${C33_TOOLCHAIN_WORK:-$work/toolchain}/install/bin/c33-linux-uclibc-
staging=${WR_BUILDROOT_BUILD:-$work/buildroot-build}/staging
out=$root/linux/artifacts/drm
mkdir -p "$out"
"${tool}gcc" --sysroot="$staging" -O2 -Wall -Wextra -Werror \
    "$root/linux/drm/drmbench.c" -o "$out/drmbench"
