#!/bin/sh
# Validate the device-tree bindings in overlay/ against their meta-schemas
# and examples, and the board's device tree against every binding
# (dt_binding_check, dtbs_check), with the dtschema provision.sh installs.
set -eu
work=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
export PATH=$work/dtschema-venv/bin:$PATH
kmake() {
	make -C "$work/linux-src" O="$work/linux-build" ARCH=c33 \
		CROSS_COMPILE="$work/toolchain/install/bin/c33-epson-elf-" "$@"
}
log=$(mktemp)
kmake dt_binding_check DT_SCHEMA_FILES="epson:openmoko:c33/" >"$log" 2>&1
kmake dtbs_check >>"$log" 2>&1
if grep -E "\.yaml:|\.dtb:|error|warning" "$log"; then
	echo "device tree check FAILED" >&2
	exit 1
fi
echo "Device tree passed: the C33 bindings and wikireader.dtb against every schema"
