#!/bin/bash
# Aggregate completed, verified Grifo runs without executing old bare ELFs.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$HERE/run.py" --coverage "$@"
