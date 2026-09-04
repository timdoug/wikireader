#!/bin/sh
set -eu

archive=${ZIM_TEST_ARCHIVE:-../../wikipedia_en-simple_all_nopic_2026-06.zim}

if [ ! -f "$archive" ]; then
	echo "SKIP: set ZIM_TEST_ARCHIVE to a ZIM 6 archive"
	exit 0
fi

info=$(./zimdump "$archive" info)
printf '%s\n' "$info" | grep '^ZIM 6\.' >/dev/null
printf '%s\n' "$info" | grep '^entries:' >/dev/null
printf '%s\n' "$info" | grep '^title listing:' >/dev/null

./zimdump "$archive" path X listing/titleOrdered/v1 |
	grep 'X/listing/titleOrdered/v1' >/dev/null
./zimdump "$archive" title Wikipedia 3 |
	grep 'Wikipedia' >/dev/null
./zimdump "$archive" blob C Wikipedia |
	grep -i '<html' >/dev/null
./zimdump "$archive" text C Wikipedia |
	grep 'free content online encyclopedia' >/dev/null
./zimdump "$archive" text C USA |
	grep '^United States$' >/dev/null
./zimdump "$archive" text C Cat |
	grep '^Cat (Felis catus)$' >/dev/null

./blob-cache-test "$archive" 40

echo "PASS: ZIM indexes, redirects, Zstandard extraction, and HTML conversion"
