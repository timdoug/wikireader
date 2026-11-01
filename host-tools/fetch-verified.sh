#!/bin/sh
# Download a file and check it against a known SHA-256 before using it.
#
#   host-tools/fetch-verified.sh URL FILE SHA256
#
# A FILE that is already there is checked too, so a truncated or replaced
# download is never built from.  The download goes to FILE.tmp and is renamed
# only once it matches.  To move to a new upstream version, update the URL and
# the hash together; the hash of a fresh download is printed on a mismatch.
set -eu

url=$1 file=$2 want=$3

sha256() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	else
		shasum -a 256 "$1" | cut -d' ' -f1
	fi
}

if [ -f "$file" ]; then
	have=$(sha256 "$file")
	[ "$have" = "$want" ] && exit 0
	echo "$file: SHA-256 $have, expected $want; downloading it again" >&2
	rm -f "$file"
fi
echo "==> downloading $(basename "$file")"
curl -fsSL -o "$file.tmp" "$url"
have=$(sha256 "$file.tmp")
if [ "$have" != "$want" ]; then
	echo "$url: SHA-256 $have, expected $want" >&2
	rm -f "$file.tmp"
	exit 1
fi
mv "$file.tmp" "$file"
