#!/bin/sh
# Fetch a Linux kernel for the rv32ima interpreter and build device trees
# for it.  The image is cnlohr's prebuilt nommu M-mode build, the one
# mini-rv32ima is developed against: rv32ima, no MMU, no supervisor mode,
# with a busybox initramfs inside it.  That configuration is why a
# Linux-capable interpreter fits in this machine's internal RAM at all.
#
# The device tree is his too, with the memory node resized: it is where the
# kernel learns the RAM size, the console and the 1 MHz timer, and its
# addresses are the ones rv32.h implements.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
# Not under build/: `make clean` removes that, and re-downloading a
# 3.5 MB kernel because you rebuilt the application is unfriendly.
out="${here}/linux"
# Pinned to the commits that were master on 2026-11-01, and checked.
url=https://github.com/cnlohr/mini-rv32ima-images/raw/7da8c6e486b7d510426b5c2283cd338abcf04eb7/images/linux-6.1.14-rv32nommu-cnl-1.zip
dts=https://raw.githubusercontent.com/cnlohr/mini-rv32ima/84858f58cb41899705e2ff2d6ee3b2d5c0795bfe/mini-rv32ima/sixtyfourmb.dts
fetch="${here}/../host-tools/fetch-verified.sh"

mkdir -p "${out}"
"${fetch}" "${url}" "${out}/image.zip" \
	add651195348b538c309becb39c5f8ef4f9d15ec275a2954b02016fc38091393
[ -f "${out}/Image" ] || ( cd "${out}" && unzip -o image.zip >/dev/null )
"${fetch}" "${dts}" "${out}/sixtyfourmb.dts" \
	65f78b5a7ce5a6d240a2685de079cc91c8ace3fce1b9e78f0f9c7d1b0f12d342

command -v dtc >/dev/null || { echo "need dtc (brew install dtc)" >&2; exit 1; }

# GUEST_RAM in riscv.c, less the page the device tree itself sits in.
size=0xbfc000
sed "s/0x3ffc000/${size}/" "${out}/sixtyfourmb.dts" > "${out}/wr.dts"
sed "s|console=ttyS0|console=ttyS0 rdinit=/bin/sh|" "${out}/wr.dts" > "${out}/wr-sh.dts"
for name in wr wr-sh; do
	dtc -I dts -O dtb -o "${out}/${name}.dtb" "${out}/${name}.dts" 2>/dev/null
done

ls -l "${out}/Image" "${out}/wr.dtb" "${out}/wr-sh.dtb"
