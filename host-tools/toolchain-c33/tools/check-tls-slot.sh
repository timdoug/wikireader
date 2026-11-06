#!/bin/sh
# The TLS slot is part of the C33 Linux ABI, defined in the GCC port
# (c33-linux-tls.h) and in the kernel's <asm/tls.h>.  Where this repository
# holds the kernel too, check that they agree.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
port=$here/../gcc/files/gcc/config/c33/c33-linux-tls.h
uapi=$here/../../../linux/overlay/arch/c33/include/uapi/asm/tls.h
[ -f "$uapi" ] || exit 0
slot() {
	sed -n 's/^#define[[:space:]]*C33_TLS_SLOT_ADDRESS[[:space:]]*\([^[:space:]]*\).*/\1/p' "$1"
}
a=$(slot "$port") b=$(slot "$uapi")
if [ -z "$a" ] || [ "$((a))" != "$((b))" ]; then
	echo "C33_TLS_SLOT_ADDRESS differs: $port ($a) and $uapi ($b)" >&2
	exit 1
fi
