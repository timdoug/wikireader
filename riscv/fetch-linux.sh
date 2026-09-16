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
url=https://github.com/cnlohr/mini-rv32ima-images/raw/master/images/linux-6.1.14-rv32nommu-cnl-1.zip
dts=https://raw.githubusercontent.com/cnlohr/mini-rv32ima/master/mini-rv32ima/sixtyfourmb.dts

mkdir -p "${out}"
if [ ! -f "${out}/Image" ]; then
	curl -sSL --fail -o "${out}/image.zip" "${url}"
	( cd "${out}" && unzip -o image.zip >/dev/null )
fi
[ -f "${out}/sixtyfourmb.dts" ] || curl -sSL --fail -o "${out}/sixtyfourmb.dts" "${dts}"

command -v dtc >/dev/null || { echo "need dtc (brew install dtc)" >&2; exit 1; }

# GUEST_RAM in riscv.c, less the page the device tree itself sits in.
size=0xbfc000
sed "s/0x3ffc000/${size}/" "${out}/sixtyfourmb.dts" > "${out}/wr.dts"
sed "s|console=ttyS0|console=ttyS0 rdinit=/bin/sh|" "${out}/wr.dts" > "${out}/wr-sh.dts"
for name in wr wr-sh; do
	dtc -I dts -O dtb -o "${out}/${name}.dtb" "${out}/${name}.dts" 2>/dev/null
done

ls -l "${out}/Image" "${out}/wr.dtb" "${out}/wr-sh.dtb"
