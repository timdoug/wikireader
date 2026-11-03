#!/usr/bin/env python3
"""Prepare the complete upstream BusyBox test inventory in Linux."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--guest", type=Path, default=Path("/home/timdoug.guest/wr-linux"))
    parser.add_argument("--output", type=Path, default=ROOT / "linux/artifacts/upstream-suites")
    parser.add_argument("--compiler", type=Path, help="Compiler for an isolated follow-up build")
    parser.add_argument("--work", default="upstream-suites", help="Guest work directory name")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if Path(args.work).name != args.work or args.work in {".", ".."}:
        raise SystemExit("Work must be a directory name under --guest")
    private = args.guest / args.work
    private.mkdir(parents=True, exist_ok=True)
    compiler = (args.compiler or args.guest / "toolchain/install/bin/c33-linux-musl-gcc").resolve()
    bb = args.guest / "buildroot-build/build/busybox-1.38.0"
    if "# CONFIG_ASH is not set" not in (bb / ".config").read_text():
        raise SystemExit("This board runner expects Hush; an Ash-enabled build needs its shell suite too")
    ash = bb / "shell/ash_test"
    excluded = {"reason": "CONFIG_ASH is not set; installed shell is Hush",
                "cases": len(list(ash.rglob("*.tests"))),
                "tests": [str(p.relative_to(bb)) for p in sorted(ash.rglob("*.tests"))],
                "source_sha256": {str(p.relative_to(bb)): sha(p) for p in sorted(ash.rglob("*")) if p.is_file()}}
    (out / "busybox-disabled-suites.json").write_text(json.dumps(excluded, indent=2) + "\n")
    stage = private / "package"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(exist_ok=False)
    shutil.copytree(bb / "testsuite", stage / "busybox/testsuite")
    shutil.copytree(bb / "shell/hush_test", stage / "busybox/shell/hush_test")
    shutil.copyfile(bb / ".config", stage / "busybox/.config")
    # Test the installed BusyBox, not a newly configured all-features binary.
    (stage / "busybox/busybox").symlink_to("/bin/busybox")
    (stage / "busybox/shell/hush_test/hush").symlink_to("/bin/busybox")
    tests = stage / "busybox/testsuite"
    # testing.sh normally compiles this helper when echo lacks -ne. There is
    # no native compiler in the guest, so cross-build the unchanged helper.
    subprocess.run([compiler, "-Os", bb / "scripts/echo.c", "-o", tests / "echo-ne"], check=True)
    bb_entries = []
    applets = sorted({p.name for p in tests.iterdir() if p.is_dir()} |
                     {p.stem for p in tests.glob("*.tests")})
    for applet in applets:
        bb_entries.append({"name": f"applets/{applet}", "directory": "busybox/testsuite",
                           "command": f"sh ./runtest -v {applet}", "status": "BUILT",
                           "host_only": applet == "all_sourcecode"})
    for module in sorted((stage / "busybox/shell/hush_test").glob("hush-*")):
        if module.is_dir():
            bb_entries.append({"name": f"hush/{module.name}", "directory": "busybox/shell/hush_test",
                               "command": f"sh ./run-all {module.name}", "status": "BUILT"})
    subprocess.run([compiler, "-O2", "-Wall", "-Werror", ROOT / "linux/upstream-tests/supervise.c",
                    "-o", out / "super.bin"], check=True)
    with tarfile.open(out / "suite.tar.gz", "w:gz", format=tarfile.USTAR_FORMAT) as archive:
        archive.add(stage / "busybox", arcname="busybox")
    manifest = {"busybox_version": "1.38.0",
                "compiler": str(compiler), "compiler_sha256": sha(compiler),
                "busybox": bb_entries,
                "busybox_source_sha256": {str(p.relative_to(bb)): sha(p)
                                          for tree in [bb / "testsuite", bb / "shell/hush_test"]
                                          for p in sorted(tree.rglob("*")) if p.is_file()},
                "busybox_config_sha256": sha(bb / ".config"),
                "echo_helper_source_sha256": sha(bb / "scripts/echo.c"),
                "busybox_binary_sha256": sha(args.guest / "buildroot-build/target/bin/busybox"),
                "archive_sha256": sha(out / "suite.tar.gz"), "supervisor_sha256": sha(out / "super.bin")}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("BusyBox applet/shell groups:", len(bb_entries), flush=True)


if __name__ == "__main__":
    main()
