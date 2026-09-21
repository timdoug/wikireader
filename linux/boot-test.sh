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
printf 'echo C33 INTERACTIVE HUSH PASS\n' >"$work/uart.in"

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
expected="*** HARDWARE PASS: BusyBox 1.38 is PID 1 on native C33 Linux ***"
irq_expected="C33 UART: received vector 57 interrupt"
init_expected="C33 BusyBox init: PID 1 userspace started"
diagnostic_expected="C33 BusyBox init: diagnostic child passed"
shell_ready="C33 BusyBox shell ready on ttyC330"
shell_expected="C33 INTERACTIVE HUSH PASS"
process_expected="C33 process test: clone -> execve -> wait4 passed"
child_expected="C33 child: execve reached /child"
signal_expected="C33 signal test: handler -> rt_sigreturn passed"
libc_output='C33 uClibc smoke: pid=[1-9][0-9]* longjmp=7'
libc_expected="C33 libc test: crt -> stdio -> getpid -> longjmp passed"
if ! grep -F "$syscall_marker" "$work/boot.log" >/dev/null || \
   ! grep -F "$expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$irq_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$init_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$diagnostic_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$shell_ready" "$work/boot.log" >/dev/null || \
   ! grep -F "$shell_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$process_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$child_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$signal_expected" "$work/boot.log" >/dev/null || \
   ! grep -E "$libc_output" "$work/boot.log" >/dev/null || \
   ! grep -F "$libc_expected" "$work/boot.log" >/dev/null; then
	cat "$work/boot.log" >&2
	echo "Native Linux did not complete the PID 1 UART round trip." >&2
	exit 1
fi
if grep -F "Kernel panic" "$work/boot.log" >/dev/null; then
	cat "$work/boot.log" >&2
	echo "Native Linux panicked after starting PID 1." >&2
	exit 1
fi

python3 "$root/linux/check-lcd.py" "$work/screen.pgm" --stages 7
cp "$work/screen.pgm" "$root/linux/artifacts/lcd-console.pgm"

grep -E "C33 Linux: entry|Linux version|Memory:|Calibrating delay loop|C33 UART:|Run /init|binfmt_flat: Load|C33: entered userspace|C33 process test|C33 signal test|C33 uClibc smoke|C33 libc test|C33 diagnostic|C33 BusyBox|HARDWARE PASS|INTERACTIVE HUSH" \
	"$work/boot.log"
echo "Full-chain native C33 Linux boot passed."
