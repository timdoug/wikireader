#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel=$root/linux/artifacts/vmlinux
emulator=$root/emulator/wremu
fixture_tool=$root/nuttx/overlay/nuttx/boards/c33/s1c33e07/wikireader/tools/make_boot_fixture.py

if [ ! -f "$kernel" ]; then
	echo "Kernel not found at $kernel" >&2
	echo "Run make -C linux build first." >&2
	exit 1
fi
if [ ! -x "$emulator" ]; then
	echo "Emulator not found at $emulator" >&2
	echo "Build emulator/wremu first." >&2
	exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/wr-linux-boot.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

python3 "$fixture_tool" --wikireader "$root" --image "$kernel" \
	--out "$work/fixture"
printf 'p' >"$work/uart.in"

(
	cd "$work"
	# Keep input out of the vendor menu and loader. PID 1 is running before
	# 500M retired instructions; the remaining 60M cover RX and its reply.
	"$emulator" -R -n 560000000 \
		-e "$work/fixture/flash-nuttx.rom" \
		-c "$work/fixture/nuttx-card.img" \
		--uart-input "$work/uart.in" --uart-start 500000000
) >"$work/boot.log" 2>&1

syscall_marker="C33: entered userspace syscall path"
expected="WikiReader native C33 userspace is alive!"
rx_expected="UART RX reached Linux userspace."
irq_expected="C33 UART: received vector 57 interrupt"
shell_expected="pid 1"
if ! grep -F "$syscall_marker" "$work/boot.log" >/dev/null || \
   ! grep -F "$expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$irq_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$rx_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$shell_expected" "$work/boot.log" >/dev/null; then
	cat "$work/boot.log" >&2
	echo "Native Linux did not complete the PID 1 UART round trip." >&2
	exit 1
fi
if grep -F "Kernel panic" "$work/boot.log" >/dev/null; then
	cat "$work/boot.log" >&2
	echo "Native Linux panicked after starting PID 1." >&2
	exit 1
fi

grep -E "C33 Linux: entry|Linux version|Memory:|Calibrating delay loop|C33 UART:|Run /init|binfmt_flat: Load|C33: entered userspace|WikiReader native|c33 shell:|UART RX reached|pid 1" \
	"$work/boot.log"
echo "Full-chain native C33 Linux boot passed."
