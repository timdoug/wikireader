#!/usr/bin/env python3
"""Run upstream library/applet suites on disposable Grifo-booted virtual cards."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tarfile
import time

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verdict(suite, case, code, segment):
    if suite == "uclibc":
        assertions = Counter(re.findall(r"^(PASS|FAIL|SKIP) \S+", segment, re.M))
    elif case["name"].startswith("hush/"):
        assertions = Counter("PASS" if x == "ok" else "SKIP" if x.startswith("skip") else "FAIL"
                             for x in re.findall(r"^(?:-n )?hush-[^:\n]+:[ \n]*(ok|skip[^\n]*|fail[^\n]*)$", segment, re.M))
    else:
        assertions = Counter(re.findall(r"^(PASS|FAIL|SKIPPED|UNTESTED):", segment, re.M))
    passed = assertions["PASS"]
    failed = assertions["FAIL"]
    skipped = sum(assertions[x] for x in ("SKIP", "SKIPPED", "UNTESTED"))
    if code is None:
        status = "MISSING"
    elif code == 124:
        status = "TIMEOUT"
    elif code >= 128 or code in (125, 127):
        status = "ERROR"
    elif code or failed:
        status = "FAIL"
    elif not passed:
        status = "SKIPPED" if skipped else "UNTESTED"
    else:
        status = "PARTIAL_PASS" if skipped else "PASS"
    return {"status": status, "assertions": dict(assertions)}


def crashed(raw, case_name):
    """An entered test ended the emulator without returning a result."""
    entered = re.search(r"^UPSTREAM-BEGIN " + re.escape(case_name) + r"\s*$",
                        raw.replace("\r", "\n"), re.M) is not None
    fault = ("stop reason: runaway" in raw or "Kernel panic" in raw or
             ("stop reason: powered off" in raw and "unmapped write" in raw))
    return entered and fault


def hush_cases(archive_path):
    with tarfile.open(archive_path) as archive:
        members = archive.getmembers()
        names = {x.name for x in members}
        prefix = "busybox/shell/hush_test/"
        return [{"name": "hush/" + x.name.removeprefix(prefix),
                 "directory": "busybox/shell/hush_test", "status": "BUILT",
                 "hush_test": x.name.removeprefix(prefix),
                 "command": "sh ./run-all " + shlex.quote(Path(x.name).parent.name)}
                for x in members if x.isfile() and x.name.startswith(prefix + "hush-")
                and x.name.endswith(".tests") and x.mode & 0o111
                and x.name[:-6] + ".right" in names]


def trial(args, manifest, cases, out):
    out.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location("wr_app", ROOT / "linux/app-test.py")
    app = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(app)
    fat = app.load_fat_helper(ROOT)
    # Stage only the requested families, keeping RAM available for the tests.
    selected = {x["directory"] for x in cases}
    with tarfile.open(args.binaries / "suite.tar.gz") as archive, tarfile.open(out / "tests.tar", "w") as packed:
        for member in archive:
            keep = member.name.startswith("busybox/") if args.suite == "busybox" else (
                member.name in {"uclibc/test/uclibcng-testrunner.sh"} or
                any(member.name == directory or member.name.startswith(directory + "/") for directory in selected))
            if keep:
                packed.addfile(member, archive.extractfile(member) if member.isfile() else None)
    files = {name: path.read_bytes() for name, path in {
        "kernel.elf": ROOT / "samo-lib/grifo/grifo.elf",
        "init.app": ROOT / "samo-lib/grifo/applications/init/init.app",
        "linux.ico": ROOT / "linux/artifacts/linux.ico",
        "linux.app": args.kernel, "linux.img": args.rootfs,
        "super.bin": args.binaries / "super.bin", "tests.tar": out / "tests.tar",
    }.items()}
    commands = ["#!/bin/sh", "ulimit -c 0", "export LC_ALL=C LANG=C TZ=UTC0",
                "mkdir -p /tmp/upstream", "cd /tmp/upstream || exit 1",
                "tar -xf /mnt/sd/tests.tar || exit 1"]
    if args.suite == "uclibc":
        # Both ethers tests explicitly require this manually supplied fixture.
        commands += ["printf '00:11:22:33:44:55 teeth\\n' >/tmp/upstream/ethers",
                     "ln -sf /tmp/upstream/ethers /etc/ethers"]
    for index, case in enumerate(cases):
        if args.suite == "uclibc":
            row = case["upstream_row"] + (" -d" if args.direct else "")
            script = ("#!/bin/sh\ncd /tmp/upstream/uclibc/test || exit 125\n"
                      "printf '%s\\n' " + shlex.quote(row) +
                      " > uclibcng-testrunner.in\nexec sh ./uclibcng-testrunner.sh\n")
        else:
            selection = ""
            if case.get("hush_test"):
                module = str(Path(case["hush_test"]).parent)
                selection = ("chmod -x " + shlex.quote(module) + "/*.tests\n" +
                             "chmod +x " + shlex.quote(case["hush_test"]) + " || exit 125\n")
            script = ("#!/bin/sh\ncd /tmp/upstream/" + case["directory"] +
                      " || exit 125\n" + selection + case["command"] + "\n")
            if case["name"].startswith("hush/"):
                script += ("status=$?\nfor file in *.fail; do\n"
                           "test -f \"$file\" || continue\n"
                           "echo UPSTREAM-DIFF \"$file\"\nsed -n '1,100p' \"$file\"\n"
                           "rm -f \"$file\"\ndone\nexit $status\n")
        name = f"t{index:03d}.sh"
        files[name] = script.encode()
        commands += [f"echo UPSTREAM-BEGIN {case['name']}",
                     f"/mnt/sd/super.bin {args.timeout} /bin/sh /mnt/sd/{name} </dev/null",
                     "status=$?", f"echo UPSTREAM-RESULT {case['name']} $status"]
    commands += ["echo UPSTREAM-DONE", "reboot -f"]
    files["run.sh"] = ("\n".join(commands) + "\n").encode()
    files["init.ini"] = b"linux.ico : linux.app loglevel=7\n" * 2
    card, flash = out / "card.img", out / "flash.rom"
    size = 64
    while sum(map(len, files.values())) > size * 1024 * 1024 * .8:
        size *= 2
    fat.make_image(card, files, size)
    subprocess.run(["python3", str(ROOT / "samo-lib/mbr/make-flash.py"), str(flash)],
                   check=True, stdout=subprocess.DEVNULL)
    (out / "uart.in").write_text("sh /mnt/sd/run.sh\n")
    command = [str(ROOT / "emulator/wremu"), "-n", "1000000000000", "-T", "40,36,100000000",
               "--uart-input", str(out / "uart.in"), "--uart-start", "850000000",
               "--uart-gap", "200000", "-c", str(card), "-e", str(flash)]
    host_error = None
    with (out / "boot.log").open("w") as log:
        process = subprocess.Popen(command, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                                   env={**os.environ, "WREMU_HOLD_MS": "33"})
        deadline = time.monotonic() + args.wall_timeout
        try:
            while process.poll() is None:
                text = (out / "boot.log").read_text(errors="replace")
                finish = text.find("UPSTREAM-DONE")
                reset = text.find("watchdog reset", finish) if finish >= 0 else -1
                if reset >= 0 and "init choosing" in text[reset:]:
                    process.terminate()
                    break
                if "Kernel panic" in text:
                    host_error = "Kernel panic"
                    process.terminate()
                    break
                if time.monotonic() >= deadline:
                    host_error = "Host deadline expired"
                    process.terminate()
                    break
                time.sleep(.5)
            process.wait(timeout=5)
            if process.returncode not in (0, -15):
                host_error = f"Emulator exit {process.returncode}"
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    raw = (out / "boot.log").read_text(errors="replace")
    text = re.sub(r" *\[[a-z][^\]\n]*\]\n?", "", raw.replace("\r", "\n"))
    results = []
    for case in cases:
        start = re.search(r"^UPSTREAM-BEGIN " + re.escape(case["name"]) + r"\s*$", text, re.M)
        ends = list(re.finditer(r"^UPSTREAM-RESULT " + re.escape(case["name"]) + r" (\d+)\s*$", text, re.M))
        code = int(ends[0][1]) if len(ends) == 1 else None
        segment = text[start.end():ends[0].start()] if start and len(ends) == 1 else text[start.end():] if start else ""
        results.append({"name": case["name"], "exit_status": code,
                        **verdict(args.suite, case, code, segment)})
        if segment:
            (out / (case["name"].replace("/", "_") + ".log")).write_text(segment)
    complete = (not host_error and "UPSTREAM-DONE" in text and "watchdog reset" in raw and
                "C33 boot: Grifo application" in text and all(x["exit_status"] is not None for x in results))
    report = {"suite": args.suite, "complete": complete, "host_error": host_error,
              "command": command, "guest_timeout": args.timeout,
              "fixture_sha256": {k: hashlib.sha256(v).hexdigest() for k, v in files.items()},
              "emulator_sha256": sha(ROOT / "emulator/wremu"),
              "environment": {k: v for k, v in os.environ.items() if k.startswith("WREMU_")},
              "results": results}
    (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    # Keep logs and exact fixture hashes; disposable card copies are large.
    card.unlink()
    (out / "tests.tar").unlink()
    print(out.name, dict(Counter(x["status"] for x in results)), flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suite", choices=["busybox", "uclibc"])
    parser.add_argument("--binaries", type=Path, default=ROOT / "linux/artifacts/upstream-suites")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, default=ROOT / "linux/artifacts/linux.app")
    parser.add_argument("--rootfs", type=Path, default=ROOT / "linux/artifacts/linux.img")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--wall-timeout", type=int, default=1200)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--case", action="append")
    parser.add_argument("--hush-case", action="append", help="Select an individual upstream Hush test, e.g. hush-misc/func7.tests")
    parser.add_argument("--direct", action="store_true", help="Use the upstream test-skeleton -d mode (selected libc tests only)")
    args = parser.parse_args()
    if min(args.timeout, args.wall_timeout, args.batch_size, args.jobs) <= 0:
        raise SystemExit("Deadlines, batch size and job count must be positive")
    for name in ["binaries", "output", "kernel", "rootfs"]:
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((args.binaries / "manifest.json").read_text())
    for name, field in [("suite.tar.gz", "archive_sha256"), ("super.bin", "supervisor_sha256")]:
        if sha(args.binaries / name) != manifest[field]:
            raise SystemExit("Build fixture hash mismatch")
    cases = manifest[args.suite]
    if args.hush_case:
        if args.suite != "busybox" or args.case:
            raise SystemExit("Hush selection requires BusyBox and cannot be combined with --case")
        names = {"hush/" + x for x in args.hush_case}
        cases = [x for x in hush_cases(args.binaries / "suite.tar.gz") if x["name"] in names]
        if len(cases) != len(names):
            raise SystemExit("Unknown selected Hush test")
    if args.case:
        cases = [c for c in cases if c["name"] in args.case]
        if len(cases) != len(set(args.case)):
            raise SystemExit("Unknown selected case")
    if args.direct:
        if args.suite != "uclibc" or not args.case:
            raise SystemExit("Direct mode requires explicitly selected libc tests")
        with tarfile.open(args.binaries / "suite.tar.gz") as archive:
            for case in cases:
                source = archive.extractfile("uclibc/test/" + case["name"] + ".c")
                if source is None or b'test-skeleton.c' not in source.read():
                    raise SystemExit("Direct mode requires the upstream test skeleton")
    results = [{"name": c["name"], "status": "HOST_ONLY" if c.get("host_only") else c["status"]}
               for c in cases if c["status"] != "BUILT" or c.get("host_only")]
    selected = [c for c in cases if c["status"] == "BUILT" and not c.get("host_only")]
    reports = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(trial, args, manifest, selected[i:i + args.batch_size],
                               args.output / f"batch-{i:04d}")
                   for i in range(0, len(selected), args.batch_size)]
        for future in as_completed(futures):
            reports.append(future.result())
    observed = {x["name"]: x for report in reports for x in report["results"]}
    # A stalled/crashed batch must not prevent later cases from being attempted.
    for index, case in enumerate(selected):
        if observed[case["name"]]["status"] == "MISSING":
            report = trial(args, manifest, [case], args.output / f"retry-{index:04d}")
            reports.append(report)
            result = {**report["results"][0], "retry": True}
            raw = (args.output / f"retry-{index:04d}" / "boot.log").read_text(errors="replace")
            if result["status"] == "MISSING" and crashed(raw, case["name"]):
                result["status"] = "CRASH"
            observed[case["name"]] = result
    results += list(observed.values())
    report = {"suite": args.suite, "manifest_sha256": sha(args.binaries / "manifest.json"),
              "cases": len(cases), "direct_mode": args.direct, "counts": dict(Counter(x["status"] for x in results)),
              "complete": all(x["status"] != "MISSING" for x in results),
              "clean_trials": all(x["complete"] for x in reports),
              "results": sorted(results, key=lambda x: x["name"]), "batches": reports}
    (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(args.suite, report["counts"], "complete:", report["complete"], flush=True)
    bad = {"FAIL", "ERROR", "TIMEOUT", "MISSING", "CRASH", "BUILD_ERROR"}
    return 0 if report["complete"] and not any(x["status"] in bad for x in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
