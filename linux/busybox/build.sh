#!/bin/sh
set -eu

if [ "$(uname -s)" != Linux ]; then
	echo "busybox/build.sh must run inside the wr-linux VM" >&2
	exit 1
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
guest_root=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
source_dir=${WR_BUSYBOX_SOURCE:-$guest_root/busybox}
build_dir=${WR_BUSYBOX_BUILD:-$guest_root/busybox-build}
tool_dir=${C33_TOOLCHAIN_WORK:-$guest_root/toolchain}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
cross=$tool_dir/install/bin/c33-epson-elf-
cc=$root/linux/busybox/c33-uclibc-gcc.sh

if [ ! -d "$source_dir/.git" ]; then
	echo "BusyBox source not found at $source_dir" >&2
	echo "Run make -C linux fetch first." >&2
	exit 1
fi
if [ ! -x "${cross}gcc" ]; then
	echo "C33 compiler not found at ${cross}gcc" >&2
	echo "Run make -C linux toolchain first." >&2
	exit 1
fi

rm -rf "$build_dir"
mkdir -p "$build_dir" "$root/linux/artifacts"
make -C "$source_dir" O="$build_dir" allnoconfig >/dev/null

# BusyBox's allnoconfig treats KCONFIG_ALLCONFIG values as defaults and then
# answers "no", so it does not actually enable a small config fragment.  Start
# with its complete all-no config, apply our explicit yes values, then let
# oldconfig resolve dependencies and newly exposed options to their defaults.
for choice in \
	INSTALL_APPLET_SYMLINKS INSTALL_APPLET_HARDLINKS \
	INSTALL_APPLET_SCRIPT_WRAPPERS SH_IS_ASH SH_IS_NONE BASH_IS_ASH BASH_IS_HUSH
do
	sed -i \
		-e "s/^CONFIG_${choice}=y\$/# CONFIG_${choice} is not set/" \
		"$build_dir/.config"
done
while IFS= read -r setting; do
	case "$setting" in
		CONFIG_*=y)
			name=${setting%%=*}
			sed -i \
				-e "s/^# ${name} is not set\$/${setting}/" \
				-e "s/^${name}=.*/${setting}/" \
				"$build_dir/.config"
			;;
	esac
done < "$root/linux/busybox/minimal.config"
yes '' | make -C "$source_dir" O="$build_dir" oldconfig >/dev/null
make -C "$source_dir" O="$build_dir" CROSS_COMPILE="$cross" CC="$cc" \
	-j"$jobs"

test -x "$build_dir/busybox_unstripped"
undefined=$(${cross}nm -u "$build_dir/busybox_unstripped")
if [ -n "$undefined" ]; then
	echo "static BusyBox has undefined symbols:" >&2
	echo "$undefined" >&2
	exit 1
fi
python3 "$root/linux/initramfs/make-flat.py" \
	"$build_dir/busybox_unstripped" "$build_dir/busybox"
chmod 755 "$build_dir/busybox"
cp "$build_dir/busybox" "$root/linux/artifacts/busybox"

${cross}size "$build_dir/busybox_unstripped"
printf '%s\n' "C33 BusyBox installed at $root/linux/artifacts/busybox"
