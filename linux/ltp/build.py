#!/usr/bin/env python3
"""Cross-build a pinned, unchanged subset of LTP's Open POSIX tests on Linux."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


REVISION = "2279d708c817db657611a30c427c7d06c4360049"
ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path.home() / "wr-linux/ltp")
    parser.add_argument("--cc", default=str(Path.home() / "wr-linux/toolchain/install/bin/c33-linux-uclibc-gcc"))
    parser.add_argument("--output", type=Path, default=ROOT / "linux/artifacts/ltp")
    parser.add_argument("--all", action="store_true", help="Attempt every non-speculative standalone interface case")
    parser.add_argument("--list", type=Path, help="Build only the cases in this list")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--port-tests", action="store_true", help="Build the local C33 regressions, separately from upstream counts")
    args = parser.parse_args()
    source = args.source.resolve()
    if not source.exists():
        source.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "https://github.com/linux-test-project/ltp.git", str(source)], check=True)
        subprocess.run(["git", "-C", str(source), "checkout", "--detach", REVISION], check=True)
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if revision != REVISION or subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"]):
        raise SystemExit(f"LTP source must be clean at {REVISION}; existing trees are not reset automatically")
    suite = source / "testcases/open_posix_testsuite"
    args.output.mkdir(parents=True, exist_ok=True)
    interfaces = suite / "conformance/interfaces"
    local = {"c33_vfork_error/1-1": ["vfork-error.c", "vfork-error-entry.S", "vfork-checked.S"],
             "c33_clock_capability/1-1": ["clock-capability.c"],
             "c33_exec_arguments/1-1": ["exec-arguments.c"],
             "c33_timer_layout/1-1": ["timer-layout.c"],
             "c33_signal_mask/1-1": ["signal-mask.c"],
             "c33_large_frame_flags/1-1": ["large-frame-flags.c"],
             "c33_sd_stream_reopen/1-1": ["sd-stream-reopen.c"],
             "c33_nommu_contract/1-1": ["nommu-contract.c"]}
    if args.port_tests:
        cases = sorted(local)
    elif args.all:
        cases = sorted(str(p.relative_to(interfaces).with_suffix("")) for p in interfaces.glob("*/*.c")
                       if re.fullmatch(r"\d+-\d+\.c", p.name))
    else:
        cases = [line.strip() for line in (args.list or ROOT / "linux/ltp/smoke.list").read_text().splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
    manifest = {"revision": revision, "compiler": subprocess.check_output([args.cc, "--version"], text=True),
                "bootstrap_sha256": digest(suite / "lib/common.c"), "cases": [], "not_built": []}
    driver = Path(shutil.which(args.cc) or args.cc).resolve()
    cc1 = Path(subprocess.check_output([args.cc, "-print-prog-name=cc1"], text=True).strip()).resolve()
    manifest["compiler_files_sha256"] = {str(p): digest(p) for p in (driver, cc1)}
    if "c33_sd_stream_reopen/1-1" in cases:
        manifest["fixtures"] = []
        for name, multiplier, bias in (("sda.dat", 17, 7), ("sdb.dat", 37, 13)):
            fixture = args.output / name
            fixture.write_bytes(bytes((i * multiplier + bias) & 255 for i in range(128 * 1024)))
            manifest["fixtures"].append({"binary": name, "binary_sha256": digest(fixture)})
    supervisor = args.output / "super.bin"
    subprocess.run([args.cc, "-std=gnu11", "-O2", "-g", str(ROOT / "linux/ltp/supervise.c"),
                    "-o", str(supervisor)], check=True)
    manifest["supervisor"] = {"binary": supervisor.name, "binary_sha256": digest(supervisor),
                              "source_sha256": digest(ROOT / "linux/ltp/supervise.c")}
    (args.output / "build-logs").mkdir(exist_ok=True)

    def build_case(item):
        index, name = item
        if not re.fullmatch(r"[a-z0-9_]+/\d+-\d+", name):
            raise ValueError(f"Invalid case name: {name}")
        sources = [ROOT / "linux/ltp" / path for path in local[name]] if name in local else [interfaces / (name + ".c"), suite / "lib/common.c"]
        src = sources[0]
        binary = args.output / f"p{index:03d}.bin"
        # Match the suite's C99/feature-test macros; empty parameter lists
        # keep their pre-C23 meaning. Framework helpers are included by cases.
        command = [args.cc, "-std=c99", "-D_POSIX_C_SOURCE=200809L", "-D_XOPEN_SOURCE=700",
                   "-O2", "-g", "-pthread", "-I" + str(suite / "include"),
                   *map(str, sources), "-lrt", "-o", str(binary)]
        result = subprocess.run(command, capture_output=True, text=True)
        log = f"build-logs/p{index:03d}.txt"
        (args.output / log).write_text(result.stdout + result.stderr)
        record = {"name": name, "source_sha256": digest(src), "command": command, "build_log": log}
        record["source_kind"] = "port-regression" if name in local else "upstream"
        if name in local:
            dependencies = sources + ([ROOT / "linux/uclibc/overlay/libc/sysdeps/linux/c33/vfork.S"]
                                      if name == "c33_vfork_error/1-1" else [])
            if name == "c33_large_frame_flags/1-1":
                dependencies.append(ROOT / "host-tools/toolchain-c33/gcc/files/gcc/testsuite/gcc.target/c33/large-frame-flags-run.c")
            record["dependencies_sha256"] = {str(p.relative_to(ROOT)): digest(p) for p in dependencies}
        if result.returncode:
            record["status"] = "BUILD_ERROR"
            if (re.search(r"undefined reference to [`'](?:__)?fork['’]", result.stderr)
                    or "implicit declaration of function 'fork'" in result.stderr):
                record["status"] = "REQUIRES_FORK"
            return False, record
        record.update(binary=binary.name, binary_sha256=digest(binary))
        return True, record

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for count, (built, record) in enumerate(pool.map(build_case, enumerate(cases)), 1):
            manifest["cases" if built else "not_built"].append(record)
            if count % 100 == 0 or count == len(cases):
                print(f"Attempted {count}/{len(cases)}; built {len(manifest['cases'])}", flush=True)
    print(dict(Counter(c["status"] for c in manifest["not_built"])), flush=True)
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
