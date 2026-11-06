#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
cross=${C33_TOOLCHAIN_WORK:-$work/toolchain}/install/bin/c33-linux-musl-
out=$root/linux/artifacts/unicode-tests
mkdir -p "$out"
cc -O2 -g -Wall -Wextra -Werror "$root/linux/console/terminal-test.c" \
	-o "$out/host-terminal-test"
"$out/host-terminal-test"
"$out/host-terminal-test" --render > "$out/expected.raw"
"$out/host-terminal-test" --demo > "$out/demo.txt"
for test in terminal-test unicode-pty-test render-bench; do
	"${cross}gcc" -Os -g -Wall -Wextra -Werror \
		"$root/linux/console/$test.c" -o "$out/$test"
done
