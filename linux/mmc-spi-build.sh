#!/bin/sh
# A comparison kernel: the card on the SPI controller and mmc_spi, from the
# kernel linux/build.sh last configured, into its own build directory and
# linux/artifacts/mmc-spi/.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${WR_LINUX_GUEST_ROOT:-/home/$USER.guest/wr-linux}
src=$work/linux-src
main=$work/linux-build
build=$work/linux-build-mmcspi
cross=$work/toolchain/install/bin/c33-epson-elf-
out=$root/linux/artifacts/mmc-spi
"$root/linux/sync-trees.sh" linux >/dev/null
mkdir -p "$build" "$out"
cp "$main/.config" "$build/.config"
"$src/scripts/config" --file "$build/.config" --enable SPI --enable SPI_MASTER \
	--enable SPI_S1C33 --enable MMC_SPI --disable MMC_S1C33_SD \
	--set-str BUILTIN_DTB_NAME wikireader-mmc-spi
make -s -C "$src" O="$build" ARCH=c33 CROSS_COMPILE="$cross" olddefconfig
grep -E "^CONFIG_(SPI_S1C33|MMC_SPI|MMC_S1C33_SD|BUILTIN_DTB_NAME)=" "$build/.config"
nice -n 19 make -s -C "$src" O="$build" ARCH=c33 CROSS_COMPILE="$cross" -j"${JOBS:-4}" vmlinux
a0=$("${cross}objdump" -h "$build/vmlinux" | awk '
	$2 == ".a0.text" || $2 == ".a0.data" {
		printf " --change-section-vma %s=0x%s", $2, $5 }')
"${cross}objcopy" --strip-all $a0 "$build/vmlinux" "$out/linux.app"
echo "mmc_spi kernel at $out/linux.app"
