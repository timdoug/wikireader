#!/usr/bin/env python3
"""Build reviewed, unchanged LTP syscall bodies with the supervised harness."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shutil
import subprocess

from build import REVISION, ROOT, digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path.home() / "wr-linux/ltp")
    parser.add_argument("--work", type=Path, required=True, help="New private build directory; must not exist")
    parser.add_argument("--cc", required=True)
    parser.add_argument("--kernel-config", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, help="Required on C33: bind the configuration fixture to this kernel")
    parser.add_argument("--output", type=Path, default=ROOT / "linux/artifacts/ltp-syscalls")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--ordinary-harness", action="store_true", help="Native comparison with upstream fork workers")
    parser.add_argument("--harness-checks", action="store_true", help="Local positive/negative harness checks, separate from upstream tests")
    parser.add_argument("--stack-size", type=int, default=262144, help="ELF stack bytes for large upstream automatic buffers")
    args = parser.parse_args()
    if args.stack_size < 4096 or args.stack_size % 4096:
        parser.error("--stack-size must be a positive page multiple")
    if not args.ordinary_harness and not args.kernel:
        parser.error("C33 supervision requires --kernel")
    source, work, out = args.source.resolve(), args.work.resolve(), args.output.resolve()
    cc = str(Path(shutil.which(args.cc) or args.cc).resolve())
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if revision != REVISION or subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"]):
        raise SystemExit(f"LTP source must be clean at {REVISION}")
    if work.exists():
        raise SystemExit("Use a new private --work directory; existing builds are never reset")
    selection = ROOT / "linux/ltp/syscall-smoke.json"
    audit = json.loads(selection.read_text())
    if audit["revision"] != revision:
        raise SystemExit("Source audit revision mismatch")
    cases = audit["cases"]
    for case in cases:
        src = source / "testcases/kernel/syscalls" / (case["name"] + ".c")
        if digest(src) != case["source_sha256"]:
            raise SystemExit(f"Unreviewed source: {case['name']}")
    if args.harness_checks:
        if args.ordinary_harness:
            parser.error("Harness checks exercise the supervised mode")
        expected = ["PASS", "FAIL", "BROKEN", "UNSUPPORTED", "WARNING", "PARTIAL_PASS",
                    "BROKEN", "BROKEN", "TIMEOUT", "UNSUPPORTED"]
        cases = [{"name": f"harness/check{mode:02d}", "mode": mode,
                  "source_sha256": digest(ROOT / "linux/ltp/harness-checks.c"),
                  "expected_status": status} for mode, status in enumerate(expected)]
        # A deliberately killed worker goes last, so follow-on SD execution
        # cannot obscure the other harness checks on the emulator.
        cases.sort(key=lambda case: case["mode"] == 8)
    out.mkdir(parents=True, exist_ok=True)
    logs = out / "build-logs"
    logs.mkdir(exist_ok=True)
    harness = ROOT / "linux/ltp/supervised-harness.patch"
    guard = ROOT / "linux/ltp/supervised-fork.h"
    flags = ["-O2", "-g", "-std=gnu11", "-D_GNU_SOURCE"]
    if not args.ordinary_harness:
        flags += ["-DTST_NOMMU_SUPERVISED", "-include", str(guard)]

    def checked(command, name, cwd=None):
        with (logs / name).open("w") as log:
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)

    checked(["git", "clone", "--shared", str(source), str(work)], "clone.txt")
    checked(["git", "apply", str(harness)], "harness.txt", work)
    checked(["make", "autotools"], "autotools.txt", work)
    target = subprocess.check_output([cc, "-dumpmachine"], text=True).strip()
    ar = subprocess.check_output([cc, "-print-prog-name=ar"], text=True).strip()
    ranlib = subprocess.check_output([cc, "-print-prog-name=ranlib"], text=True).strip()
    checked([str(work / "configure"), "--host=" + target, "CC=" + cc,
             "AR=" + ar, "RANLIB=" + ranlib], "configure.txt", work)
    checked(["make", "-C", str(work / "lib"), f"-j{args.jobs}",
             "CFLAGS=" + " ".join(flags + ["-DLTPLIB", "-I."]),
             "RANLIB=" + ranlib, "libltp.a"], "library.txt")
    supervisor = out / "super.bin"
    checked([cc, "-O2", "-std=gnu11", str(ROOT / "linux/ltp/supervise.c"),
             "-o", str(supervisor)], "supervisor.txt")
    shutil.copyfile(args.kernel_config, out / "kconf.txt")
    library = work / "lib/libltp.a"
    manifest = {
        "revision": revision, "result_format": "ltp",
        "harness_mode": "ordinary-fork" if args.ordinary_harness else "supervised-no-MMU",
        "stack_size": args.stack_size,
        "scope": "Unchanged reviewed syscall bodies; adapted supervised harness, separate from the POSIX interface cohort.",
        "compiler": subprocess.check_output([cc, "--version"], text=True),
        "compiler_files_sha256": {cc: digest(Path(cc)),
            "cc1": digest(Path(subprocess.check_output([cc, "-print-prog-name=cc1"], text=True).strip()))},
        "selection_sha256": digest(selection),
        "harness_sha256": {str(p.relative_to(ROOT)): digest(p) for p in
            (harness, guard, ROOT / "linux/ltp/supervised-fork.c", ROOT / "linux/ltp/supervise.c")},
        "upstream_harness_sha256": digest(source / "lib/tst_test.c"),
        "built_harness_sha256": digest(work / "lib/tst_test.c"),
        "config_sha256": digest(work / "include/config.h"),
        "library_sha256": digest(library),
        "fixtures": [{"binary": "kconf.txt", "binary_sha256": digest(out / "kconf.txt")}],
        "supervisor": {"binary": supervisor.name, "binary_sha256": digest(supervisor)},
        "cases": [], "not_built": []}
    if args.ordinary_harness:
        manifest["scope"] = "Unchanged reviewed syscall bodies under the original native fork-based harness."
        manifest["native_kernel"] = subprocess.check_output(["uname", "-srvm"], text=True).strip()
    else:
        manifest["kernel_sha256"] = digest(args.kernel)
    if args.harness_checks:
        manifest["scope"] = "Local harness validation; intentional negative outcomes, not upstream passes."
        manifest["harness_sha256"]["linux/ltp/harness-checks.c"] = digest(ROOT / "linux/ltp/harness-checks.c")

    def build_case(item):
        index, case = item
        src = (ROOT / "linux/ltp/harness-checks.c" if args.harness_checks else
               work / "testcases/kernel/syscalls" / (case["name"] + ".c"))
        if digest(src) != case["source_sha256"]:
            raise ValueError("Private tree changed a test body")
        binary = out / f"p{index:03d}.bin"
        command = [cc, *flags, "-I" + str(work / "include"),
                   "-I" + str(work / "testcases/kernel/syscalls/utils"), str(src),
                   str(ROOT / "linux/ltp/supervised-fork.c"), str(library),
                   "-pthread", "-lrt", "-lm", f"-Wl,-z,stack-size={args.stack_size}", "-o", str(binary)]
        if args.harness_checks:
            command.insert(1, f"-DCHECK_MODE={case['mode']}")
        result = subprocess.run(command, capture_output=True, text=True)
        log = f"build-logs/p{index:03d}.txt"
        (out / log).write_text(result.stdout + result.stderr)
        record = {**case, "source_kind": "harness-validation" if args.harness_checks else
                  "upstream-body-adapted-harness", "command": command, "build_log": log}
        if result.returncode:
            record["status"] = "BUILD_ERROR"
        else:
            record.update(binary=binary.name, binary_sha256=digest(binary))
        return record

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for record in pool.map(build_case, enumerate(cases)):
            manifest["not_built" if "status" in record else "cases"].append(record)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Built {len(manifest['cases'])}/{len(cases)}; {len(manifest['not_built'])} build errors", flush=True)


if __name__ == "__main__":
    main()
