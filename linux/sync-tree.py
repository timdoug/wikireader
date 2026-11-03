#!/usr/bin/env python3
"""Make an upstream work tree exactly its pinned commit plus this repository's
patches and overlay, and nothing else:

    sync-tree.py SRC [--overlay DIR] [--overlay-first] [--keep PATTERN]
                 [PATCH...]

SRC is a git checkout of the pinned revision (fetch.sh makes it).  The
tree wanted -- HEAD, the overlay's files and the patches applied in the
order given, the overlay first with --overlay-first -- is built in git's
object store without touching the work tree; then SRC's index becomes that
tree, the files whose contents differ are checked out, and every file the
tree does not have is removed, except paths matching --keep.  Files whose
contents are unchanged keep their timestamps, so an incremental build stays
incremental, and the repository stays the only source of truth: nothing
edited in SRC survives a build.
"""
import argparse
import os
import subprocess
import sys


def git(src, *args, env=None, data=None):
    return subprocess.run(["git", "-C", src, *args], env=env, input=data,
                          check=True, capture_output=True).stdout


def add_overlay(src, overlay, env):
    entries = []
    for root, dirs, files in os.walk(overlay):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(root, name)
            rel = os.path.relpath(path, overlay)
            blob = git(src, "hash-object", "-w", "--", os.path.abspath(path)).decode().strip()
            mode = "100755" if os.access(path, os.X_OK) else "100644"
            entries.append(f"{mode} {blob}\t{rel}\n")
    git(src, "update-index", "--add", "--index-info", env=env,
        data="".join(entries).encode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--overlay")
    ap.add_argument("--overlay-first", action="store_true")
    ap.add_argument("--keep", action="append", default=[])
    ap.add_argument("patches", nargs="*")
    args = ap.parse_args()
    src = os.path.abspath(args.src)

    scratch = os.path.join(src, ".git", "sync-tree-index")
    env = dict(os.environ, GIT_INDEX_FILE=scratch)
    try:
        git(src, "read-tree", "HEAD", env=env)
        if args.overlay and args.overlay_first:
            add_overlay(src, args.overlay, env)
        for patch in args.patches:
            try:
                git(src, "apply", "--cached", os.path.abspath(patch), env=env)
            except subprocess.CalledProcessError as e:
                sys.exit(f"{patch} does not apply:\n{e.stderr.decode()}")
        if args.overlay and not args.overlay_first:
            add_overlay(src, args.overlay, env)
        tree = git(src, "write-tree", env=env).decode().strip()
    finally:
        if os.path.exists(scratch):
            os.remove(scratch)

    # One-tree merge, whatever the work tree holds: entries whose contents
    # match keep their stat data.
    git(src, "read-tree", "--reset", tree)
    subprocess.run(["git", "-C", src, "update-index", "-q", "--refresh"],
                   capture_output=True)
    changed = git(src, "diff-files", "--name-only", "-z")
    if changed:
        git(src, "checkout-index", "-f", "-z", "--stdin", data=changed)
    keep = [a for k in args.keep for a in ("-e", k)]
    removed = git(src, "clean", "-fdx", *keep).decode().splitlines()
    n = changed.count(b"\0")
    print(f"{src}: {n} files written, {len(removed)} removed")


main()
