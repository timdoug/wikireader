#!/usr/bin/env python3
"""Prepare the complete upstream BusyBox and uClibc-ng test inventories in Linux."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
UCLIBC_REVISION = "132c6134d69146bcafbdda68ae3a9cbd9f8fb921"
# C33 support in upstream's per-architecture form: a TLS macro header with
# its include line, and the math tests' ULP tolerances.
ARCH = ROOT / "linux/upstream-tests/arch"
ARCH_PATCH = ARCH / "tls-macros.patch"
ARCH_FILES = {"tls-macros-c33.h": "test/tls/tls-macros-c33.h",
              "libm-test-ulps-c33": "test/math/libm-test-ulps-c33"}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--guest", type=Path, default=Path("/home/timdoug.guest/wr-linux"))
    parser.add_argument("--output", type=Path, default=ROOT / "linux/artifacts/upstream-suites")
    parser.add_argument("--compiler", type=Path, help="Compiler for an isolated follow-up build")
    parser.add_argument("--work", default="upstream-suites", help="Guest work directory name")
    parser.add_argument("--package-only", action="store_true", help="Reuse the preceding libc test build")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if Path(args.work).name != args.work or args.work in {".", ".."}:
        raise SystemExit("Work must be a directory name under --guest")
    private = args.guest / args.work
    src = private / "uclibc-ng-test"
    private.mkdir(parents=True, exist_ok=True)
    if not src.exists():
        subprocess.run(["git", "clone", "https://git.uclibc-ng.org/git/uclibc-ng-test.git", src], check=True)
        subprocess.run(["git", "-C", src, "checkout", "--detach", UCLIBC_REVISION], check=True)
    compiler = (args.compiler or args.guest / "toolchain/install/bin/c33-linux-uclibc-gcc").resolve()
    if subprocess.check_output(["git", "-C", src, "rev-parse", "HEAD"], text=True).strip() != UCLIBC_REVISION:
        raise SystemExit("Unexpected uClibc test revision")
    # A previous build leaves the C33 include line applied.
    subprocess.run(["git", "-C", src, "checkout", "--", "test/tls/tls-macros.h"], check=True)
    if subprocess.check_output(["git", "-C", src, "status", "--porcelain", "--untracked-files=no"], text=True).strip():
        raise SystemExit("uClibc test source must be clean")
    subprocess.run(["git", "-C", src, "apply", ARCH_PATCH], check=True)
    for name, target in ARCH_FILES.items():
        if (ARCH / name).exists():
            shutil.copyfile(ARCH / name, src / target)
        else:
            (src / target).unlink(missing_ok=True)
    config = args.guest / "uclibc-build/.config"
    features = [line for line in config.read_text().splitlines()
                if line.endswith("=y") and line.startswith(("UCLIBC_", "MALLOC_", "HAVE_SHARED="))]
    variables = [f"CC={compiler}", "UCLIBC_EXTRA_CFLAGS=-O2 -std=gnu99 -fpermissive",
                 "UCLIBC_EXTRA_LDFLAGS=-Wl,-z,stack-size=262144", "V=1", *features]
    command = ["make", "-C", str(src), "-k", "-j4", *variables, "test_compile"]
    if args.package_only:
        previous = json.loads((out / "manifest.json").read_text())
        if previous["compiler_sha256"] != sha(compiler) or previous["libc_config_sha256"] != sha(config):
            raise SystemExit("Reused libc build compiler/configuration changed")
        for case in previous["uclibc"]:
            path = src / "test" / case["name"]
            if case["status"] == "BUILT" and sha(path) != case["binary_sha256"]:
                raise SystemExit("Reused libc test binary changed: " + case["name"])
        build_exit = previous["build_exit"]
    else:
        with (out / "uclibc-build.log").open("w") as log:
            # A failed rebuild must not leave a previous executable looking
            # like a successful compilation of this configuration.
            subprocess.run(["make", "-C", str(src), *variables, "clean"],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
            build_exit = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT).returncode
    with (out / "uclibc-gen.log").open("w") as log:
        generated = subprocess.run(["make", "-C", src, *variables, "test_gen"],
                                   stdout=log, stderr=subprocess.STDOUT)
    if generated.returncode:
        raise SystemExit("Upstream test inventory generation failed")
    entries = []
    for line in (src / "test/uclibcng-testrunner.in").read_text().splitlines():
        expected, source, binary, directory, shell = line.split(None, 4)
        path = src / "test" / directory / binary
        built = path.is_file() and path.read_bytes()[:4] == b"\x7fELF"
        entries.append({"name": f"{directory}/{binary}", "directory": f"uclibc/test/{directory}",
                        "command": shell, "upstream_row": line, "expected_exit": int(expected),
                        "source_sha256": sha(src / "test" / directory / f"{source}.c")
                        if (src / "test" / directory / f"{source}.c").exists() else None,
                        "status": "BUILT" if built else "BUILD_ERROR",
                        "binary_sha256": sha(path) if built else None})
    # Keep upstream-disabled tests distinct from build failures and runtime skips.
    disabled = []
    for directory in sorted((src / "test").glob("*/Makefile")):
        cmd = ["make", "-s", "--no-print-directory", "-C", str(directory.parent), *variables,
               "--eval=wr_inventory: ; @printf '%s\\n' '$(TESTS_DISABLED)'", "wr_inventory"]
        text = subprocess.check_output(cmd, text=True)
        disabled += [f"{directory.parent.name}/{name}" for name in text.split()]
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
    shutil.copytree(src / "test", stage / "uclibc/test",
                    ignore=shutil.ignore_patterns("*.o"))
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
        for name in ["uclibc", "busybox"]:
            archive.add(stage / name, arcname=name)
    if (subprocess.check_output(["git", "-C", src, "status", "--porcelain", "--untracked-files=no"], text=True).split()
            != ["M", "test/tls/tls-macros.h"] or
            subprocess.run(["git", "-C", src, "apply", "--reverse", "--check", ARCH_PATCH]).returncode):
        raise SystemExit("Build changed tracked upstream test source")
    tracked = subprocess.check_output(["git", "-C", src, "ls-files", "-z"]).decode().split("\0")
    manifest = {"uclibc_revision": UCLIBC_REVISION, "busybox_version": "1.38.0",
                "compiler": str(compiler), "compiler_sha256": sha(compiler),
                "build_command": command, "build_exit": build_exit, "libc_config_sha256": sha(config),
                "arch_support_sha256": {p.name: sha(p) for p in sorted(ARCH.iterdir()) if p.is_file()},
                "uclibc": entries, "uclibc_upstream_disabled": disabled, "busybox": bb_entries,
                "source_sha256": {p: sha(src / p) for p in tracked if p and (src / p).is_file()},
                "busybox_source_sha256": {str(p.relative_to(bb)): sha(p)
                                          for tree in [bb / "testsuite", bb / "shell/hush_test"]
                                          for p in sorted(tree.rglob("*")) if p.is_file()},
                "busybox_config_sha256": sha(bb / ".config"),
                "echo_helper_source_sha256": sha(bb / "scripts/echo.c"),
                "busybox_binary_sha256": sha(args.guest / "buildroot-build/target/bin/busybox"),
                "archive_sha256": sha(out / "suite.tar.gz"), "supervisor_sha256": sha(out / "super.bin")}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("uClibc inventory:", len(entries), "built:", sum(x["status"] == "BUILT" for x in entries), flush=True)
    print("BusyBox applet/shell groups:", len(bb_entries), flush=True)


if __name__ == "__main__":
    main()
