#!/bin/sh
set -eu

if [ "$#" -ne 6 ]; then
	echo "usage: $0 CROSS_PREFIX BUILD_DIR OUTPUT.list UCLIBC_SMOKE BUSYBOX CONSOLE" >&2
	exit 2
fi

cross=$1
build=$2
manifest=$3
uclibc_smoke=$4
busybox=$5
console=$6
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

mkdir -p "$build"
"${cross}as" -mc33pe "$here/init.S" -o "$build/diag-start.o"
"${cross}gcc" -mc33pe -Os -ffreestanding -fno-builtin \
	-medda32 \
	-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -c "$here/init.c" -o "$build/diag-init-c.o"
"${cross}objcopy" --remove-section=.debug_frame "$build/diag-init-c.o" \
	"$build/diag-init-code.o"
"${cross}ld" --gc-sections --emit-relocs -T "$here/init.ld" \
	-o "$build/diag-init.elf" "$build/diag-start.o" "$build/diag-init-code.o"
if ! "${cross}readelf" -rW "$build/diag-init.elf" | grep -q 'R_C33_H'; then
	echo "linked diagnostic init does not exercise C33 absolute relocations" >&2
	exit 1
fi
python3 "$here/make-flat.py" "$build/diag-init.elf" "$build/diag-init"
chmod 755 "$build/diag-init"

"${cross}gcc" -mc33pe -Os -ffreestanding -fno-builtin -DDIAG_ONESHOT \
	-medda32 \
	-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -c "$here/init.c" -o "$build/diag-test-c.o"
"${cross}objcopy" --remove-section=.debug_frame "$build/diag-test-c.o" \
	"$build/diag-test-code.o"
"${cross}ld" --gc-sections --emit-relocs -T "$here/init.ld" \
	-o "$build/diag-test.elf" "$build/diag-start.o" "$build/diag-test-code.o"
python3 "$here/make-flat.py" "$build/diag-test.elf" "$build/diag-test"
chmod 755 "$build/diag-test"

"${cross}as" -mc33pe "$here/child.S" -o "$build/child-start.o"
"${cross}gcc" -mc33pe -Os -ffreestanding -fno-builtin \
	-medda32 \
	-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -c "$here/child.c" -o "$build/child-c.o"
"${cross}objcopy" --remove-section=.debug_frame "$build/child-c.o" \
	"$build/child-c-code.o"
"${cross}ld" --gc-sections --emit-relocs -T "$here/init.ld" \
	-o "$build/child.elf" "$build/child-start.o" "$build/child-c-code.o"
python3 "$here/make-flat.py" "$build/child.elf" "$build/child"
chmod 755 "$build/child"

printf 'dir /bin 0755 0 0\ndir /sbin 0755 0 0\ndir /etc 0755 0 0\ndir /etc/init.d 0755 0 0\ndir /dev 0755 0 0\ndir /dev/pts 0755 0 0\ndir /proc 0555 0 0\ndir /sys 0555 0 0\ndir /tmp 01777 0 0\ndir /mnt 0755 0 0\ndir /mnt/sd 0755 0 0\nnod /dev/console 0600 0 0 c 5 1\nfile /bin/busybox %s 0755 0 0\nslink /init bin/busybox 0755 0 0\nslink /bin/sh busybox 0755 0 0\nslink /bin/mount busybox 0755 0 0\nslink /bin/uname busybox 0755 0 0\nslink /bin/echo busybox 0755 0 0\nslink /bin/cat busybox 0755 0 0\nslink /bin/ls busybox 0755 0 0\nslink /bin/pwd busybox 0755 0 0\nslink /bin/ps busybox 0755 0 0\nslink /bin/dmesg busybox 0755 0 0\nslink /sbin/init ../bin/busybox 0755 0 0\nslink /sbin/halt ../bin/busybox 0755 0 0\nslink /sbin/poweroff ../bin/busybox 0755 0 0\nslink /sbin/reboot ../bin/busybox 0755 0 0\nfile /sbin/wr-console %s 0755 0 0\nfile /etc/inittab %s/inittab 0644 0 0\nfile /etc/init.d/rcS %s/rcS 0755 0 0\nfile /diag-init %s/diag-init 0755 0 0\nfile /diag-test %s/diag-test 0755 0 0\nfile /child %s/child 0755 0 0\nfile /uclibc-smoke %s 0755 0 0\n' \
	"$busybox" "$console" "$here" "$here" "$build" "$build" "$build" \
	"$uclibc_smoke" >"$manifest"

# Keep every enabled recovery command available through a conventional path.
# Hush can dispatch applets internally, but explicit links also work from
# chroot scripts and from programs which call execve directly.
for applet in \
	awk basename blkid chmod chown chroot clear cmp cp cpio cut date dd df \
	diff dirname du env expr false fdisk find grep gunzip gzip head \
	hexdump hush id kill ln md5sum mkdir mkfifo mknod more mountpoint \
	mv printf realpath reset rm rmdir sed seq sha256sum sleep sort \
	stat strings sync tail tar tee test timeout touch tr true umount uniq \
	unzip vi wc whoami xargs
do
	printf 'slink /bin/%s busybox 0755 0 0\n' "$applet"
done >>"$manifest"

printf 'file /etc/init.d/busybox-test %s/busybox-test 0755 0 0\n' \
	"$here" >>"$manifest"
