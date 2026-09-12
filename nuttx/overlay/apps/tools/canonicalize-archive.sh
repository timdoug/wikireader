#!/bin/sh
# Rewrite an archive with its members in a fixed order.
#
# SPDX-License-Identifier: Apache-2.0
#
# libapps.a is built up one subdirectory at a time, each contribution
# appended under a lock by whichever parallel job got there first -- so the
# order of its members is the order this machine happened to finish compiling
# them in, and it is different on the next build.  The linker lays sections
# out in the order it pulls members from the archive, so that ordering reaches
# the image: two builds of an unchanged tree differ, in nothing but padding,
# which makes "did this change the output?" an unanswerable question.
#
# Sorting the members by name answers it.  Any fixed order would do; sorted is
# the one that can be checked by eye.
#
# The reason this is not four lines of "ar x && ar r" is that member names in
# this archive are not unique: NuttX names an object after its source path
# with the separators turned into dots, and toybox has lib/env.c and
# toys/posix/env.c, which come out the same.  Extraction is by name, so the
# second would overwrite the first and the image would lose whichever of
# env_main and environ_bytes was unlucky.  A name that appears more than once
# is therefore extracted an instance at a time, into a directory each -- ar
# stores a member under its base name, so both keep the name they had.

set -eu

if [ $# -ne 2 ]; then
    echo "usage: $0 <ar> <archive>" >&2
    exit 2
fi

ar=$1
archive=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
work=$(dirname "$archive")/.canonicalize.$$
trap 'rm -rf "$work"' EXIT INT TERM

mkdir -p "$work/once"
"$ar" t "$archive" > "$work/order"
LC_ALL=C sort "$work/order" > "$work/sorted"

# Everything comes out in one pass; the duplicated names are then redone
# properly below, since this pass leaves only the last of each of them.
(cd "$work/once" && "$ar" x "$archive")

: > "$work/members"
previous=
instance=0

while IFS= read -r name; do
    if [ "$name" = "$previous" ]; then
        instance=$((instance + 1))
    else
        previous=$name
        instance=1
    fi

    if [ "$(grep -c -x -F -- "$name" "$work/order")" -eq 1 ]; then
        printf '%s\n' "$work/once/$name" >> "$work/members"
        continue
    fi

    mkdir -p "$work/dup.$instance"
    (cd "$work/dup.$instance" && "$ar" xN "$instance" "$archive" "$name")
    printf '%s\n' "$work/dup.$instance/$name" >> "$work/members"
done < "$work/sorted"

# D: no timestamps or ownership in the members either, so the archive itself
# is the same file every time rather than merely a source of the same image.
xargs "$ar" qcD "$work/sorted.a" < "$work/members"
"$ar" s "$work/sorted.a"

before=$(wc -l < "$work/order")
after=$("$ar" t "$work/sorted.a" | wc -l)
if [ "$before" -ne "$after" ]; then
    echo "$0: $archive had $before members, rewrote $after" >&2
    exit 1
fi

mv "$work/sorted.a" "$archive"
