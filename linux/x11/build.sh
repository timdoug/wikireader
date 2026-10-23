#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
tool=${C33_TOOLCHAIN_WORK:-$work/toolchain}/install/bin/c33-linux-uclibc-
staging=${WR_BUILDROOT_BUILD:-$work/buildroot-build}/staging
out=$root/linux/artifacts/x11-tests
mkdir -p "$out"
"${tool}gcc" -Os -g -Wall -Wextra -Werror "$root/linux/x11/pty-check.c" \
    -o "$out/pty-check"
"${tool}gcc" --sysroot="$staging" -Os -g -Wall -Wextra -Werror \
    "$root/linux/x11/window-check.c" -L"$staging/usr/lib" \
    -Wl,-rpath-link,"$staging/usr/lib" -lX11 -o "$out/window-check"
