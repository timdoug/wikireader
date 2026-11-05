#!/bin/bash
# Keep the existing DT_* interface; all target runs boot through Grifo.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$HERE/run.py" "$@"
