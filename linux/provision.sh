#!/bin/sh
# Install the host-side dependencies used by Kbuild, the C33 toolchain and
# Buildroot.  Buildroot uses the host's CMake when it is recent enough;
# otherwise it builds its own, for Ninja, which Meson packages (pixman,
# xkeyboard-config) need: 4.5 of the build's 10 minutes.
set -eu

if [ "$(uname -s)" != Linux ]; then
	echo "provision.sh must run inside the wr-linux VM" >&2
	exit 1
fi

export DEBIAN_FRONTEND=noninteractive
sudo -E apt-get update
sudo -E apt-get install -y \
	build-essential bc bison flex git curl rsync patch gawk dejagnu \
	python3 python3-venv cpio xz-utils unzip file wget perl \
	libssl-dev libelf-dev libncurses-dev zlib1g-dev \
	libgmp-dev libmpfr-dev libmpc-dev device-tree-compiler \
	dwarves texinfo cmake

# The VM keeps the Mac's time, which Lima's guest agent sets every ten
# seconds.  timesyncd set it to NTP's in between, 0.8 s apart, and each
# step back could make a later build stamp older than an earlier one, so
# that make redid finished packages.
sudo systemctl disable --now systemd-timesyncd
