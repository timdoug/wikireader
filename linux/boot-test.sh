#!/bin/sh
set -eu

# The device's own boot: the MBR flash, Grifo as kernel.elf, init.app, and
# linux.app from a single-entry init.ini, which the launcher starts without
# drawing its menu.  Never bypass Grifo: the direct paths run at the 48 MHz
# reset clock and skip Grifo's hardware setup, so they test a machine that
# does not exist.

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
app=$root/linux/artifacts/linux.app
system=$root/linux/artifacts/linux.img
icon=$root/linux/artifacts/linux.ico
grifo=$root/samo-lib/grifo/grifo.elf
launcher=$root/samo-lib/grifo/applications/init/init.app
make_flash=$root/samo-lib/mbr/make-flash.py
emulator=$root/emulator/wremu
fat_helper=$root/emulator/tools/mem_dma_bench/run.py
touch_output="touch-keyboard pass"
touch_latency_limit=8000000

for input in "$app" "$system" "$icon" "$grifo" "$launcher"; do
	if [ ! -f "$input" ]; then
		echo "Boot test input not found: $input" >&2
		echo "Run make -C linux build, and build samo-lib's Grifo and init.app." >&2
		exit 1
	fi
done
if [ ! -x "$emulator" ]; then
	echo "Emulator not found at $emulator" >&2
	echo "Build emulator/wremu first." >&2
	exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/wr-linux-boot.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

# wr.selftest runs the userspace checks rcS skips on a normal boot, and
# loglevel=7 puts the whole boot log on the serial port for them.
python3 - "$fat_helper" "$work/card.img" "$grifo" "$launcher" "$app" \
	"$system" "$icon" <<'PYEOF'
import importlib.util, sys
from pathlib import Path
helper, card, grifo, launcher, app, system, icon = sys.argv[1:]
spec = importlib.util.spec_from_file_location("wr_fat_fixture", helper)
fat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fat)
files = {
    "kernel.elf": Path(grifo).read_bytes(),
    "init.app": Path(launcher).read_bytes(),
    "linux.app": Path(app).read_bytes(),
    "linux.img": Path(system).read_bytes(),
    "linux.ico": Path(icon).read_bytes(),
    "init.ini": b"linux.ico : linux.app loglevel=7 wr.selftest\n",
}
fat.make_image(Path(card), files, 64)
for name, data in files.items():
    if fat.read_file(Path(card), name) != data:
        sys.exit(f"Boot test card did not read back {name}")
PYEOF
python3 "$make_flash" "$work/flash.rom" >/dev/null
printf 'echo C33 INTERACTIVE HUSH PASS\n' >"$work/uart.in"

(
	cd "$work"
	# Keep input out of Grifo and the launcher: the serial shell is up by
	# UART_START.  UART proves the serial recovery path first; scripted
	# panel taps then type into the userspace PTY console.
	# The soft keyboard types into a PTY whose shell is still starting;
	# anything typed before hush sets its terminal up is discarded, so
	# leave the keys well clear of that.
	# The buttons are polled every 50 ms; hold the scripted press longer.
	WREMU_BUTTON_HOLD_MS=200 \
	WREMU_UART_TRACE="${UART_TRACE-$touch_output|/ # =}" "$emulator" \
		-n "${LIMIT:-900000000}" \
		-e "$work/flash.rom" -c "$work/card.img" \
		--uart-input "$work/uart.in" --uart-start "${UART_START:-500000000}" \
		-K "${KEYS_AT:-600000000},ecj<ho touch-keyboard pass#" \
		-T 36,197,${TAPS_AT:-780000000} -T 108,175,$((${TAPS_AT:-780000000} + 10000000)) \
		-T 228,197,$((${TAPS_AT:-780000000} + 20000000)) \
		-N 1,$((${TAPS_AT:-780000000} + 40000000))
) >"$work/boot.log" 2>&1
[ -z "${KEEP_LOG:-}" ] || cp "$work/boot.log" "$KEEP_LOG"

syscall_marker="C33: entered userspace syscall path"
boot_path_expected="C33 boot: Grifo application (incoming TTBR 00000400)"
expected="*** HARDWARE PASS: BusyBox 1.38 is PID 1 on native C33 Linux ***"
irq_controller_expected="C33 IRQ: registered 24 interrupt sources"
itc_expected="s1c33-itc: 24 interrupt sources, trap vector as hardware interrupt number"
irq_userspace_expected="C33 IRQ: generic registrations"
# The column after the chip is the hardware number, the trap vector.
framebuffer_expected="C33 framebuffer: /dev/fb0 240x208 mono read/write passed"
blank_expected="C33 display: fbdev blank and unblank passed"
seeded_clock_expected="C33 time: clock set from the card, @"
random_expected="C33 random: credited a 2048-bit seed from the last boot"
lcd_power_expected='--- lcd power: [1-9][0-9]* stops, [1-9][0-9]* starts, panel driving ---'
tux_expected="s1c33-fb s1c33-fb: registered /dev/fb0, 240x208 mono; Tux logo shown"
input_expected="C33 input: /dev/input/event[0-9]+ absolute touchscreen registered"
uinput_expected="C33 input: soft keyboard registered as a uinput device"
keyboard_expected="C33 input: keyboards feed the PTY through evdev"
button_expected="C33 input: front button search"
contrast_expected="wikireader-lcd wikireader-lcd: contrast 2048 of 4095 adopted from the PWM"
# wremu's converter reads 832 and 502: 3.08 V through the divider, 22 C.
sensors_expected="C33 sensors: battery 3083 mV, board 22 C"
# 2^30 counts of Grifo's 60 MHz MCLK.
watchdog_expected="s1c33-wdt s1c33-wdt: 60000000 Hz, up to 17895 ms a period"
evdev_expected="C33 input: userspace console received evdev touch events"
init_expected="C33 BusyBox init: PID 1 userspace started"
diagnostic_expected="C33 BusyBox init: kernel self-test passed"
busybox_expected="C33 BusyBox recovery suite passed: hush + file/text/archive tools"
shared_text_expected="C33 BusyBox shared text: [0-9a-f]+-[0-9a-f]+ and libc [0-9a-f]+-[0-9a-f]+ mapped once for every process"
shell_ready="C33 BusyBox shell ready on ttyC0"
shell_expected="C33 INTERACTIVE HUSH PASS"
userspace_console_expected="C33 userspace console: fbdev + evdev + PTY shell ready"
sd_expected="C33 MMC/SPI: mounted /dev/mmcblk0p1 and persisted linux.ok"
sd_probe_expected="mmc0: new SDHC card on SPI"
# Without the trailing ", no poweroff" the slot found its regulators.
# MCLK/4 of Grifo's 60 MHz.
sd_power_expected="s1c33-sd s1c33-sd: SD host mmc0 at up to 15000000 Hz, streamed HSDMA block reads"
hsdma_expected="s1c33-hsdma s1c33-hsdma: 4 channels, completion polled"
sd_clock_expected="--- spi clock: 0 unclamped disables with SD selected ---"
sd_width_expected='--- spi width: [0-9]+ 8-bit, [0-9]+ 16-bit, [1-9][0-9]* 32-bit characters ---'
sd_dma_channels_expected='--- dma channels: HSDMA2 TX [1-9][0-9]*, HSDMA3 RX [1-9][0-9]* ---'
process_expected="C33 process test: vfork -> execve -> wait4 passed"
child_expected="C33 child: execve reached the child"
signal_expected="C33 signal test: handler -> rt_sigreturn passed"
trace_expected="C33 trace test: PTRACE_SYSCALL stopped the child passed"
libc_output='C33 uClibc smoke: pid=[1-9][0-9]* longjmp=7'
libc_expected="C33 libc test: crt -> stdio -> getpid -> longjmp passed"
clock_expected="C33 clock: registered 60000000 Hz MCLK and 6 peripheral gates"
gpio_expected="s1c33-pinctrl s1c33-pinctrl: registered 74 pins, 111 functions and 56 GPIOs, P03 and P60..P62 interrupting"
if ! grep -F "$boot_path_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$syscall_marker" "$work/boot.log" >/dev/null || \
   ! grep -F "$expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$irq_controller_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$irq_userspace_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$clock_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$gpio_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$itc_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$framebuffer_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$blank_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$seeded_clock_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$random_expected" "$work/boot.log" >/dev/null || \
   ! grep -E -- "$lcd_power_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$tux_expected" "$work/boot.log" >/dev/null || \
   ! grep -E "$input_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$uinput_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$keyboard_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$button_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$contrast_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$sensors_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$watchdog_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$hsdma_expected" "$work/boot.log" >/dev/null || \
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
   ! grep -E "$shared_text_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$shell_ready" "$work/boot.log" >/dev/null || \
   ! grep -F "$shell_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$userspace_console_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$sd_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$sd_probe_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$sd_power_expected" "$work/boot.log" >/dev/null || \
   ! grep -F -- "$sd_clock_expected" "$work/boot.log" >/dev/null || \
   ! grep -E -- "$sd_width_expected" "$work/boot.log" >/dev/null || \
   ! grep -E -- "$sd_dma_channels_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$process_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$child_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$signal_expected" "$work/boot.log" >/dev/null || \
   ! grep -F "$trace_expected" "$work/boot.log" >/dev/null || \
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

if ! python3 "$root/linux/check-sd.py" "$fat_helper" \
	"$work/card.img"; then
	cat "$work/boot.log" >&2
	echo "Native Linux did not persist its FAT status file." >&2
	exit 1
fi
python3 "$root/linux/check-lcd.py" "$work/screen.pgm" --symbols --edited
cp "$work/screen.pgm" "$root/linux/artifacts/lcd-console.pgm"

grep -E "C33 Linux: entry|C33 boot:|Linux version|Memory:|Calibrating delay loop|s1c33-sd|s1c33-fb|mmcblk0|spi width:|dma channels:|C33 IRQ:|s1c33-itc|c33-timer|s1c33-uart[01]|serdev|wikireader-touch|wikireader-lcd|C33 sensors|s1c33-wdt|s1c33-pinctrl|s1c33-hsdma|C33 input:|C33 framebuffer:|C33 PTY:|C33 userspace|Run /init|C33: entered userspace|C33 process test|C33 signal test|C33 trace test|C33 uClibc smoke|C33 libc test|C33 diagnostic|C33 BusyBox|C33 MMC/SPI|C33 time|C33 random|C33 root|HARDWARE PASS|INTERACTIVE HUSH|touch[- ]keyboard pass" \
	"$work/boot.log"
echo "Full-chain native C33 Linux boot passed."
