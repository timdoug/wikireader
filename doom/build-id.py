#!/usr/bin/env python3
"""Stable source fingerprint, associated with binary hashes in the run manifest."""
import hashlib
from pathlib import Path
import sys

root = Path(__file__).resolve().parent
paths = sorted([*root.glob('*.c'), *root.glob('*.h'), root / 'vendor/PureDOOM.h',
                root / 'Makefile', root / 'memory.lds', Path(__file__).resolve()])
digest = hashlib.sha256()
for path in paths:
    digest.update(str(path.relative_to(root)).encode() + b'\0' + path.read_bytes())
Path(sys.argv[1]).write_text('#define WR_BUILD_ID "' + digest.hexdigest()[:16] + '"\n')
