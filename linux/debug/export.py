#!/usr/bin/env python3
"""Buildroot finalize hook: save host ELFs, remove on-card DWARF only."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    target, output = map(Path, sys.argv[1:3])
    strip = sys.argv[3]
    digest = hashlib.sha256()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="wr-symbols-", dir=output.parent) as temporary:
        snapshot = Path(temporary) / "symbols"
        snapshot.mkdir()
        stripped = Path(temporary) / "stripped"
        export(target, output, snapshot, stripped, strip, digest)
        build = digest.hexdigest() + "\n"
        (snapshot / "build-id").write_text(build)
        (target / "etc/wr-build-id").write_text(build)
        if output.exists():
            shutil.rmtree(output)
        shutil.move(snapshot, output)
    print(f"Saved host symbols: {output}")


def export(target, previous, output, stripped, strip, digest):
    for path in sorted(target.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        relative = path.relative_to(target)
        data = path.read_bytes()
        saved = output / relative
        saved.parent.mkdir(parents=True, exist_ok=True)
        old = previous / relative
        # An incremental finalization sees already stripped files. Reuse
        # the old DWARF only if stripping it reproduces the target exactly.
        # Changed executables always replace their previous symbols.
        source = path
        if old.is_file() and not old.is_symlink():
            subprocess.run([strip, "--strip-debug", "-o", str(stripped), str(old)], check=True)
            if stripped.read_bytes() == data:
                source = old
        shutil.copy2(source, saved)
        digest.update(str(relative).encode() + b"\0" + saved.read_bytes())
        subprocess.run([strip, "--strip-debug", str(path)], check=True)
    # Preserve target symlinks too (ld.so/libc aliases); all are relative.
    for path in sorted(target.rglob("*")):
        if path.is_symlink() and path.is_file():
            relative = path.relative_to(target)
            if path.resolve().read_bytes()[:4] == b"\x7fELF":
                saved = output / relative
                saved.parent.mkdir(parents=True, exist_ok=True)
                saved.symlink_to(path.readlink())


if __name__ == "__main__":
    main()
