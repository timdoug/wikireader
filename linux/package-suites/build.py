#!/usr/bin/env python3
"""Build zlib's, expat's, fontconfig's and pixman's own test suites for the
C33, from Buildroot's build trees of the packages in the root image, as
ltp/run.py manifests in linux/artifacts/package-suites/NAME.  Runs in the
wr-linux VM after linux/world.sh; package-suites/run.py runs them.

Each package is copied out of Buildroot's tree and built there, so the
image's build stays as Buildroot left it.  The card's FAT fixture takes
8.3 names only, hence the short binary names.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
WORK = Path.home() / "wr-linux"
BUILDROOT = WORK / "buildroot-build"
TARGET = "c33-linux-musl"
TOOLS = WORK / "toolchain/install/bin"

# pixman's tests that are left out: each takes more than 20 guest minutes,
# most of it in software floating point.
PIXMAN_SLOW = {"pixel-test", "alphamap", "gradient-crash-test", "filter-reduction-test",
               "stress-test", "composite", "tolerance-test"}
# alpha-loop calls a composite through a loop of alpha maps an infinite
# loop if it takes 10 seconds, which this CPU needs more than; it finishes
# in well under two minutes.
ALPHA_LOOP_SECONDS = 120
# The fuzzers run the first iterations of their range, compared with the
# checksum a native build prints for the same range.  glyph-test's 5,000
# take more than 20 guest minutes.
PIXMAN_ITERATIONS = {"glyph-test": 1000}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(*command, cwd=None, env=None):
    subprocess.run([str(c) for c in command], cwd=cwd, env=env, check=True,
                   stdout=subprocess.DEVNULL)


def package_tree(name):
    trees = sorted((BUILDROOT / "build").glob(name + "-[0-9]*"))
    if not trees:
        raise SystemExit(f"no Buildroot build of {name}; run linux/world.sh first")
    return trees[-1]


def copy_tree(source, scratch, name):
    dest = scratch / name
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(source, dest, symlinks=True,
                    ignore=shutil.ignore_patterns("buildroot-build", ".stamp_*"))
    return dest


class Suite:
    def __init__(self, out, name, revision, supervisor):
        self.dir = out / name
        shutil.rmtree(self.dir, ignore_errors=True)
        self.dir.mkdir(parents=True)
        shutil.copy(supervisor, self.dir / "super.bin")
        self.manifest = {"revision": revision, "cases": [], "fixtures": [], "setup": [],
                         "supervisor": {"binary": "super.bin",
                                        "binary_sha256": digest(self.dir / "super.bin")}}

    def add_file(self, name, source=None, text=None, strip=False):
        dest = self.dir / name
        if text is not None:
            dest.write_text(text)
            dest.chmod(0o755)
        elif strip:
            run(TOOLS / f"{TARGET}-strip", "-o", dest, source)
        else:
            shutil.copy(source, dest)
        return name

    def fixture(self, name, **kw):
        self.add_file(name, **kw)
        self.manifest["fixtures"].append({"binary": name, "binary_sha256": digest(self.dir / name)})

    def case(self, name, binary, **extra):
        self.manifest["cases"].append({"name": name, "binary": binary,
                                       "binary_sha256": digest(self.dir / binary), **extra})

    def write(self):
        if not self.manifest["fixtures"]:
            del self.manifest["fixtures"]
        if not self.manifest["setup"]:
            del self.manifest["setup"]
        (self.dir / "manifest.json").write_text(json.dumps(self.manifest, indent=1) + "\n")
        print(f"{self.dir.name}: {len(self.manifest['cases'])} cases")


def build_zlib(out, scratch, supervisor, env):
    tree = copy_tree(package_tree("libzlib"), scratch, "zlib")
    run("make", "example", "minigzip", "examplesh", "minigzipsh", "example64", "minigzip64",
        cwd=tree, env=env)
    suite = Suite(out, "zlib", tree.name, supervisor)
    for program, name in (("example", "ex.bin"), ("minigzip", "mg.bin"),
                          ("examplesh", "exsh.bin"), ("minigzipsh", "mgsh.bin"),
                          ("example64", "ex64.bin"), ("minigzip64", "mg64.bin")):
        suite.fixture(name, source=tree / program, strip=True)
    suite.fixture("libz.so", source=(tree / "libz.so").resolve(), strip=True)
    suite.manifest["setup"].append("cp /mnt/sd/libz.so /tmp/libz.so.1")
    # zlib's "make test": a round trip through minigzip, then example, for
    # the static, shared and 64-bit-offset builds.
    suite.add_file("zlib.sh", text="""#!/bin/sh
S=/mnt/sd
r=0
t() {
	if echo hello world | env $4 $S/$1 | env $4 $S/$1 -d && env $4 $S/$2 tmp_$$; then
		echo "ZLIB $3 OK"
	else
		echo "ZLIB $3 FAILED"
		r=1
	fi
	rm -f tmp_$$
}
t mg.bin ex.bin static Z=
t mgsh.bin exsh.bin shared LD_LIBRARY_PATH=/tmp
t mg64.bin ex64.bin 64 Z=
exit $r
""")
    suite.case("zlib/test", "zlib.sh")
    suite.write()


def build_expat(out, scratch, supervisor, env):
    tree = copy_tree(package_tree("expat"), scratch, "expat")
    subprocess.run(["make", "distclean"], cwd=tree, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    run("./configure", f"--host={TARGET}", f"CC={TARGET}-gcc", f"CXX={TARGET}-g++",
        "--with-tests", "--without-docbook", "--without-examples", "--without-xmlwf",
        "--with-dev-urandom", cwd=tree, env=env)
    run("make", f"-j{os.environ.get('JOBS', '4')}", cwd=tree, env=env)
    # test_misc_input_2gb parses 2 GB, hours on this CPU.
    tests = tree / "tests/misc_tests.c"
    tests.write_text(tests.read_text().replace(
        "tcase_add_test(tc_misc, test_misc_input_2gb);\n", ""))
    run("make", "-C", "tests", "runtests", cwd=tree, env=env)
    suite = Suite(out, "expat", tree.name, supervisor)
    suite.add_file("rt.bin", source=tree / "tests/runtests", strip=True)
    suite.case("expat/runtests", "rt.bin", argv=["-v"])
    suite.write()


def meson_setup(tree, build, cross, options, env):
    command = ["meson", "setup", build, *options, "--buildtype=release",
               "-Ddefault_library=static"]
    if cross:
        command += ["--cross-file", cross]
    run(*command, cwd=tree, env=env)
    run("ninja", "-C", build, f"-j{os.environ.get('JOBS', '4')}", cwd=tree, env=env)


# Where the fontconfig fixture puts its test tree on the guest.
FONTCONFIG_GUEST = "tmp/fc-t"


def build_fontconfig(out, scratch, supervisor, env):
    source = package_tree("fontconfig")
    tree = copy_tree(source, scratch, "fontconfig")
    # test-bz89617 and test-bz106632 compile in the test directory's path
    # for 4x6.pcf; on the guest that is where fctest.tar unpacks it.
    tests_build = tree / "test/meson.build"
    text = tests_build.read_text()
    for macro in ("SRCDIR", "FONTFILE"):
        lines = [line for line in text.splitlines(True) if f"-D{macro}=" in line]
        if len(lines) != 1:
            raise SystemExit(f"fontconfig: expected one -D{macro} in {tests_build}")
        text = text.replace(lines[0], lines[0].replace(
            "meson.current_source_dir()", f"'/{FONTCONFIG_GUEST}/test'"))
    tests_build.write_text(text)
    # The tests call putenv, which -std=c11 leaves undeclared.
    meson_setup(tree, "tb", source / "buildroot-build/cross-compilation.conf",
                ["-Dtests=enabled", "-Ddoc=disabled", "-Dnls=disabled", "-Dcache-build=disabled",
                 "-Dc_args=-Os -D_GNU_SOURCE"], env)
    suite = Suite(out, "fontconfig", tree.name, supervisor)
    tests = json.loads((tree / "tb/meson-info/intro-tests.json").read_text())
    index = 0
    for test in tests:
        program = Path(test["cmd"][0])
        if any("run-test.sh" in part for part in test["cmd"]) or \
                not program.is_file() or program.read_bytes()[:4] != b"\x7fELF":
            continue
        index += 1
        name = f"f{index:02d}.bin"
        suite.add_file(name, source=program, strip=True)
        suite.case("fontconfig/" + test["name"], name)
    # run-test.sh runs the fc-* tools on the test fonts, from where the
    # fixture unpacks them.
    guest = FONTCONFIG_GUEST
    pack = scratch / "fcpack"
    shutil.rmtree(pack, ignore_errors=True)
    shutil.copytree(tree / "test", pack / guest / "test")
    for directory in [tree / "tb/test", *sorted((tree / "tb").glob("fc-*"))]:
        dest = pack / guest / "tb" / directory.name
        dest.mkdir(parents=True, exist_ok=True)
        for f in directory.iterdir():
            if f.is_file() and f.suffix != ".o":
                if os.access(f, os.X_OK) and f.read_bytes()[:4] == b"\x7fELF":
                    run(TOOLS / f"{TARGET}-strip", "-o", dest / f.name, f)
                else:
                    shutil.copy(f, dest / f.name)
    run("tar", "-cf", scratch / "fctest.tar", "-C", pack, "tmp")
    suite.fixture("fctest.tar", source=scratch / "fctest.tar")
    suite.manifest["setup"].append("tar -xf /mnt/sd/fctest.tar -C /")
    suite.add_file("rts.sh", text=f"""#!/bin/sh
exec env srcdir=/{guest}/test builddir=/{guest}/tb/test EXEEXT= VERBOSE=1 sh /{guest}/test/run-test.sh
""")
    suite.case("fontconfig/run_test_sh", "rts.sh")
    suite.write()


def build_pixman(out, scratch, supervisor, env):
    source = package_tree("pixman")
    tree = copy_tree(source, scratch, "pixman")
    alpha_loop = tree / "test/alpha-loop.c"
    text = alpha_loop.read_text()
    if text.count("fail_after (10,") != 1:
        raise SystemExit("pixman: alpha-loop's watchdog is not where it was")
    alpha_loop.write_text(text.replace("fail_after (10,", f"fail_after ({ALPHA_LOOP_SECONDS},"))
    options = ["-Dtests=enabled", "-Ddemos=disabled", "-Dgtk=disabled", "-Dlibpng=disabled",
               "-Dopenmp=disabled"]
    # The tests are written for a growable stack; the toolchain links 32 KB.
    meson_setup(tree, "tb", source / "buildroot-build/cross-compilation.conf",
                options + ["-Dc_link_args=-Wl,-z,stack-size=1048576"], env)
    # Their utilities guard buffers with mprotect when it links, and
    # without an MMU it fails, so every guarded buffer would be NULL.
    config = tree / "tb/pixman/pixman-config.h"
    config.write_text(config.read_text().replace("#define HAVE_MPROTECT 1\n", ""))
    run("ninja", "-C", "tb", cwd=tree, env=env)
    # musl takes PT_GNU_STACK's size for every thread's stack too, and
    # without an MMU each is one contiguous block: thread-test's 16
    # threads at 1 MB failed to start (it then joins the threads it never
    # made and hangs).  Its threads' largest frame is pixman-general.c's
    # 24 KB scanline buffer, so it is relinked with 128 KB.
    link = subprocess.run(["ninja", "-C", "tb", "-t", "commands", "test/thread-test"],
                          cwd=tree, env=env, check=True, capture_output=True,
                          text=True).stdout.splitlines()[-1]
    if "-z,stack-size=1048576" not in link:
        raise SystemExit(f"pixman: unexpected thread-test link: {link}")
    run("sh", "-c", link.replace("-z,stack-size=1048576", "-z,stack-size=131072"),
        cwd=tree / "tb", env=env)
    meson_setup(tree, "nb", None, options, dict(os.environ))
    suite = Suite(out, "pixman", tree.name, supervisor)
    suite.add_file("fuzz.sh", text="""#!/bin/sh
# A fuzzer over a range of iterations, against the checksum a native build
# of the same tests prints for that range.
out=$("/mnt/sd/$1" "$2" "$3") || exit $?
echo "$out"
case $out in *"checksum=$4"*) exit 0 ;; esac
echo "expected checksum=$4"
exit 1
""")
    tests = json.loads((tree / "tb/meson-info/intro-tests.json").read_text())
    index = 0
    for test in sorted(tests, key=lambda t: t["name"]):
        name = test["name"]
        if name in PIXMAN_SLOW:
            continue
        index += 1
        binary = f"p{index:02d}.bin"
        test_source = tree / "test" / (name + ".c")
        fuzzer = test_source.is_file() and "fuzzer_test_main" in test_source.read_text()
        if not fuzzer:
            suite.add_file(binary, source=tree / "tb/test" / name, strip=True)
            suite.case("pixman/" + name, binary)
            continue
        last = str(PIXMAN_ITERATIONS.get(name, 5000))
        printed = subprocess.run([tree / "nb/test" / name, "1", last], capture_output=True,
                                 text=True).stdout
        found = re.search(r"checksum=([0-9A-F]{8})", printed)
        if not found:
            raise SystemExit(f"no native checksum from {name}: {printed!r}")
        suite.fixture(binary, source=tree / "tb/test" / name, strip=True)
        suite.case("pixman/" + name, "fuzz.sh", argv=[binary, "1", last, found.group(1)])
    suite.write()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "linux/artifacts/package-suites")
    parser.add_argument("--scratch", type=Path, default=WORK / "package-suites")
    parser.add_argument("suites", nargs="*", default=["zlib", "expat", "fontconfig", "pixman"])
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    # Cross builds take Buildroot's host tools (its meson and pkg-config)
    # and the toolchain; the native pixman build, the system's.
    env = dict(os.environ)
    env["PATH"] = f"{BUILDROOT / 'host/bin'}:{TOOLS}:{env['PATH']}"
    supervisor = args.scratch / "super.bin"
    run(TOOLS / f"{TARGET}-gcc", "-std=gnu11", "-O2", ROOT / "linux/ltp/supervise.c",
        "-o", supervisor)
    builders = {"zlib": build_zlib, "expat": build_expat, "fontconfig": build_fontconfig,
                "pixman": build_pixman}
    for suite in args.suites:
        builders[suite](args.output, args.scratch, supervisor, env)


if __name__ == "__main__":
    main()
