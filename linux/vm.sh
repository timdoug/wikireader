#!/bin/sh
# Start the small ARM64 Debian build appliance used by the C33 Linux port.
set -eu

name=${WR_LINUX_VM:-wr-linux}
root=$(cd "$(dirname "$0")/.." && pwd)

if limactl list -q "$name" | grep -qx "$name"; then
	exec limactl start "$name"
fi

exec limactl start --name="$name" --vm-type=vz --arch=aarch64 \
	--cpus=6 --memory=8 --disk=40 --containerd=none \
	--mount-type=virtiofs --mount-only="${root}:w" -y template:debian
