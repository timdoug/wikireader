#!/bin/sh
# Finish a full BusyBox run (run.py busybox --output DIR): the Hush tests a
# module left when it ran past its time limit are run one each into
# DIR-hush, and summarize.py writes DIR-summary.json from both.  Fails when
# a case is still unaccounted for.
#
#   linux/upstream-tests/hush-followup.sh DIR
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
dir=${1:?usage: hush-followup.sh DIR}
dir=${dir%/}
python3 "$here/summarize.py" --busybox "$dir" --output "$dir-summary.json" >/dev/null
cases=$(python3 -c '
import json, sys
s = json.load(open(sys.argv[1]))
for r in s["hush_individual"]["results"]:
    if r["status"] == "MISSING":
        print("--hush-case", r["name"][len("hush/"):])
' "$dir-summary.json")
hush=
if [ -n "$cases" ]; then
	# run.py exits 1 on any failing test; the summary judges the whole.
	# shellcheck disable=SC2086
	python3 "$here/run.py" busybox $cases --output "$dir-hush" || true
	hush="--hush $dir-hush"
fi
# shellcheck disable=SC2086
python3 "$here/summarize.py" --busybox "$dir" $hush --output "$dir-summary.json"
python3 -c '
import json, sys
sys.exit(0 if json.load(open(sys.argv[1]))["accounted"] else 1)
' "$dir-summary.json"
