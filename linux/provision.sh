#!/bin/sh
# Install the host-side dependencies used by Kbuild and the C33 toolchain.
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
	dwarves texinfo
