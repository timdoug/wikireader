#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel=$root/linux/artifacts/vmlinux
emulator=$root/emulator/wremu
fixture_tool=$root/nuttx/overlay/nuttx/boards/c33/s1c33e07/wikireader/tools/make_boot_fixture.py
fat_helper=$root/emulator/tools/mem_dma_bench/run.py
touch_output="touch-keyboard pass"
touch_latency_limit=8000000

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
	# 500M retired instructions. UART proves the serial recovery path first;
	# scripted panel taps then type into the userspace PTY console.
	WREMU_UART_TRACE="$touch_output|/ # =" "$emulator" -n 800000000 \
		-e "$work/fixture/flash-nuttx.rom" \
		-c "$work/fixture/nuttx-card.img" \
		--uart-input "$work/uart.in" --uart-start 500000000 \
		-K "520000000,ecj<ho touch-keyboard pass#" \
		-T 36,197,690000000 -T 108,175,700000000 \
		-T 228,197,710000000
) >"$work/boot.log" 2>&1

syscall_marker="C33: entered userspace syscall path"
expected="*** HARDWARE PASS: BusyBox 1.38 is PID 1 on native C33 Linux ***"
irq_controller_expected="C33 IRQ: registered 24 interrupt sources"
irq_userspace_expected="C33 IRQ: generic registrations"
dma_irq_expected='[[:space:]]25:[[:space:]]+[1-9][0-9]*[[:space:]]+S1C33-ITC[[:space:]]+s1c33-spi-rx'
framebuffer_expected="C33 framebuffer: /dev/fb0 240x208 mono read/write passed"
tux_expected="s1c33-fb s1c33-fb: registered /dev/fb0, 240x208 mono; Tux logo shown"
input_expected="C33 input: /dev/input/event0 absolute touchscreen registered"
evdev_expected="C33 input: userspace console received evdev touch events"
init_expected="C33 BusyBox init: PID 1 userspace started"
diagnostic_expected="C33 BusyBox init: diagnostic child passed"
busybox_expected="C33 BusyBox recovery suite passed: hush + file/text/archive tools"
shell_ready="C33 BusyBox shell ready on ttyC0"
shell_expected="C33 INTERACTIVE HUSH PASS"
userspace_console_expected="C33 userspace console: fbdev + evdev + PTY shell ready"
sd_expected="C33 MMC/SPI: mounted /dev/mmcblk0p1 and persisted linux.ok"
sd_probe_expected="mmc0: new SDHC card on SPI"
sd_dma_expected="mmc_spi spi0.0: 32-bit HSDMA bulk reads use IRQ completion"
sd_clock_expected="--- spi clock: 0 unclamped disables with SD selected ---"
sd_width_expected='--- spi width: [0-9]+ 8-bit, [0-9]+ 16-bit, [1-9][0-9]* 32-bit characters ---'
sd_dma_channels_expected='--- dma channels: HSDMA2 TX [1-9][0-9]*, HSDMA3 RX [1-9][0-9]* ---'
process_expected="C33 process test: clone -> execve -> wait4 passed"
child_expected="C33 child: execve reached /child"
signal_expected="C33 signal test: handler -> rt_sigreturn passed"
libc_output='C33 uClibc smoke: pid=[1-9][0-9]* longjmp=7'
libc_expected="C33 libc test: crt -> stdio -> getpid -> longjmp passed"
clock_expected="C33 clock: registered 48000000 Hz MCLK"
gpio_expected="s1c33-gpio s1c33-gpio: registered 56 GPIOs through gpiolib"
if ! grep -F "$syscall_marker" "$work/boot.log" >/dev/null || \
   ! grep -F "$expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$irq_controller_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$irq_userspace_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$clock_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$gpio_expected" "$work/boot.log" >/dev/null || \
   ! grep -E "$dma_irq_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$framebuffer_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$tux_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$input_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$evdev_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "c33-timer" "$work/boot.log" >/dev/null || \
   ! grep -F "s1c33-uart0-rx" "$work/boot.log" >/dev/null || \
   ! grep -F "s1c33-uart1-error" "$work/boot.log" >/dev/null || \
   ! grep -F "s1c33-uart1-rx" "$work/boot.log" >/dev/null || \
   ! grep -F "registered UART1 as a tty-backed serdev controller" \
	"$work/boot.log" >/dev/null || \
   ! grep -F "wikireader-touch serial0-0: registered 240x208 touchscreen through serdev" \
	"$work/boot.log" >/dev/null || \
   ! grep -F "$init_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$diagnostic_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$busybox_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$shell_ready" "$work/boot.log" >/dev/null || \
   ! grep -F "$shell_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$userspace_console_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$sd_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$sd_probe_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$sd_dma_expected" "$work/boot.log" >/dev/null || \
   ! grep -F -- "$sd_clock_expected" "$work/boot.log" >/dev/null || \
   ! grep -E -- "$sd_width_expected" "$work/boot.log" >/dev/null || \
   ! grep -E -- "$sd_dma_channels_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$process_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$child_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$signal_expected" "$work/boot.log" >/dev/null || \
   ! grep -E "$libc_output" "$work/boot.log" >/dev/null || \
   ! grep -F "$libc_expected" "$work/boot.log" >/dev/null; then
	cat "$work/boot.log" >&2
	echo "Native Linux did not complete the PID 1 UART round trip." >&2
	exit 1
fi

touch_down=$(LC_ALL=C sed -n \
	"s/.*\[key '#' down at [0-9-]*,[0-9-]*, MCLK \([0-9][0-9]*\)\].*/\1/p" \
	"$work/boot.log" | tail -n 1)
touch_echo=$(LC_ALL=C sed -n \
	"s/.*\[uart line at MCLK \([0-9][0-9]*\)\] $touch_output/\1/p" \
	"$work/boot.log" | tail -n 1)
if [ -z "$touch_down" ] || [ -z "$touch_echo" ] || \
   [ "$touch_echo" -lt "$touch_down" ]; then
	echo "Touch-to-shell latency timestamps are missing or invalid." >&2
	cat "$work/boot.log" >&2
	exit 1
fi
touch_latency=$((touch_echo - touch_down))
if [ "$touch_latency" -gt "$touch_latency_limit" ]; then
	echo "Touch-to-shell latency is $touch_latency MCLK cycles; limit is $touch_latency_limit." >&2
	cat "$work/boot.log" >&2
	exit 1
fi
echo "Touch-to-shell latency passed: $touch_latency MCLK cycles"
if ! LC_ALL=C grep -E \
   '\[uart line at MCLK [0-9]+\] / # =$' "$work/boot.log" >/dev/null; then
	echo "The 123 keyboard page did not send '=' to the shell." >&2
	cat "$work/boot.log" >&2
	exit 1
fi
if grep -E "mmc[0-9]+: error -[0-9]+ whilst initialising" \
   "$work/boot.log" >/dev/null; then
	cat "$work/boot.log" >&2
	echo "Generic MMC required an avoidable card-initialisation retry." >&2
	exit 1
fi
if ! LC_ALL=C tr -d '\015' <"$work/boot.log" | \
	grep -Fx "$touch_output" >/dev/null; then
	cat "$work/boot.log" >&2
	echo "The userspace PTY keyboard did not execute its shell command." >&2
	exit 1
fi
if grep -F "Kernel panic" "$work/boot.log" >/dev/null; then
	cat "$work/boot.log" >&2
	echo "Native Linux panicked after starting PID 1." >&2
	exit 1
fi
if grep -F "binfmt_flat: Loading file:" "$work/boot.log" >/dev/null; then
	cat "$work/boot.log" >&2
	echo "A bFLT image unexpectedly enabled kernel load tracing." >&2
	exit 1
fi

if ! python3 "$root/linux/check-sd.py" "$fat_helper" \
	"$work/fixture/nuttx-card.img"; then
	cat "$work/boot.log" >&2
	echo "Native Linux did not persist its FAT status file." >&2
	exit 1
fi
python3 "$root/linux/check-lcd.py" "$work/screen.pgm" --stages 11 --symbols \
	--edited
cp "$work/screen.pgm" "$root/linux/artifacts/lcd-console.pgm"

grep -E "C33 Linux: entry|Linux version|Memory:|Calibrating delay loop|s1c33-spi|s1c33-fb|mmc_spi|mmcblk0|spi width:|dma channels:|C33 IRQ:|s1c33-spi-rx|c33-timer|s1c33-uart[01]|serdev|wikireader-touch|C33 input:|C33 framebuffer:|C33 PTY:|C33 userspace|Run /init|C33: entered userspace|C33 process test|C33 signal test|C33 uClibc smoke|C33 libc test|C33 diagnostic|C33 BusyBox|C33 MMC/SPI|HARDWARE PASS|INTERACTIVE HUSH|touch[- ]keyboard pass" \
	"$work/boot.log"
echo "Full-chain native C33 Linux boot passed."
