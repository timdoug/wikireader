#!/usr/bin/env python3
"""Prepare the complete upstream BusyBox and musl libc-test inventories in Linux."""
import argparse
import hashlib
import json
from pathlib import Path
import os
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
LIBC_TEST = "https://repo.or.cz/libc-test.git"
LIBC_TEST_REVISION = "63e36e934e8f2bedf645303a90b64098e2af6c8f"
# libc-test's own settings, for the musl cross compiler.  A test's stack is
# taken whole without an MMU: 256 KB, which the math drivers need.
LIBC_TEST_CONFIG = """CFLAGS += -pipe -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wno-unused-function -Wno-missing-braces -Wno-unused -Wno-overflow
CFLAGS += -Wno-unknown-pragmas -fno-builtin -frounding-math
CFLAGS += -Werror=implicit-function-declaration -Werror=implicit-int -Werror=pointer-sign -Werror=pointer-arith
LDFLAGS += -Wl,-z,stack-size=262144
LDLIBS += -lpthread -lm -lrt
"""

# The VM's work tree, as the shell scripts find it.
GUEST_ROOT = Path(os.environ.get(
    "WR_LINUX_GUEST_ROOT", f"/home/{os.environ.get('USER', '')}.guest/wr-linux"))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def libc_test(private, compiler, out):
    """Build every libc-test binary in its tree, as its Makefile names them,
    without running any: its runner forks.  Returns the inventory."""
    src = private / "libc-test"
    if not src.exists():
        subprocess.run(["git", "clone", LIBC_TEST, src], check=True)
    subprocess.run(["git", "-C", src, "checkout", "-q", "--detach", LIBC_TEST_REVISION], check=True)
    if subprocess.check_output(["git", "-C", src, "status", "--porcelain", "--untracked-files=no"], text=True).strip():
        raise SystemExit("libc-test source must be clean")
    subprocess.run(["git", "-C", src, "clean", "-qfdx"], check=True)
    (src / "config.mak").write_text(f"CC = {compiler}\n" + LIBC_TEST_CONFIG)
    cross = str(compiler)[:-len("gcc")]
    make = ["make", "-C", str(src), f"CROSS_COMPILE={cross}"]
    listing = subprocess.check_output([*make, "-s", "debug"], text=True)
    names = {key: value.split() for key, _, value in
             (line.partition(" ") for line in listing.splitlines())}
    targets = names["BINS"] + [x for x in names["LIBS"] if x.endswith(".so")]
    with (out / "libc-test-build.log").open("w") as log:
        build_exit = subprocess.run([*make, "-k", "-j4", *targets],
                                    stdout=log, stderr=subprocess.STDOUT).returncode
    entries = []
    for binary in names["BINS"]:
        path = src / binary
        built = path.is_file() and path.read_bytes()[:4] == b"\x7fELF"
        name = binary.removeprefix("src/").removesuffix(".exe")
        source = src / "src" / (name.removesuffix("-static") + ".c")
        entries.append({"name": name, "directory": "libc-test", "command": "./" + binary,
                        "status": "BUILT" if built else "BUILD_ERROR",
                        "source_sha256": sha(source) if source.exists() else None,
                        "binary_sha256": sha(path) if built else None})
    files = [p for p in sorted((src / "src").rglob("*")) if p.suffix in (".exe", ".so") and p.is_file()]
    return entries, files, src, build_exit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--guest", type=Path, default=GUEST_ROOT)
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
    libc_entries, libc_files, libc_src, libc_build_exit = libc_test(private, compiler, out)
    for path in libc_files:
        target = stage / "libc-test" / path.relative_to(libc_src)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        target.chmod(0o755)
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
        archive.add(stage / "libc-test", arcname="libc-test")
    manifest = {"busybox_version": "1.38.0", "libc_test_revision": LIBC_TEST_REVISION,
                "compiler": str(compiler), "compiler_sha256": sha(compiler),
                "busybox": bb_entries, "libc": libc_entries, "libc_build_exit": libc_build_exit,
                "busybox_source_sha256": {str(p.relative_to(bb)): sha(p)
                                          for tree in [bb / "testsuite", bb / "shell/hush_test"]
                                          for p in sorted(tree.rglob("*")) if p.is_file()},
                "busybox_config_sha256": sha(bb / ".config"),
                "echo_helper_source_sha256": sha(bb / "scripts/echo.c"),
                "busybox_binary_sha256": sha(args.guest / "buildroot-build/target/bin/busybox"),
                "archive_sha256": sha(out / "suite.tar.gz"), "supervisor_sha256": sha(out / "super.bin")}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("BusyBox applet/shell groups:", len(bb_entries), flush=True)
    print("libc-test binaries:", len(libc_entries), "built:",
          sum(x["status"] == "BUILT" for x in libc_entries), flush=True)


if __name__ == "__main__":
    main()
